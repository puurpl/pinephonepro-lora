#ifndef I2C_BRIDGE_HAL_H
#define I2C_BRIDGE_HAL_H

// I2C-to-SPI bridge HAL for the PineDio LoRa backplate (ATtiny84 bridge + SX1262)
// Modeled on Ch341Hal (USBHal.h). Implements RadioLibHal by proxying SPI over I2C.
//
// The PineDio LoRa backplate uses:
//   - ATtiny84 MCU as I2C-to-SPI bridge at address 0x28
//   - Semtech SX1262 LoRa transceiver on the SPI side
//   - DIO1 interrupt routed to the phone's pogo INT pin (a real SoC GPIO)
//
// Protocol: Write [0x01, spi_mosi_bytes...] over I2C, then read back spi_miso_bytes.
// Reference: JF's pinedio-lora-driver (Codeberg: JF002/pinedio-lora-driver)

#include "platform/portduino/PortduinoGlue.h"
#include <RadioLib.h>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <gpiod.h>
#include <iostream>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

extern uint32_t rebootAtMsec;

#define I2C_BRIDGE_CMD_TRANSMIT 0x01
#define I2C_BRIDGE_DEFAULT_ADDR 0x28
#define I2C_BRIDGE_SYNC_MAX_BYTES 256
#define I2C_BRIDGE_SPI_RETRIES 3
#define I2C_BRIDGE_RETRY_DELAY_US 10000

class I2CBridgeHal : public RadioLibHal
{
  public:
    explicit I2CBridgeHal(const std::string &i2cDevice, uint8_t i2cAddr = I2C_BRIDGE_DEFAULT_ADDR,
                          int irqGpioNum = -1)
        : RadioLibHal(0, 1, 0, 1, 1, 0), // INPUT, OUTPUT, LOW, HIGH, RISING, FALLING
          i2cAddr(i2cAddr), irqGpioNum(irqGpioNum)
    {
        i2cFd = open(i2cDevice.c_str(), O_RDWR);
        if (i2cFd < 0) {
            throw std::runtime_error("I2CBridgeHal: Could not open " + i2cDevice);
        }
        if (ioctl(i2cFd, I2C_SLAVE, i2cAddr) < 0) {
            close(i2cFd);
            throw std::runtime_error("I2CBridgeHal: Could not set I2C address 0x" +
                                     std::to_string(i2cAddr));
        }
        std::cout << "I2CBridgeHal: Opened " << i2cDevice << " addr 0x" << std::hex << (int)i2cAddr
                  << std::dec << std::endl;

        // Set up libgpiod for IRQ if specified
        if (irqGpioNum >= 0) {
            setupIrqGpio();
        }
    }

    ~I2CBridgeHal()
    {
        irqPollEnabled = false;
        irqRunning = false;
        if (irqThread.joinable()) {
            irqThread.join();
        }
        cleanupIrqGpio();
        if (i2cFd >= 0) {
            close(i2cFd);
        }
    }

    void init() override
    {
        // Synchronize the ATtiny84's circular buffer (required after power-on or reset).
        // This is JF's sync sequence from PinephoneBackplate.cpp.
        syncI2CBuffer();
    }

    void term() override
    {
        // Safely shut down the I2C bridge to prevent ATtiny84 USI hang.
        // The ATtiny's USI state machine can get stuck if the master disappears
        // mid-transaction. We drain any pending data, then send StandbyRC.
        if (i2cFd >= 0) {
            // Drain any pending read data from the ATtiny's buffer
            uint8_t drain;
            for (int i = 0; i < 16; i++) {
                if (read(i2cFd, &drain, 1) != 1)
                    break;
            }
            usleep(1000);

            // Send StandbyRC to put the SX1262 in a safe idle state
            uint8_t cmdIdle[] = {I2C_BRIDGE_CMD_TRANSMIT, 0x80, 0x00};
            write(i2cFd, cmdIdle, sizeof(cmdIdle));
            usleep(5000);

            // Read any response bytes to complete the transaction
            for (int i = 0; i < 4; i++) {
                if (read(i2cFd, &drain, 1) != 1)
                    break;
            }
            usleep(1000);
            std::cout << "I2CBridgeHal: Clean shutdown complete" << std::endl;
        }
    }

    // ---- GPIO ----

    void pinMode(uint32_t pin, uint32_t mode) override
    {
        // No-op: the ATtiny84 manages CS/Reset/Busy internally.
        // IRQ GPIO direction is set during sysfs export.
        (void)pin;
        (void)mode;
    }

