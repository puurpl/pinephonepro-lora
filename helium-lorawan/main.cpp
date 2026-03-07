// helium-lorawan — Standalone Helium IoT / LoRaWAN CLI tool for PinePhone Pro
//
// Sends and receives data over the Helium IoT network (LoRaWAN) using the
// PineDio LoRa backplate hardware: ATtiny84 I2C-to-SPI bridge + SX1262.
//
// This is NOT an Arduino program. It uses RadioLib 7.5.0 compiled in generic
// (non-Arduino) mode with a custom I2CBridgeHal that proxies SPI over I2C.
//
// Commands:
//   join           — Perform OTAA join with the Helium network, save session
//   send [port] d  — Send uplink data (hex or ASCII), show any downlink
//   status         — Show session info and frame counters (no radio needed)
//
// Radio sharing: meshtasticd is stopped before radio access and restarted
// after. Signal handlers ensure restart even on SIGINT/SIGTERM.
//
// Session persistence: nonces and session buffers are saved to
// ~/.helium-lorawan/ so subsequent sends don't need to re-join.
//
// Region: US915 FSB2 (Helium subband, channels 8-15, 903.9-905.3 MHz)
//
// See docs/full-build-log.md Step 8 for full documentation.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#include <RadioLib.h>
#include "I2CBridgeHal.h"
#include "credentials.h"

// ---------------------------------------------------------------------------
// Hardware configuration
// ---------------------------------------------------------------------------
#define I2C_DEVICE    "/dev/i2c-5"
#define I2C_ADDR      0x28
#define IRQ_PIN       10          // virtual pin for RadioLib

// ---------------------------------------------------------------------------
// LoRaWAN configuration
// ---------------------------------------------------------------------------
static const LoRaWANBand_t* region = &US915;
static const uint8_t subBand = 2;  // Helium uses FSB2 (channels 8-15)

// Session persistence paths
#define SESSION_DIR   "/home/mobian/.helium-lorawan"
#define NONCES_FILE   SESSION_DIR "/nonces.bin"
#define SESSION_FILE  SESSION_DIR "/session.bin"

// ---------------------------------------------------------------------------
// Global state for signal handler cleanup
// ---------------------------------------------------------------------------
static I2CBridgeHal* g_hal = nullptr;

static void restartMeshtasticd()
{
    printf("[helium] Restarting meshtasticd...\n");
    system("systemctl start meshtasticd 2>/dev/null");
}

static void cleanShutdown()
{
    if (g_hal) {
        g_hal->term();
        g_hal = nullptr;
    }
    restartMeshtasticd();
}

static void signalHandler(int sig)
{
    (void)sig;
    fprintf(stderr, "\n[helium] Interrupted, cleaning up...\n");
    cleanShutdown();
    _exit(1);
}

// ---------------------------------------------------------------------------
// File I/O helpers
// ---------------------------------------------------------------------------
static bool ensureSessionDir()
{
    struct stat st;
    if (stat(SESSION_DIR, &st) != 0) {
        if (mkdir(SESSION_DIR, 0700) != 0) {
            fprintf(stderr, "[helium] ERROR: Could not create %s\n", SESSION_DIR);
            return false;
        }
    }
    return true;
}

static bool writeFile(const char* path, const uint8_t* data, size_t len)
{
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f.write(reinterpret_cast<const char*>(data), len);
    return f.good();
}

static bool readFile(const char* path, uint8_t* data, size_t len)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f.read(reinterpret_cast<char*>(data), len);
    return (f.gcount() == (std::streamsize)len);
}

