# Meshtastic Firmware Patches for I2C Bridge Support

This document describes the exact changes needed to add PineDio LoRa backplate
(I2C bridge) support to the meshtastic firmware. These modifications were tested
against meshtastic firmware version 2.7.7.

## Overview

Four files need modification, plus one new file:

| File | Action |
|---|---|
| `src/platform/portduino/I2CBridgeHal.h` | **NEW** (copy from this repo) |
| `src/platform/portduino/PortduinoGlue.h` | Add include, extern, config fields |
| `src/platform/portduino/PortduinoGlue.cpp` | Add HAL init, config parsing, cleanup handlers |
| `src/mesh/RadioInterface.cpp` | Add HAL selection for i2c-bridge |
| `src/main.cpp` | Add I2C bridge error detection and recovery |

## Step 1: Copy I2CBridgeHal.h

Copy `I2CBridgeHal.h` from this repo's `meshtastic/` directory to
`src/platform/portduino/I2CBridgeHal.h` in the meshtastic firmware tree.

```bash
cp meshtastic/I2CBridgeHal.h ~/meshtastic-firmware/src/platform/portduino/
```

## Step 2: Modify PortduinoGlue.h

### Add include (near the top, with other includes)

```cpp
#include "platform/portduino/I2CBridgeHal.h"
```

### Add extern declaration (near other extern declarations)

```cpp
extern I2CBridgeHal *i2cBridgeHal;
```

### Add config fields (in the portduino config struct, near other lora fields)

```cpp
std::string lora_i2c_device = "/dev/i2c-5";
int lora_i2c_address = 0x28;
int lora_irq_gpio = -1;
```

## Step 3: Modify PortduinoGlue.cpp

### Add global variable (near other global declarations at file scope)

```cpp
I2CBridgeHal *i2cBridgeHal = nullptr;
```

### Add cleanup function and signal handler (before any function definitions)

```cpp
// Cleanup handler to safely shut down the I2C bridge HAL on exit/signal.
// Without this, the ATtiny84's USI state machine hangs if meshtasticd exits
// mid-I2C-transaction, requiring a full power cycle to recover.
static void i2cBridgeCleanup()
{
    if (i2cBridgeHal) {
        i2cBridgeHal->term();
        i2cBridgeHal = nullptr; // prevent double cleanup via atexit + signal handler
    }
}

static void signalHandler(int sig)
{
    i2cBridgeCleanup();
    // Re-raise with default handler to get proper exit status
    signal(sig, SIG_DFL);
    raise(sig);
}
```

### Add I2C bridge HAL initialization (in the HAL setup section, after the CH341 `else if` block)

Find the section that handles `spidev` configuration (look for `ch341`). Add an
`else if` block after the CH341 one:

```cpp
} else if (portduino_config.lora_spi_dev == "i2c-bridge") {
    try {
        i2cBridgeHal = new I2CBridgeHal(portduino_config.lora_i2c_device,
                                        portduino_config.lora_i2c_address,
                                        portduino_config.lora_irq_gpio);
    } catch (std::exception &e) {
        std::cerr << e.what() << std::endl;
        std::cerr << "Could not initialize I2C bridge device!" << std::endl;
        exit(EXIT_FAILURE);
    }
    std::cout << "I2C Bridge LoRa on " << portduino_config.lora_i2c_device
              << " addr 0x" << std::hex << portduino_config.lora_i2c_address << std::dec << std::endl;
    // Register cleanup handlers so the ATtiny84 I2C bridge is safely
    // shut down on exit or signal, preventing USI state machine hang.
    std::atexit(i2cBridgeCleanup);
    signal(SIGTERM, signalHandler);
    signal(SIGINT, signalHandler);
}
```

### Add YAML config parsing (in the YAML parsing section, after spidev is read)

Find where `lora_spi_dev` (`spidev`) is read from the YAML config. Add after it:

```cpp
if (portduino_config.lora_spi_dev == "i2c-bridge") {
    portduino_config.lora_i2c_device = yamlConfig["Lora"]["I2CDevice"].as<std::string>("/dev/i2c-5");
    portduino_config.lora_i2c_address = yamlConfig["Lora"]["I2CAddress"].as<int>(0x28);
    portduino_config.lora_irq_gpio = yamlConfig["Lora"]["IRQ_GPIO"].as<int>(-1);
}
```