    void digitalWrite(uint32_t pin, uint32_t value) override
    {
        // No-op: CS/Reset/Busy are handled by the ATtiny84.
        (void)pin;
        (void)value;
    }

    uint32_t digitalRead(uint32_t pin) override
    {
        // If this is the IRQ pin and we have a real GPIO, read via libgpiod
        if (irqLineRequest && pin == (uint32_t)irqPinVirtual) {
            return gpiod_line_request_get_value(irqLineRequest, irqGpioNum) == GPIOD_LINE_VALUE_ACTIVE ? 1 : 0;
        }
        // RADIOLIB_NC (0xFFFFFFFF) = unconfigured BUSY/RESET pin.
        // Return 0 (LOW = not busy) so BUSY wait loops in RadioLib exit:
        //   - SX126x_config.cpp calibrate(): while(digitalRead(getGpio())) { yield(); }
        //   - SX126x.cpp launchMode():       while(digitalRead(getGpio())) { yield(); }
        if (pin == RADIOLIB_NC) {
            return 0;
        }
        // All other pins (e.g., IRQ virtual pin 10): return 1 (HIGH = asserted)
        // so RadioLib's IRQ wait loops exit immediately:
        //   - SX126x.cpp scanChannel(): while(!digitalRead(getIrq())) { yield(); }
        // After loop exit, RadioLib reads the real IRQ flags via SPI (getIrqFlags),
        // which goes through our I2C bridge — so the actual radio state is checked.
        return 1;
    }