static bool fileExists(const char* path)
{
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

// ---------------------------------------------------------------------------
// Session persistence
// ---------------------------------------------------------------------------
static bool saveNonces(LoRaWANNode& node)
{
    if (!ensureSessionDir()) return false;
    uint8_t* buf = node.getBufferNonces();
    if (!writeFile(NONCES_FILE, buf, RADIOLIB_LORAWAN_NONCES_BUF_SIZE)) {
        fprintf(stderr, "[helium] ERROR: Failed to save nonces\n");
        return false;
    }
    printf("[helium] Nonces saved (%d bytes)\n", RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
    return true;
}

static bool saveSession(LoRaWANNode& node)
{
    if (!ensureSessionDir()) return false;
    uint8_t* buf = node.getBufferSession();
    if (!writeFile(SESSION_FILE, buf, RADIOLIB_LORAWAN_SESSION_BUF_SIZE)) {
        fprintf(stderr, "[helium] ERROR: Failed to save session\n");
        return false;
    }
    printf("[helium] Session saved (%d bytes)\n", RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
    return true;
}

static bool restoreSession(LoRaWANNode& node)
{
    uint8_t nonceBuf[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
    uint8_t sessionBuf[RADIOLIB_LORAWAN_SESSION_BUF_SIZE];

    if (!fileExists(NONCES_FILE) || !fileExists(SESSION_FILE)) {
        printf("[helium] No saved session found\n");
        return false;
    }

    if (!readFile(NONCES_FILE, nonceBuf, RADIOLIB_LORAWAN_NONCES_BUF_SIZE)) {
        fprintf(stderr, "[helium] ERROR: Failed to read nonces file\n");
        return false;
    }
    if (!readFile(SESSION_FILE, sessionBuf, RADIOLIB_LORAWAN_SESSION_BUF_SIZE)) {
        fprintf(stderr, "[helium] ERROR: Failed to read session file\n");
        return false;
    }

    int16_t state = node.setBufferNonces(nonceBuf);
    if (state != RADIOLIB_ERR_NONE) {
        fprintf(stderr, "[helium] ERROR: setBufferNonces failed (code %d)\n", state);
        return false;
    }

    state = node.setBufferSession(sessionBuf);
    if (state != RADIOLIB_ERR_NONE) {
        fprintf(stderr, "[helium] ERROR: setBufferSession failed (code %d)\n", state);
        return false;
    }

    printf("[helium] Session restored from disk\n");
    return true;
}

// ---------------------------------------------------------------------------
// Hex string utilities
// ---------------------------------------------------------------------------
static bool isHexString(const char* s)
{
    size_t len = strlen(s);
    if (len == 0 || len % 2 != 0) return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

static size_t hexToBytes(const char* hex, uint8_t* out, size_t maxLen)
{
    size_t hexLen = strlen(hex);
    size_t byteLen = hexLen / 2;
    if (byteLen > maxLen) return 0;
    for (size_t i = 0; i < byteLen; i++) {
        unsigned int byte;
        if (sscanf(hex + 2 * i, "%2x", &byte) != 1) return 0;
        out[i] = (uint8_t)byte;
    }
    return byteLen;
}

static void printHex(const uint8_t* data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        printf("%02X", data[i]);
        if (i < len - 1) printf(" ");
    }
}

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------
static void printError(const char* context, int16_t code)
{
    fprintf(stderr, "[helium] ERROR: %s failed with code %d", context, code);
    switch (code) {
        case RADIOLIB_ERR_NETWORK_NOT_JOINED:
            fprintf(stderr, " (network not joined)\n"); break;
        case RADIOLIB_ERR_NONCES_DISCARDED:
            fprintf(stderr, " (nonces discarded / invalid)\n"); break;
        case RADIOLIB_ERR_SESSION_DISCARDED:
            fprintf(stderr, " (session discarded / invalid)\n"); break;
        case RADIOLIB_ERR_NO_JOIN_ACCEPT:
            fprintf(stderr, " (no join accept received)\n"); break;
        case RADIOLIB_ERR_INVALID_FREQUENCY:
            fprintf(stderr, " (invalid frequency)\n"); break;
        case RADIOLIB_ERR_INVALID_BANDWIDTH:
            fprintf(stderr, " (invalid bandwidth)\n"); break;
        case RADIOLIB_ERR_SPI_CMD_FAILED:
            fprintf(stderr, " (SPI command failed -- check I2C bridge)\n"); break;
        case RADIOLIB_ERR_SPI_CMD_TIMEOUT:
            fprintf(stderr, " (SPI timeout -- radio not responding)\n"); break;
        case RADIOLIB_ERR_CHIP_NOT_FOUND:
            fprintf(stderr, " (chip not found)\n"); break;
        default:
            fprintf(stderr, "\n"); break;
    }
}

// ---------------------------------------------------------------------------
// Radio init / cleanup
// ---------------------------------------------------------------------------
struct RadioContext {
    I2CBridgeHal* hal;
    Module* mod;
    SX1262* radio;
};

static int initRadio(RadioContext& ctx)
{
    printf("[helium] Stopping meshtasticd...\n");
    system("systemctl stop meshtasticd 2>/dev/null");
    sleep(1);

    try {
        ctx.hal = new I2CBridgeHal(I2C_DEVICE, I2C_ADDR);
    } catch (const std::exception& e) {
        fprintf(stderr, "[helium] ERROR: %s\n", e.what());
        restartMeshtasticd();
        return 1;
    }

    g_hal = ctx.hal;

    ctx.mod = new Module(ctx.hal, RADIOLIB_NC, IRQ_PIN, RADIOLIB_NC, RADIOLIB_NC);
    ctx.radio = new SX1262(ctx.mod);

    printf("[helium] Initializing SX1262...\n");
    int16_t state = ctx.radio->begin();
    if (state != RADIOLIB_ERR_NONE) {
        printError("radio.begin()", state);
        cleanShutdown();
        return 1;
    }
    printf("[helium] SX1262 initialized OK\n");

    return 0;
}

static void freeRadio(RadioContext& ctx)
{
    if (ctx.radio) { delete ctx.radio; ctx.radio = nullptr; }
    if (ctx.mod)   { delete ctx.mod;   ctx.mod = nullptr; }
}

// ---------------------------------------------------------------------------
// Command: join
// ---------------------------------------------------------------------------
static int cmdJoin()
{
    RadioContext ctx = {};
    if (initRadio(ctx) != 0) return 1;

    LoRaWANNode node(ctx.radio, region, subBand);
    node.setDwellTime(true, 400);  // US915 FCC requirement

    uint64_t joinEUI = LORAWAN_JOIN_EUI;
    uint64_t devEUI  = LORAWAN_DEV_EUI;
    uint8_t nwkKey[] = { LORAWAN_NWK_KEY };
    uint8_t appKey[] = { LORAWAN_APP_KEY };

    int16_t state = node.beginOTAA(joinEUI, devEUI, nwkKey, appKey);
    if (state != RADIOLIB_ERR_NONE) {
        printError("beginOTAA()", state);
        freeRadio(ctx);
        cleanShutdown();
        return 1;
    }

    // Restore existing nonces for incremented DevNonce (some servers require it)
    if (fileExists(NONCES_FILE)) {
        uint8_t nonceBuf[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
        if (readFile(NONCES_FILE, nonceBuf, RADIOLIB_LORAWAN_NONCES_BUF_SIZE)) {
            node.setBufferNonces(nonceBuf);
            printf("[helium] Restored existing nonces for re-join\n");
        }
    }

    node.clearSession();

    printf("[helium] Sending Join Request...\n");
    printf("[helium] (This may take 5-10 seconds for the join accept window)\n");

    LoRaWANJoinEvent_t joinEvent;
    state = node.activateOTAA(&joinEvent);
    if (state != RADIOLIB_ERR_NONE && state != RADIOLIB_LORAWAN_NEW_SESSION) {
        printError("activateOTAA()", state);
        fprintf(stderr, "[helium] Join failed. Check:\n");
        fprintf(stderr, "  - Credentials in credentials.h match your console device\n");
        fprintf(stderr, "  - There is Helium hotspot coverage in your area\n");
        fprintf(stderr, "  - The antenna is connected to the PineDio backplate\n");
        freeRadio(ctx);
        cleanShutdown();
        return 1;
    }

    printf("[helium] Join successful!\n");
    printf("[helium] DevNonce:  %u\n", joinEvent.devNonce);
    printf("[helium] JoinNonce: %u\n", joinEvent.joinNonce);
    printf("[helium] DevAddr:   0x%08lX\n", (unsigned long)node.getDevAddr());

    if (!saveNonces(node) || !saveSession(node)) {
        fprintf(stderr, "[helium] WARNING: Session state not fully saved!\n");
    }

    freeRadio(ctx);
    cleanShutdown();
    return 0;
}

// ---------------------------------------------------------------------------
// Command: send
// ---------------------------------------------------------------------------
static int cmdSend(int fPort, const char* dataStr)
{
    RadioContext ctx = {};
    if (initRadio(ctx) != 0) return 1;

    LoRaWANNode node(ctx.radio, region, subBand);
    node.setDwellTime(true, 400);

    uint64_t joinEUI = LORAWAN_JOIN_EUI;
    uint64_t devEUI  = LORAWAN_DEV_EUI;
    uint8_t nwkKey[] = { LORAWAN_NWK_KEY };
    uint8_t appKey[] = { LORAWAN_APP_KEY };

    int16_t state = node.beginOTAA(joinEUI, devEUI, nwkKey, appKey);
    if (state != RADIOLIB_ERR_NONE) {
        printError("beginOTAA()", state);
        freeRadio(ctx);
        cleanShutdown();
        return 1;
    }

    if (!restoreSession(node)) {
        fprintf(stderr, "[helium] ERROR: No saved session. Run 'helium-lorawan join' first.\n");
        freeRadio(ctx);
        cleanShutdown();
        return 1;
    }

    printf("[helium] Restoring LoRaWAN session...\n");
    state = node.activateOTAA();
    if (state != RADIOLIB_ERR_NONE && state != RADIOLIB_LORAWAN_SESSION_RESTORED) {
        printError("activateOTAA() [restore]", state);
        fprintf(stderr, "[helium] Session may be expired. Try 'helium-lorawan join' again.\n");
        freeRadio(ctx);
        cleanShutdown();
        return 1;
    }
    printf("[helium] Session active (DevAddr: 0x%08lX, FCntUp: %lu)\n",
           (unsigned long)node.getDevAddr(), (unsigned long)node.getFCntUp());

    // Parse uplink payload
    uint8_t uplinkData[242];
    size_t uplinkLen = 0;

    if (isHexString(dataStr)) {
        uplinkLen = hexToBytes(dataStr, uplinkData, sizeof(uplinkData));
        if (uplinkLen == 0) {
            fprintf(stderr, "[helium] ERROR: Invalid hex string\n");
            freeRadio(ctx);
            cleanShutdown();
            return 1;
        }
        printf("[helium] Sending %zu bytes (hex) on fPort %d: ", uplinkLen, fPort);
        printHex(uplinkData, uplinkLen);
        printf("\n");
    } else {
        uplinkLen = strlen(dataStr);
        if (uplinkLen > sizeof(uplinkData)) {
            fprintf(stderr, "[helium] ERROR: Payload too large (%zu bytes, max %zu)\n",
                    uplinkLen, sizeof(uplinkData));
            freeRadio(ctx);
            cleanShutdown();
            return 1;
        }
        memcpy(uplinkData, dataStr, uplinkLen);
        printf("[helium] Sending %zu bytes (ASCII) on fPort %d: \"%s\"\n",
               uplinkLen, fPort, dataStr);
    }

    // Send uplink and wait for downlink
    uint8_t downlinkData[RADIOLIB_LORAWAN_MAX_DOWNLINK_SIZE];
    size_t downlinkLen = 0;
    LoRaWANEvent_t uplinkEvent;
    LoRaWANEvent_t downlinkEvent;

    printf("[helium] Transmitting...\n");
    state = node.sendReceive(uplinkData, uplinkLen, (uint8_t)fPort,
                             downlinkData, &downlinkLen,
                             false,  // unconfirmed uplink
                             &uplinkEvent, &downlinkEvent);

    if (state < RADIOLIB_ERR_NONE) {
        printError("sendReceive()", state);
        freeRadio(ctx);
        cleanShutdown();
        return 1;
    }

    printf("[helium] Uplink sent successfully\n");
    printf("[helium]   Frequency:     %.1f MHz\n", uplinkEvent.freq);
    printf("[helium]   Data Rate:     DR%d\n", uplinkEvent.datarate);
    printf("[helium]   Frame Counter: %lu\n", (unsigned long)uplinkEvent.fCnt);

    if (state > 0) {
        printf("[helium] Downlink received (RX window %d)\n", state);
        printf("[helium]   fPort:         %u\n", downlinkEvent.fPort);
        printf("[helium]   Frame Counter: %lu\n", (unsigned long)downlinkEvent.fCnt);
        printf("[helium]   Frequency:     %.1f MHz\n", downlinkEvent.freq);
        if (downlinkLen > 0) {
            printf("[helium]   Data (%zu bytes): ", downlinkLen);
            printHex(downlinkData, downlinkLen);
            printf("\n");
            // Try to print as ASCII if printable
            bool printable = true;
            for (size_t i = 0; i < downlinkLen; i++) {
                if (downlinkData[i] < 0x20 || downlinkData[i] > 0x7E) {
                    printable = false;
                    break;
                }
            }
            if (printable) {
                printf("[helium]   ASCII:         \"%.*s\"\n",
                       (int)downlinkLen, (char*)downlinkData);
            }
        }
    } else {
        printf("[helium] No downlink received\n");
    }

    if (!saveNonces(node) || !saveSession(node)) {
        fprintf(stderr, "[helium] WARNING: Session state not fully saved!\n");
    }

    freeRadio(ctx);
    cleanShutdown();
    return 0;
}

// ---------------------------------------------------------------------------
// Command: status
// ---------------------------------------------------------------------------
static int cmdStatus()
{
    printf("[helium] Session files:\n");
    printf("  Nonces:  %s  %s\n", NONCES_FILE,
           fileExists(NONCES_FILE) ? "[exists]" : "[not found]");
    printf("  Session: %s  %s\n", SESSION_FILE,
           fileExists(SESSION_FILE) ? "[exists]" : "[not found]");

    if (!fileExists(NONCES_FILE) || !fileExists(SESSION_FILE)) {
        printf("\n[helium] No active session. Run 'helium-lorawan join' to join the network.\n");
        return 0;
    }

    uint8_t sessionBuf[RADIOLIB_LORAWAN_SESSION_BUF_SIZE];
    uint8_t nonceBuf[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];

    if (!readFile(SESSION_FILE, sessionBuf, RADIOLIB_LORAWAN_SESSION_BUF_SIZE)) {
        fprintf(stderr, "[helium] ERROR: Could not read session file\n");
        return 1;
    }
    if (!readFile(NONCES_FILE, nonceBuf, RADIOLIB_LORAWAN_NONCES_BUF_SIZE)) {
        fprintf(stderr, "[helium] ERROR: Could not read nonces file\n");
        return 1;
    }

    uint32_t devAddr = 0;
    memcpy(&devAddr, sessionBuf + RADIOLIB_LORAWAN_SESSION_DEV_ADDR, sizeof(uint32_t));

    uint32_t fCntUp = 0;
    memcpy(&fCntUp, sessionBuf + RADIOLIB_LORAWAN_SESSION_FCNT_UP, sizeof(uint32_t));

    uint32_t nFCntDown = 0;
    memcpy(&nFCntDown, sessionBuf + RADIOLIB_LORAWAN_SESSION_N_FCNT_DOWN, sizeof(uint32_t));

    uint32_t aFCntDown = 0;
    memcpy(&aFCntDown, sessionBuf + RADIOLIB_LORAWAN_SESSION_A_FCNT_DOWN, sizeof(uint32_t));

    uint8_t status = sessionBuf[RADIOLIB_LORAWAN_SESSION_STATUS];

    uint16_t devNonce = 0;
    memcpy(&devNonce, nonceBuf + RADIOLIB_LORAWAN_NONCES_DEV_NONCE, sizeof(uint16_t));

    printf("\n[helium] LoRaWAN Session Status:\n");
    printf("  Session State:    ");
    switch (status) {
        case RADIOLIB_LORAWAN_SESSION_NONE:       printf("NONE\n"); break;
        case RADIOLIB_LORAWAN_SESSION_ACTIVATING: printf("ACTIVATING\n"); break;
        case RADIOLIB_LORAWAN_SESSION_PENDING:    printf("PENDING\n"); break;
        case RADIOLIB_LORAWAN_SESSION_ACTIVE:     printf("ACTIVE\n"); break;
        default:                                  printf("0x%02X\n", status); break;
    }
    printf("  DevAddr:          0x%08X\n", devAddr);
    printf("  DevNonce:         %u\n", devNonce);
    printf("  FCnt Up:          %u\n", fCntUp);
    printf("  NFCnt Down:       %u\n", nFCntDown);
    printf("  AFCnt Down:       %u\n", aFCntDown);
    printf("  Region:           US915 (FSB%d)\n", subBand);

    return 0;
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------
static void printUsage(const char* progName)
{
    printf("Helium LoRaWAN Tool for PinePhone Pro (PineDio backplate)\n");
    printf("\n");
    printf("Usage:\n");
    printf("  %s join                     Perform OTAA join with Helium network\n", progName);
    printf("  %s send <port> <data>       Send uplink on fPort (hex or ASCII)\n", progName);
    printf("  %s send <data>              Send uplink on fPort 1 (default)\n", progName);
    printf("  %s status                   Show session info and frame counters\n", progName);
    printf("  %s --help                   Show this help\n", progName);
    printf("\n");
    printf("Data format:\n");
    printf("  If <data> is all hex chars and even length, it's sent as hex bytes.\n");
    printf("  Otherwise it is sent as raw ASCII text.\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s join\n", progName);
    printf("  %s send 1 48656C6C6F        Send 'Hello' as hex on port 1\n", progName);
    printf("  %s send 2 \"Hello World\"     Send ASCII on port 2\n", progName);
    printf("  %s send \"temperature=22.5\"  Send on default port 1\n", progName);
    printf("  %s status\n", progName);
    printf("\n");
    printf("Notes:\n");
    printf("  - meshtasticd is stopped before radio access and restarted after\n");
    printf("  - Credentials are compiled in from credentials.h\n");
    printf("  - Session is persisted in %s\n", SESSION_DIR);
    printf("  - Uses US915 FSB2 (Helium subband)\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandler;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        printUsage(argv[0]);
        return (argc < 2) ? 1 : 0;
    }

    const char* command = argv[1];

    if (strcmp(command, "join") == 0) {
        return cmdJoin();
    }

    if (strcmp(command, "send") == 0) {
        int fPort = 1;
        const char* dataStr = nullptr;

        if (argc == 3) {
            dataStr = argv[2];
        } else if (argc >= 4) {
            fPort = atoi(argv[2]);
            if (fPort < 1 || fPort > 223) {
                fprintf(stderr, "[helium] ERROR: fPort must be 1-223 (got %d)\n", fPort);
                return 1;
            }
            dataStr = argv[3];
        } else {
            fprintf(stderr, "[helium] ERROR: 'send' requires data argument\n");
            fprintf(stderr, "  Usage: %s send [port] <data>\n", argv[0]);
            return 1;
        }

        if (strlen(dataStr) == 0) {
            fprintf(stderr, "[helium] ERROR: data cannot be empty\n");
            return 1;
        }

        return cmdSend(fPort, dataStr);
    }

    if (strcmp(command, "status") == 0) {
        return cmdStatus();
    }

    fprintf(stderr, "[helium] ERROR: Unknown command '%s'\n", command);
    printUsage(argv[0]);
    return 1;
}