### Add GPIO skip logic (in GPIO initialization loops)

Find the GPIO initialization loops that iterate over configured pins. There are
two loops (one for `settingsMap` entries, one for `gpioMapping` entries). In
each, add a `continue` for I2C bridge, similar to the existing CH341 skip:

```cpp
// In both GPIO init loops, change:
if (i->config_section == "Lora" && portduino_config.lora_spi_dev == "ch341") {
    continue;
}
// To:
if (i->config_section == "Lora" && (portduino_config.lora_spi_dev == "ch341" || portduino_config.lora_spi_dev == "i2c-bridge")) {
    continue;
}
```

### Skip SPI init for I2C bridge

Find where the SPI device is initialized (the condition checking
`lora_spi_dev != ""` and `!= "ch341"`). Add `i2c-bridge` to the exclusion:

```cpp
// Change:
if (portduino_config.lora_spi_dev != "" && portduino_config.lora_spi_dev != "ch341") {
// To:
if (portduino_config.lora_spi_dev != "" && portduino_config.lora_spi_dev != "ch341" && portduino_config.lora_spi_dev != "i2c-bridge") {
```

## Step 4: Modify RadioInterface.cpp

### Add HAL selection (in the HAL selection section, after the CH341 case)

Find where `RadioLibHAL` is assigned based on `lora_spi_dev`. Add after the
CH341 case:

```cpp
} else if (portduino_config.lora_spi_dev == "i2c-bridge") {
    RadioLibHAL = i2cBridgeHal;
```

## Step 5: Modify main.cpp

### Add I2C bridge error detection and recovery (in the main loop)

Find the main loop where `portduino_status.LoRa_in_error` is checked. Add I2C
bridge error detection before the existing error handler:

```cpp
// Add before the LoRa_in_error check:
if (portduino_config.lora_spi_dev == "i2c-bridge" && i2cBridgeHal != nullptr && i2cBridgeHal->isInError()) {
    portduino_status.LoRa_in_error = true;
}
```

Then, inside the existing `if (portduino_status.LoRa_in_error && rebootAtMsec == 0)`
block, add an `else if` for the I2C bridge after the CH341 recovery code:

```cpp
} else if (portduino_config.lora_spi_dev == "i2c-bridge") {
    if (i2cBridgeHal != nullptr) {
        extern RadioLibHal *RadioLibHAL;
        RadioLibHAL = nullptr; // prevent use-after-free during sleep
        delete i2cBridgeHal;
        i2cBridgeHal = nullptr;
        sleep(3);
    }
    try {
        i2cBridgeHal = new I2CBridgeHal(portduino_config.lora_i2c_device,
                                        portduino_config.lora_i2c_address,
                                        portduino_config.lora_irq_gpio);
    } catch (std::exception &e) {
        LOG_ERROR("Could not reinitialize I2C bridge: %s", e.what());
        LOG_WARN("I2C bridge recovery failed, exiting for systemd restart");
        rebootAtMsec = millis() + 25;
        return;
    }
}
```

### Add CPU busy-loop fix (at the end of the main loop)

Find the end of the `loop()` function, after `mainDelay.delay(delayMsec);` and
the closing brace of the `while` loop. Add a sleep to prevent 100% CPU usage:

```cpp
#ifdef ARCH_PORTDUINO
    // On Linux, enforce a minimum sleep to prevent busy-looping.
    // The cooperative scheduler can return delayMsec=0 (or the
    // InterruptableDelay may wake immediately), causing 100% CPU
    // which starves the API server from processing TCP connections.
    // 10ms cap is plenty fast for LoRa (packets take 50-500ms on air).
    usleep(10000);
#endif
```

Without this, meshtasticd uses 100% CPU on Linux.

## Build

After applying all patches:

```bash
cd ~/meshtastic-firmware
python3 -m venv venv && source venv/bin/activate
pip install platformio
pio pkg install --environment native
pio run --environment native    # ~15-18 minutes on PinePhone Pro
```

The binary will be at `.pio/build/native/meshtasticd`.