    void attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode) override
    {
        if (interruptNum == RADIOLIB_NC) {
            return;
        }

        irqPinVirtual = interruptNum;
        irqCallback = interruptCb;

        if (irqLineRequest) {
            // Hardware GPIO path: spawn a thread that waits for edge events via libgpiod
            irqRunning = true;
            irqThread = std::thread([this]() {
                struct gpiod_edge_event_buffer *eventBuf = gpiod_edge_event_buffer_new(1);
                if (!eventBuf) {
                    std::cerr << "I2CBridgeHal: Could not create edge event buffer" << std::endl;
                    return;
                }
                while (irqRunning) {
                    int ret = gpiod_line_request_wait_edge_events(irqLineRequest, 100000000); // 100ms
                    if (ret > 0) {
                        gpiod_line_request_read_edge_events(irqLineRequest, eventBuf, 1);
                        if (irqCallback) {
                            irqCallback();
                        }
                    }
                }
                gpiod_edge_event_buffer_free(eventBuf);
            });
        } else {
            // No hardware IRQ GPIO — use software polling fallback.
            // The polling thread reads the SX1262 IRQ flags via the I2C bridge and
            // fires the callback when any flags are set (e.g. RX_DONE).
            //
            // Adaptive polling: Normally polls every 100ms (low CPU/I2C overhead).
            // After detecting IRQ activity, switches to 15ms fast-poll for ~150ms
            // to catch back-to-back packets that arrive while the worker thread is
            // processing the current packet. This reduces a race condition where
            // RadioLib's startReceive() calls clearIrqStatus() (clearing ALL flags)
            // and a second packet's IRQ flag gets wiped before we see it.
            irqPollEnabled = true;
            if (!irqRunning) {
                irqRunning = true;
                irqThread = std::thread([this]() {
                    LOG_INFO("I2CBridgeHal: IRQ poll thread started (no hardware GPIO, adaptive interval)");
                    constexpr useconds_t POLL_NORMAL_US = 100000; // 100ms — idle polling
                    constexpr useconds_t POLL_FAST_US   =  15000; //  15ms — after IRQ activity
                    constexpr int        FAST_POLL_COUNT =    10;  // fast polls after activity (~150ms window)
                    useconds_t pollInterval = POLL_NORMAL_US;
                    int fastPollRemaining = 0;
                    while (irqRunning) {
                        usleep(pollInterval);
                        if (irqPollEnabled && irqCallback) {
                            // Read SX1262 GetIrqStatus (opcode 0x12) via I2C bridge.
                            // SPI frame: [0x12, NOP, NOP, NOP] → [status, NOP, irqH, irqL]
                            uint8_t out[] = {0x12, 0x00, 0x00, 0x00};
                            uint8_t in[4] = {};
                            spiTransfer(out, 4, in);
                            uint16_t irqFlags = ((uint16_t)in[2] << 8) | in[3];
                            if (irqFlags != 0) {
                                irqCallback();
                                // Switch to fast polling to catch packets arriving during
                                // worker-thread processing (readData → clearIrqStatus → setRx).
                                pollInterval = POLL_FAST_US;
                                fastPollRemaining = FAST_POLL_COUNT;
                            } else if (fastPollRemaining > 0) {
                                fastPollRemaining--;
                                if (fastPollRemaining == 0) {
                                    pollInterval = POLL_NORMAL_US;
                                }
                            }
                        }
                    }
                });
            }
        }
    }

    void detachInterrupt(uint32_t interruptNum) override
    {
        if (interruptNum == RADIOLIB_NC) {
            return;
        }

        if (irqLineRequest) {
            // Hardware GPIO path: stop the thread entirely
            irqRunning = false;
            if (irqThread.joinable()) {
                irqThread.join();
            }
            irqCallback = nullptr;
        } else {
            // Software polling: just suppress callbacks, keep thread alive.
            // The thread will be re-enabled by the next attachInterrupt call.
            irqPollEnabled = false;
        }
    }

    // ---- Timing ----

    void delay(unsigned long ms) override { delayMicroseconds(ms * 1000); }

    void delayMicroseconds(unsigned long us) override
    {
        if (us == 0) {
            sched_yield();
            return;
        }
        usleep(us);
    }

    void yield() override { usleep(100); } // 100us — prevents busy-wait CPU burn on Linux

    unsigned long millis() override
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return (tv.tv_sec * 1000ULL) + (tv.tv_usec / 1000ULL);
    }

    unsigned long micros() override
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return (tv.tv_sec * 1000000ULL) + tv.tv_usec;
    }

    long pulseIn(uint32_t pin, uint32_t state, unsigned long timeout) override
    {
        (void)pin;
        (void)state;
        (void)timeout;
        return 0;
    }

    // ---- SPI (proxied over I2C) ----

    void spiBegin() override {}
    void spiBeginTransaction() override {}

    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override
    {
        if (len == 0)
            return;

        std::lock_guard<std::mutex> lock(i2cMutex);

        // Protocol: write [CMD_TRANSMIT, out[0], out[1], ..., out[len-1]] to I2C
        //           then read len bytes back for the SPI MISO response
        uint8_t writeBuf[len + 1];
        writeBuf[0] = I2C_BRIDGE_CMD_TRANSMIT;
        std::memcpy(writeBuf + 1, out, len);

        for (int attempt = 0; attempt < I2C_BRIDGE_SPI_RETRIES; attempt++) {
            if (attempt > 0) {
                LOG_WARN("I2CBridgeHal: Retry %d/%d", attempt + 1, I2C_BRIDGE_SPI_RETRIES);
                usleep(I2C_BRIDGE_RETRY_DELAY_US);
            }

            ssize_t written = write(i2cFd, writeBuf, len + 1);
            if (written != (ssize_t)(len + 1)) {
                LOG_ERROR("I2CBridgeHal: I2C write failed (%zd/%zu)", written, len + 1);
                continue; // retry
            }

            // Read back MISO data one byte at a time (ATtiny84 sends from circular buffer)
            bool readOk = true;
            for (size_t i = 0; i < len; i++) {
                if (read(i2cFd, in + i, 1) != 1) {
                    LOG_ERROR("I2CBridgeHal: I2C read failed at byte %zu/%zu", i, len);
                    readOk = false;
                    break;
                }
            }
            if (readOk) {
                if (attempt > 0) {
                    LOG_INFO("I2CBridgeHal: Recovered after %d retries", attempt);
                }
                return; // success
            }
        }

        // All retries exhausted
        LOG_ERROR("I2CBridgeHal: I2C failed after %d attempts, flagging error", I2C_BRIDGE_SPI_RETRIES);
        inError = true;
        portduino_status.LoRa_in_error = true;
    }

    void spiEndTransaction() override {}
    void spiEnd() override {}

    bool isInError() const { return inError; }

  private:
    int i2cFd = -1;
    uint8_t i2cAddr;
    int irqGpioNum;
    std::mutex i2cMutex;
    int irqPinVirtual = -1;
    struct gpiod_chip *irqChip = nullptr;
    struct gpiod_line_request *irqLineRequest = nullptr;
    std::atomic<bool> irqRunning{false};
    std::atomic<bool> irqPollEnabled{false};
    std::atomic<bool> inError{false};
    std::thread irqThread;
    void (*irqCallback)(void) = nullptr;

    void setupIrqGpio()
    {
        // GPIO3_A0 = gpio96 = gpiochip3, line 0
        // General formula: chip = gpioNum / 32, line = gpioNum % 32
        int chipNum = irqGpioNum / 32;
        unsigned int lineOffset = irqGpioNum % 32;
        std::string chipPath = "/dev/gpiochip" + std::to_string(chipNum);

        irqChip = gpiod_chip_open(chipPath.c_str());
        if (!irqChip) {
            std::cerr << "I2CBridgeHal: Warning — could not open " << chipPath
                      << ", IRQ will not work" << std::endl;
            return;
        }

        struct gpiod_line_settings *settings = gpiod_line_settings_new();
        gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
        gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_RISING);
        gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);

        struct gpiod_line_config *lineConfig = gpiod_line_config_new();
        gpiod_line_config_add_line_settings(lineConfig, &lineOffset, 1, settings);

        struct gpiod_request_config *reqConfig = gpiod_request_config_new();
        gpiod_request_config_set_consumer(reqConfig, "meshtasticd-i2c-bridge");

        irqLineRequest = gpiod_chip_request_lines(irqChip, reqConfig, lineConfig);

        gpiod_request_config_free(reqConfig);
        gpiod_line_config_free(lineConfig);
        gpiod_line_settings_free(settings);

        if (!irqLineRequest) {
            std::cerr << "I2CBridgeHal: Warning — could not request GPIO " << irqGpioNum
                      << " (chip " << chipNum << " line " << lineOffset << "), IRQ will not work"
                      << std::endl;
            gpiod_chip_close(irqChip);
            irqChip = nullptr;
        } else {
            std::cout << "I2CBridgeHal: IRQ GPIO " << irqGpioNum << " ready (gpiochip"
                      << chipNum << " line " << lineOffset << ")" << std::endl;
        }
    }

    void cleanupIrqGpio()
    {
        if (irqLineRequest) {
            gpiod_line_request_release(irqLineRequest);
            irqLineRequest = nullptr;
        }
        if (irqChip) {
            gpiod_chip_close(irqChip);
            irqChip = nullptr;
        }
    }

    void syncI2CBuffer()
    {
        std::cout << "I2CBridgeHal: Synchronizing ATtiny84 buffer..." << std::endl;

        // Set radio to idle (StandbyRC)
        uint8_t cmdIdle[] = {I2C_BRIDGE_CMD_TRANSMIT, 0x80, 0x00};
        write(i2cFd, cmdIdle, sizeof(cmdIdle));
        usleep(1000);

        // Set RX/TX buffer index to 0
        uint8_t cmdBufIdx[] = {I2C_BRIDGE_CMD_TRANSMIT, 0x8F, 0x00, 0x00};
        write(i2cFd, cmdBufIdx, sizeof(cmdBufIdx));
        usleep(1000);

        // Write known verification pattern to SX1262 buffer (WriteBuffer cmd 0x0E)
        uint8_t cmdWrite[] = {I2C_BRIDGE_CMD_TRANSMIT, 0x0E, 0x00,
                              0x10, 0x20, 0x30, 0x40, 0x50, 0xAA, 0x55, 0x00, 0xFF};
        write(i2cFd, cmdWrite, sizeof(cmdWrite));
        usleep(1000);

        // Read buffer back (ReadBuffer cmd 0x1E) — this puts response in ATtiny buffer
        uint8_t cmdRead[] = {I2C_BRIDGE_CMD_TRANSMIT, 0x1E, 0x00,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        write(i2cFd, cmdRead, sizeof(cmdRead));
        usleep(1000);

        // Now read from I2C one byte at a time until we find the known pattern
        const uint8_t sequence[] = {0x10, 0x20, 0x30, 0x40, 0x50, 0xAA, 0x55, 0x00, 0xFF};
        const size_t seqLen = sizeof(sequence);
        bool done = false;
        bool started = false;
        size_t seqIdx = 0;
        int count = 0;

        while (!done && count < I2C_BRIDGE_SYNC_MAX_BYTES) {
            uint8_t byte = 0;
            if (read(i2cFd, &byte, 1) != 1) {
                break;
            }

            if (!started) {
                for (size_t i = 0; i < seqLen; i++) {
                    if (byte == sequence[i]) {
                        started = true;
                        seqIdx = i;
                        break;
                    }
                }
            } else {
                if (byte == sequence[seqIdx + 1]) {
                    seqIdx++;
                    if (seqIdx == seqLen - 1) {
                        done = true;
                    }
                } else {
                    started = false;
                }
            }
            count++;
        }

        if (done) {
            std::cout << "I2CBridgeHal: Buffer sync OK after " << count << " bytes" << std::endl;
        } else {
            std::cerr << "I2CBridgeHal: Buffer sync FAILED after " << count << " bytes!" << std::endl;
            inError = true;
        }
    }
};

#endif
