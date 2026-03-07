#ifndef I2C_BRIDGE_HAL_H
#define I2C_BRIDGE_HAL_H

// Standalone I2C-to-SPI bridge HAL for the PineDio LoRa backplate (ATtiny84 bridge + SX1262)
// Implements RadioLibHal by proxying SPI over I2C.
//
// The PineDio LoRa backplate uses:
//   - ATtiny84 MCU as I2C-to-SPI bridge at address 0x28
//   - Semtech SX1262 LoRa transceiver on the SPI side
//
// Protocol: Write [0x01, spi_mosi_bytes...] over I2C, then read back spi_miso_bytes.
// Reference: JF's pinedio-lora-driver (Codeberg: JF002/pinedio-lora-driver)
//
// Standalone version: no Meshtastic/Portduino dependencies, no libgpiod IRQ.

#include <RadioLib.h>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/i2c-dev.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#define I2C_BRIDGE_CMD_TRANSMIT 0x01
#define I2C_BRIDGE_DEFAULT_ADDR 0x28
#define I2C_BRIDGE_SYNC_MAX_BYTES 256
#define I2C_BRIDGE_SPI_RETRIES 3
#define I2C_BRIDGE_RETRY_DELAY_US 10000

class I2CBridgeHal : public RadioLibHal
{
  public:
    explicit I2CBridgeHal(const std::string &i2cDevice, uint8_t i2cAddr = I2C_BRIDGE_DEFAULT_ADDR)
        : RadioLibHal(0, 1, 0, 1, 1, 0), // INPUT, OUTPUT, LOW, HIGH, RISING, FALLING
          i2cAddr(i2cAddr)
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
    }

    ~I2CBridgeHal()
    {
        if (i2cFd >= 0) {
            close(i2cFd);
        }
    }

    void init() override
    {
        syncI2CBuffer();
    }

    void term() override
    {
        if (i2cFd >= 0) {
            uint8_t drain;
            for (int i = 0; i < 16; i++) {
                if (read(i2cFd, &drain, 1) != 1)
                    break;
            }
            usleep(1000);

            uint8_t cmdIdle[] = {I2C_BRIDGE_CMD_TRANSMIT, 0x80, 0x00};
            write(i2cFd, cmdIdle, sizeof(cmdIdle));
            usleep(5000);

            for (int i = 0; i < 4; i++) {
                if (read(i2cFd, &drain, 1) != 1)
                    break;
            }
            usleep(1000);
            const char msg[] = "I2CBridgeHal: Clean shutdown complete\n";
            (void)::write(STDOUT_FILENO, msg, sizeof(msg) - 1);
        }
    }

    // ---- GPIO ----

    void pinMode(uint32_t pin, uint32_t mode) override
    {
        (void)pin;
        (void)mode;
    }

    void digitalWrite(uint32_t pin, uint32_t value) override
    {
        (void)pin;
        (void)value;
    }

    uint32_t digitalRead(uint32_t pin) override
    {
        // RADIOLIB_NC = unconfigured BUSY/RESET pin.
        // Return 0 (LOW = not busy) so BUSY wait loops exit.
        if (pin == RADIOLIB_NC) {
            return 0;
        }
        // All other pins (e.g., IRQ virtual pin): return 1 (HIGH = asserted)
        // so RadioLib's IRQ wait loops exit immediately. RadioLib then reads
        // the real IRQ flags via SPI (through our I2C bridge).
        return 1;
    }

    void attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode) override
    {
        (void)interruptNum;
        (void)interruptCb;
        (void)mode;
    }

    void detachInterrupt(uint32_t interruptNum) override
    {
        (void)interruptNum;
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

    void yield() override { usleep(100); }

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

        uint8_t writeBuf[len + 1];
        writeBuf[0] = I2C_BRIDGE_CMD_TRANSMIT;
        std::memcpy(writeBuf + 1, out, len);

        for (int attempt = 0; attempt < I2C_BRIDGE_SPI_RETRIES; attempt++) {
            if (attempt > 0) {
                fprintf(stderr, "WARN: I2CBridgeHal: Retry %d/%d\n", attempt + 1, I2C_BRIDGE_SPI_RETRIES);
                usleep(I2C_BRIDGE_RETRY_DELAY_US);
            }

            ssize_t written = write(i2cFd, writeBuf, len + 1);
            if (written != (ssize_t)(len + 1)) {
                fprintf(stderr, "ERROR: I2CBridgeHal: I2C write failed (%zd/%zu)\n", written, len + 1);
                continue;
            }

            bool readOk = true;
            for (size_t i = 0; i < len; i++) {
                if (read(i2cFd, in + i, 1) != 1) {
                    fprintf(stderr, "ERROR: I2CBridgeHal: I2C read failed at byte %zu/%zu\n", i, len);
                    readOk = false;
                    break;
                }
            }
            if (readOk) {
                if (attempt > 0) {
                    fprintf(stderr, "INFO: I2CBridgeHal: Recovered after %d retries\n", attempt);
                }
                return;
            }
        }

        fprintf(stderr, "ERROR: I2CBridgeHal: I2C failed after %d attempts\n", I2C_BRIDGE_SPI_RETRIES);
        inError = true;
    }

    void spiEndTransaction() override {}
    void spiEnd() override {}

    bool isInError() const { return inError; }

  private:
    int i2cFd = -1;
    uint8_t i2cAddr;
    bool inError = false;

    void syncI2CBuffer()
    {
        std::cout << "I2CBridgeHal: Synchronizing ATtiny84 buffer..." << std::endl;

        uint8_t cmdIdle[] = {I2C_BRIDGE_CMD_TRANSMIT, 0x80, 0x00};
        write(i2cFd, cmdIdle, sizeof(cmdIdle));
        usleep(1000);

        uint8_t cmdBufIdx[] = {I2C_BRIDGE_CMD_TRANSMIT, 0x8F, 0x00, 0x00};
        write(i2cFd, cmdBufIdx, sizeof(cmdBufIdx));
        usleep(1000);

        uint8_t cmdWrite[] = {I2C_BRIDGE_CMD_TRANSMIT, 0x0E, 0x00,
                              0x10, 0x20, 0x30, 0x40, 0x50, 0xAA, 0x55, 0x00, 0xFF};
        write(i2cFd, cmdWrite, sizeof(cmdWrite));
        usleep(1000);

        uint8_t cmdRead[] = {I2C_BRIDGE_CMD_TRANSMIT, 0x1E, 0x00,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        write(i2cFd, cmdRead, sizeof(cmdRead));
        usleep(1000);

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
                for (size_t i = 0; i + 1 < seqLen; i++) {
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
                    for (size_t i = 0; i + 1 < seqLen; i++) {
                        if (byte == sequence[i]) {
                            started = true;
                            seqIdx = i;
                            break;
                        }
                    }
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
