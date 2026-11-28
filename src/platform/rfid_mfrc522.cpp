/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Native MFRC522 / WS1850S (M5 RFID2) reader over Linux i2c-dev. See the header
 * for the feature list and provenance. Register/command constants and the
 * REQA/anticollision/SELECT/auth/read/write flows mirror the standard MFRC522
 * driver; Crypto1 for MIFARE Classic runs in the reader's hardware.
 */

#include "rfid_mfrc522.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "rfid_st25r.h" // platform::to_hex / parse_hex / sak_describe (shared helpers)

#if defined(__linux__)
#include <cerrno>
#include <time.h>

#include "cap_bringup.h"
#include "rfid_i2c.h"
#endif

#ifndef APP_RFID2_I2C_ADDR
#define APP_RFID2_I2C_ADDR 0x28
#endif

namespace platform {

#if defined(__linux__)
namespace {

// ===== MFRC522 register / command constants ==================================
enum Reg : uint8_t {
    CommandReg = 0x01,
    ComIrqReg = 0x04,
    DivIrqReg = 0x05,
    ErrorReg = 0x06,
    Status2Reg = 0x08,
    FIFODataReg = 0x09,
    FIFOLevelReg = 0x0A,
    ControlReg = 0x0C,
    BitFramingReg = 0x0D,
    CollReg = 0x0E,
    ModeReg = 0x11,
    TxModeReg = 0x12,
    RxModeReg = 0x13,
    TxControlReg = 0x14,
    TxASKReg = 0x15,
    CRCResultRegH = 0x21,
    CRCResultRegL = 0x22,
    ModWidthReg = 0x24,
    RFCfgReg = 0x26,
    TModeReg = 0x2A,
    TPrescalerReg = 0x2B,
    TReloadRegH = 0x2C,
    TReloadRegL = 0x2D,
    VersionReg = 0x37,
};

enum Cmd : uint8_t {
    CMD_Idle = 0x00,
    CMD_CalcCRC = 0x03,
    CMD_Transceive = 0x0C,
    CMD_MFAuthent = 0x0E,
    CMD_SoftReset = 0x0F,
};

enum Picc : uint8_t {
    PICC_REQA = 0x26,
    PICC_WUPA = 0x52,
    PICC_CT = 0x88,
    PICC_SEL_CL1 = 0x93,
    PICC_SEL_CL2 = 0x95,
    PICC_SEL_CL3 = 0x97,
    PICC_HLTA = 0x50,
    PICC_AUTH_KEY_A = 0x60,
    PICC_AUTH_KEY_B = 0x61,
    PICC_MF_READ = 0x30,
    PICC_MF_WRITE = 0xA0,
    PICC_UL_WRITE = 0xA2,
};

enum Status {
    ST_OK = 0,
    ST_ERROR,
    ST_COLLISION,
    ST_TIMEOUT,
    ST_NO_ROOM,
    ST_INTERNAL,
    ST_INVALID,
    ST_CRC_WRONG,
    ST_MIFARE_NACK,
};

constexpr uint8_t MF_ACK = 0x0A;

// Crypto1 key dictionary (same as the firmware's RFIDInterface default set).
const uint8_t kKeys[][6] = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5},
    {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5}, {0x4D, 0x3A, 0x99, 0xC3, 0x51, 0xDD},
    {0x1A, 0x98, 0x2C, 0x7E, 0x45, 0x9A}, {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
    {0x71, 0x4C, 0x5C, 0x88, 0x6E, 0x97}, {0x58, 0x7E, 0xE5, 0xF9, 0x35, 0x0F},
    {0xA0, 0x47, 0x8C, 0xC3, 0x90, 0x91}, {0x53, 0x3C, 0xB6, 0xC7, 0x23, 0xF6},
    {0x8F, 0xD0, 0xA4, 0xF2, 0x56, 0xE9}, {0xA6, 0x45, 0x98, 0xA7, 0x74, 0x78},
    {0x26, 0x94, 0x0B, 0x21, 0xFF, 0x5D}, {0xFC, 0x00, 0x01, 0x87, 0x78, 0xF7},
    {0x00, 0x00, 0x0F, 0xFE, 0x24, 0x88},
};
constexpr int kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);

// ===== Transport state =======================================================
int g_fd = -1;
uint8_t g_addr = APP_RFID2_I2C_ADDR;

void sleep_ms(long ms) {
    timespec ts{ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

void reg_write(uint8_t reg, uint8_t val) {
    uint8_t w[2] = {reg, val};
    i2c_write(g_fd, w, 2);
}

void reg_write_n(uint8_t reg, const uint8_t* vals, uint8_t n) {
    uint8_t w[1 + 64];
    if (n > 64) n = 64;
    w[0] = reg;
    std::memcpy(&w[1], vals, n);
    i2c_write(g_fd, w, static_cast<size_t>(n) + 1);
}

uint8_t reg_read(uint8_t reg) {
    uint8_t v = 0xFF;
    i2c_write_read(g_fd, g_addr, &reg, 1, &v, 1);
    return v;
}

void reg_read_n(uint8_t reg, uint8_t* vals, uint8_t n, uint8_t rxAlign) {
    if (n == 0) return;
    uint8_t tmp[64];
    if (n > 64) n = 64;
    i2c_write_read(g_fd, g_addr, &reg, 1, tmp, n);
    for (uint8_t i = 0; i < n; ++i) {
        if (i == 0 && rxAlign) {
            uint8_t mask = 0;
            for (uint8_t b = rxAlign; b <= 7; ++b) mask |= (1 << b);
            vals[0] = (vals[0] & ~mask) | (tmp[0] & mask);
        } else {
            vals[i] = tmp[i];
        }
    }
}

void set_bits(uint8_t reg, uint8_t mask) { reg_write(reg, reg_read(reg) | mask); }
void clr_bits(uint8_t reg, uint8_t mask) { reg_write(reg, reg_read(reg) & ~mask); }

// ===== PCD (reader) primitives ===============================================
void pcd_reset() {
    reg_write(CommandReg, CMD_SoftReset);
    for (int tries = 0; tries < 3; ++tries) {
        sleep_ms(50);
        if (!(reg_read(CommandReg) & (1 << 4))) break; // PowerDown cleared
    }
}

void antenna_on() {
    uint8_t v = reg_read(TxControlReg);
    if ((v & 0x03) != 0x03) reg_write(TxControlReg, v | 0x03);
}

uint8_t get_version() { return reg_read(VersionReg); }

bool pcd_init() {
    pcd_reset();
    reg_write(TxModeReg, 0x00);
    reg_write(RxModeReg, 0x00);
    reg_write(ModWidthReg, 0x26);
    reg_write(TModeReg, 0x80);       // TAuto
    reg_write(TPrescalerReg, 0xA9);  // ~25us ticks
    reg_write(TReloadRegH, 0x03);    // ~25ms timeout
    reg_write(TReloadRegL, 0xE8);
    reg_write(TxASKReg, 0x40);       // force 100% ASK
    reg_write(ModeReg, 0x3D);        // CRC preset 0x6363
    antenna_on();
    reg_write(RFCfgReg, 0x07 << 4);  // RxGain max (helps the WS1850S)
    sleep_ms(4);
    return true;
}

int calc_crc(const uint8_t* data, uint8_t len, uint8_t out2[2]) {
    reg_write(CommandReg, CMD_Idle);
    reg_write(DivIrqReg, 0x04);      // clear CRCIRq
    reg_write(FIFOLevelReg, 0x80);   // flush FIFO
    reg_write_n(FIFODataReg, data, len);
    reg_write(CommandReg, CMD_CalcCRC);
    for (int i = 5000; i > 0; --i) {
        if (reg_read(DivIrqReg) & 0x04) {
            reg_write(CommandReg, CMD_Idle);
            out2[0] = reg_read(CRCResultRegL);
            out2[1] = reg_read(CRCResultRegH);
            return ST_OK;
        }
    }
    return ST_TIMEOUT;
}

// Core transceive (ported from MFRC522 PCD_CommunicateWithPICC).
int communicate(uint8_t command, uint8_t waitIRq, const uint8_t* sendData, uint8_t sendLen,
                uint8_t* backData, uint8_t* backLen, uint8_t* validBits, uint8_t rxAlign,
                bool checkCRC) {
    uint8_t txLastBits = validBits ? *validBits : 0;
    uint8_t bitFraming = static_cast<uint8_t>((rxAlign << 4) + txLastBits);

    reg_write(CommandReg, CMD_Idle);
    reg_write(ComIrqReg, 0x7F);
    reg_write(FIFOLevelReg, 0x80);
    reg_write_n(FIFODataReg, sendData, sendLen);
    reg_write(BitFramingReg, bitFraming);
    reg_write(CommandReg, command);
    if (command == CMD_Transceive) set_bits(BitFramingReg, 0x80); // StartSend

    int i;
    for (i = 2000; i > 0; --i) {
        uint8_t n = reg_read(ComIrqReg);
        if (n & waitIRq) break;
        if (n & 0x01) return ST_TIMEOUT; // timer IRQ
    }
    if (i == 0) return ST_TIMEOUT;

    uint8_t err = reg_read(ErrorReg);
    if (err & 0x13) return ST_ERROR; // BufferOvfl ParityErr ProtocolErr

    uint8_t _validBits = 0;
    if (backData && backLen) {
        uint8_t n = reg_read(FIFOLevelReg);
        if (n > *backLen) return ST_NO_ROOM;
        *backLen = n;
        reg_read_n(FIFODataReg, backData, n, rxAlign);
        _validBits = reg_read(ControlReg) & 0x07;
        if (validBits) *validBits = _validBits;
    }
    if (err & 0x08) return ST_COLLISION;

    if (backData && backLen && checkCRC) {
        if (*backLen == 1 && _validBits == 4) return ST_MIFARE_NACK;
        if (*backLen < 2 || _validBits != 0) return ST_CRC_WRONG;
        uint8_t ctrl[2];
        int s = calc_crc(backData, static_cast<uint8_t>(*backLen - 2), ctrl);
        if (s != ST_OK) return s;
        if (backData[*backLen - 2] != ctrl[0] || backData[*backLen - 1] != ctrl[1])
            return ST_CRC_WRONG;
    }
    return ST_OK;
}

int transceive(const uint8_t* send, uint8_t sendLen, uint8_t* back, uint8_t* backLen,
               uint8_t* validBits, uint8_t rxAlign, bool checkCRC) {
    return communicate(CMD_Transceive, 0x30, send, sendLen, back, backLen, validBits, rxAlign,
                       checkCRC);
}

int reqa_or_wupa(uint8_t command, uint8_t atqa[2]) {
    clr_bits(CollReg, 0x80);
    uint8_t bufferSize = 2;
    uint8_t validBits = 7; // short frame
    int s = transceive(&command, 1, atqa, &bufferSize, &validBits, 0, false);
    if (s != ST_OK) return s;
    if (bufferSize != 2 || validBits != 0) return ST_ERROR;
    return ST_OK;
}

struct Uid {
    uint8_t size = 0; // 4, 7 or 10
    uint8_t uidByte[10] = {0};
    uint8_t sak = 0;
};

// Anticollision + SELECT across cascade levels (ported from PICC_Select).
int select_tag(Uid& uid) {
    bool uidComplete = false, useCascadeTag;
    uint8_t cascadeLevel = 1;
    uint8_t buffer[9];
    int result;

    clr_bits(CollReg, 0x80);
    while (!uidComplete) {
        uint8_t uidIndex;
        switch (cascadeLevel) {
            case 1: buffer[0] = PICC_SEL_CL1; uidIndex = 0; break;
            case 2: buffer[0] = PICC_SEL_CL2; uidIndex = 3; break;
            case 3: buffer[0] = PICC_SEL_CL3; uidIndex = 6; break;
            default: return ST_INTERNAL;
        }
        useCascadeTag = false; // we never supply known bits (full anticollision)

        int8_t currentLevelKnownBits = 0;
        uint8_t index = 2;
        if (useCascadeTag) buffer[index++] = PICC_CT;

        bool selectDone = false;
        uint8_t txLastBits = 0, rxAlign, bufferUsed, count, checkBit;
        uint8_t* responseBuffer;
        uint8_t responseLength;
        while (!selectDone) {
            if (currentLevelKnownBits >= 32) {
                buffer[1] = 0x70; // NVB: all 7 bytes
                buffer[6] = buffer[2] ^ buffer[3] ^ buffer[4] ^ buffer[5];
                result = calc_crc(buffer, 7, &buffer[7]);
                if (result != ST_OK) return result;
                txLastBits = 0;
                bufferUsed = 9;
                responseBuffer = &buffer[6];
                responseLength = 3;
            } else {
                txLastBits = currentLevelKnownBits % 8;
                count = currentLevelKnownBits / 8;
                index = 2 + count;
                buffer[1] = static_cast<uint8_t>((index << 4) + txLastBits);
                bufferUsed = index + (txLastBits ? 1 : 0);
                responseBuffer = &buffer[index];
                responseLength = static_cast<uint8_t>(sizeof(buffer) - index);
            }
            rxAlign = txLastBits;
            reg_write(BitFramingReg, static_cast<uint8_t>((rxAlign << 4) + txLastBits));
            result = transceive(buffer, bufferUsed, responseBuffer, &responseLength, &txLastBits,
                               rxAlign, false);
            if (result == ST_COLLISION) {
                uint8_t coll = reg_read(CollReg);
                if (coll & 0x20) return ST_COLLISION;
                uint8_t collPos = coll & 0x1F;
                if (collPos == 0) collPos = 32;
                if (collPos <= currentLevelKnownBits) return ST_INTERNAL;
                currentLevelKnownBits = collPos;
                count = (currentLevelKnownBits - 1) % 8;
                checkBit = count;
                index = 1 + (currentLevelKnownBits / 8) + ((currentLevelKnownBits % 8) ? 1 : 0);
                buffer[index] |= (1 << checkBit);
            } else if (result != ST_OK) {
                return result;
            } else {
                if (currentLevelKnownBits >= 32) selectDone = true;
                else currentLevelKnownBits = 32;
            }
        }

        index = (buffer[2] == PICC_CT) ? 3 : 2;
        uint8_t bytesToCopy = (buffer[2] == PICC_CT) ? 3 : 4;
        for (count = 0; count < bytesToCopy; ++count) uid.uidByte[uidIndex + count] = buffer[index++];

        if (responseLength != 3 || txLastBits != 0) return ST_ERROR;
        uint8_t ctrl[2];
        result = calc_crc(responseBuffer, 1, ctrl);
        if (result != ST_OK) return result;
        if (ctrl[0] != responseBuffer[1] || ctrl[1] != responseBuffer[2]) return ST_CRC_WRONG;

        if (responseBuffer[0] & 0x04) {
            cascadeLevel++;
        } else {
            uidComplete = true;
            uid.sak = responseBuffer[0];
        }
    }
    uid.size = static_cast<uint8_t>(3 * cascadeLevel + 1);
    return ST_OK;
}

int halt() {
    uint8_t buffer[4] = {PICC_HLTA, 0, 0, 0};
    int s = calc_crc(buffer, 2, &buffer[2]);
    if (s != ST_OK) return s;
    s = transceive(buffer, 4, nullptr, nullptr, nullptr, 0, false);
    if (s == ST_TIMEOUT) return ST_OK; // no response = HALT acknowledged
    return (s == ST_OK) ? ST_ERROR : s;
}

void stop_crypto1() { clr_bits(Status2Reg, 0x08); }

int authenticate(uint8_t keyCmd, uint8_t block, const uint8_t key[6], const Uid& uid) {
    uint8_t send[12];
    send[0] = keyCmd;
    send[1] = block;
    std::memcpy(&send[2], key, 6);
    for (int i = 0; i < 4; ++i) send[8 + i] = uid.uidByte[i + uid.size - 4];
    return communicate(CMD_MFAuthent, 0x10, send, sizeof(send), nullptr, nullptr, nullptr, 0, false);
}

int mifare_read(uint8_t block, uint8_t out[18], uint8_t* outLen) {
    out[0] = PICC_MF_READ;
    out[1] = block;
    int s = calc_crc(out, 2, &out[2]);
    if (s != ST_OK) return s;
    *outLen = 18;
    return transceive(out, 4, out, outLen, nullptr, 0, true);
}

// Two-step MIFARE write of up to 16 bytes (Classic block / UL compatibility).
int mifare_transceive(const uint8_t* data, uint8_t len) {
    if (len > 16) return ST_INVALID;
    uint8_t buf[18];
    std::memcpy(buf, data, len);
    int s = calc_crc(buf, len, &buf[len]);
    if (s != ST_OK) return s;
    uint8_t backLen = sizeof(buf);
    uint8_t validBits = 0;
    s = communicate(CMD_Transceive, 0x30, buf, static_cast<uint8_t>(len + 2), buf, &backLen,
                    &validBits, 0, false);
    if (s != ST_OK) return s;
    if (backLen != 1 || validBits != 4) return ST_ERROR;
    if (buf[0] != MF_ACK) return ST_MIFARE_NACK;
    return ST_OK;
}

int mifare_write_block(uint8_t block, const uint8_t data16[16]) {
    uint8_t cmd[2] = {PICC_MF_WRITE, block};
    int s = mifare_transceive(cmd, 2);
    if (s != ST_OK) return s;
    return mifare_transceive(data16, 16);
}

// MIFARE Ultralight / NTAG page write: 4-byte page via UL_WRITE (0xA2).
int ul_write_page(uint8_t page, const uint8_t data4[4]) {
    uint8_t buf[6] = {PICC_UL_WRITE, page, data4[0], data4[1], data4[2], data4[3]};
    return mifare_transceive(buf, 6);
}

// ===== Tag typing ============================================================
const char* picc_type_name(uint8_t sak) {
    switch (sak & 0x7F) {
        case 0x04: return "UID not complete";
        case 0x09: return "MIFARE Mini";
        case 0x08: return "MIFARE Classic 1K";
        case 0x18: return "MIFARE Classic 4K";
        case 0x00: return "MIFARE Ultralight / NTAG";
        case 0x10:
        case 0x11: return "MIFARE Plus";
        case 0x20: return "ISO14443-4";
        case 0x40: return "ISO18092 / NFC";
        default: return "unknown";
    }
}
bool is_classic(uint8_t sak) {
    uint8_t s = sak & 0x7F;
    return s == 0x08 || s == 0x18 || s == 0x09;
}
// Blocks per sector and total sectors per Classic variant.
void classic_geometry(uint8_t sak, uint8_t& sectors, uint16_t& totalBlocks) {
    switch (sak & 0x7F) {
        case 0x09: sectors = 5;  totalBlocks = 20;  break; // Mini
        case 0x18: sectors = 40; totalBlocks = 256; break; // 4K
        default:   sectors = 16; totalBlocks = 64;  break; // 1K
    }
}
uint8_t sector_first_block(uint8_t sector) {
    return sector < 32 ? static_cast<uint8_t>(sector * 4)
                       : static_cast<uint8_t>(128 + (sector - 32) * 16);
}
uint8_t sector_block_count(uint8_t sector) { return sector < 32 ? 4 : 16; }

// ===== Transport open / power ================================================
bool rfid2_detect_known_version(uint8_t v) {
    switch (v) {
        case 0x15: // WS1850S (M5 RFID2)
        case 0x12: // counterfeit
        case 0x88: // FM17522
        case 0xB2: // FM17522
        case 0x89: // FM17522E
        case 0x90:
        case 0x91:
        case 0x92:
            return true;
        default:
            return false;
    }
}

bool open_one(const std::string& dev, uint8_t addr, std::string& report) {
    g_addr = addr;
    g_fd = i2c_open(dev.c_str(), addr);
    if (g_fd < 0) {
        report += "  cannot open " + dev + " (" + std::strerror(errno) + ")\n";
        return false;
    }
    pcd_init();
    const uint8_t v = get_version();
    if (!rfid2_detect_known_version(v)) {
        char line[96];
        std::snprintf(line, sizeof(line), "  no MFRC522 at %s@0x%02X (VersionReg=0x%02X)\n",
                      dev.c_str(), addr, v);
        report += line;
        i2c_close(g_fd);
        g_fd = -1;
        return false;
    }
    char line[96];
    std::snprintf(line, sizeof(line), "MFRC522/WS1850S detected at %s @ 0x%02X (VersionReg=0x%02X)\n",
                  dev.c_str(), addr, v);
    report += line;
    return true;
}

// spec: ""/"auto" (scan buses@0x28), "/dev/i2c-1", "/dev/i2c-1@0x28", "i2c:..."
bool open_chip(const std::string& spec, std::string& report) {
    report += cap_bringup().text; // power the module rail (best-effort)

    std::string s = spec;
    if (s == "auto") s.clear();
    if (s.rfind("i2c:", 0) == 0) s = s.substr(4);

    uint8_t addr = APP_RFID2_I2C_ADDR;
    std::string dev;
    if (!s.empty()) {
        const auto at = s.find('@');
        if (at != std::string::npos) {
            addr = static_cast<uint8_t>(std::strtol(s.c_str() + at + 1, nullptr, 0));
            dev = s.substr(0, at);
        } else {
            dev = s;
        }
    }
    if (const char* env = std::getenv("RFID2_I2C_ADDR")) {
        if (env[0]) addr = static_cast<uint8_t>(std::strtol(env, nullptr, 0));
    }

    if (!dev.empty()) return open_one(dev, addr, report);

    report += "scanning I2C buses for an MFRC522 at 0x" +
              to_hex(&addr, 1) + " ...\n";
    for (const auto& bus : i2c_buses()) {
        if (open_one(bus, addr, report)) return true;
    }
    report += "error: no MFRC522 / WS1850S found. Check the RFID2 unit wiring and the Grove bus.\n";
    return false;
}

void close_chip() {
    if (g_fd >= 0) {
        stop_crypto1();
        i2c_close(g_fd);
        g_fd = -1;
    }
}

// Shared: REQA/WUPA + SELECT; prints UID/ATQA/SAK/type. Returns ST_OK/err.
int scan_once(std::string& report, Uid& uid, uint8_t atqa[2]) {
    int s = reqa_or_wupa(PICC_REQA, atqa);
    if (s != ST_OK) s = reqa_or_wupa(PICC_WUPA, atqa);
    if (s != ST_OK) {
        report += "No tag in field (present a card on the antenna).\n";
        return s;
    }
    char line[96];
    std::snprintf(line, sizeof(line), "ATQA: %02X %02X\n", atqa[0], atqa[1]);
    report += line;
    s = select_tag(uid);
    if (s != ST_OK) {
        report += "Anticollision/SELECT failed (tag moved or unsupported).\n";
        return s;
    }
    report += "UID:  " + to_hex(uid.uidByte, uid.size, true) + "\n";
    std::snprintf(line, sizeof(line), "SAK:  %02X  %s\n", uid.sak, picc_type_name(uid.sak));
    report += line;
    return ST_OK;
}

std::string arg_spec(int argc, char** argv, int at) {
    if (argc > at && argv[at] && argv[at][0] != '\0') return argv[at];
    if (const char* e = std::getenv("RFID2_I2C")) {
        if (e[0]) return e;
    }
    return "auto";
}

} // namespace
#endif // __linux__

// ===== Subcommand entry points ===============================================

int rfid2_detect_main(int argc, char** argv) {
#if defined(__linux__)
    std::string report = "M5 RFID2 (MFRC522 / WS1850S) detection\n";
    const bool ok = open_chip(arg_spec(argc, argv, 0), report);
    close_chip();
    std::fputs(report.c_str(), stdout);
    std::fflush(stdout);
    return ok ? 0 : 1;
#else
    (void)argc; (void)argv;
    std::fputs("RFID2 is only available on Linux builds.\n", stdout);
    return 1;
#endif
}

int rfid2_read_main(int argc, char** argv) {
#if defined(__linux__)
    std::string report = "RFID2 read (ISO14443A)\n";
    if (!open_chip(arg_spec(argc, argv, 0), report)) {
        std::fputs(report.c_str(), stdout);
        return 1;
    }
    Uid uid;
    uint8_t atqa[2] = {0};
    const int s = scan_once(report, uid, atqa);
    halt();
    close_chip();
    std::fputs(report.c_str(), stdout);
    std::fflush(stdout);
    return s == ST_OK ? 0 : 1;
#else
    (void)argc; (void)argv;
    std::fputs("RFID2 is only available on Linux builds.\n", stdout);
    return 1;
#endif
}

int rfid2_dump_main(int argc, char** argv) {
#if defined(__linux__)
    std::string report = "RFID2 dump\n";
    if (!open_chip(arg_spec(argc, argv, 0), report)) {
        std::fputs(report.c_str(), stdout);
        return 1;
    }
    Uid uid;
    uint8_t atqa[2] = {0};
    if (scan_once(report, uid, atqa) != ST_OK) {
        close_chip();
        std::fputs(report.c_str(), stdout);
        return 1;
    }

    if (is_classic(uid.sak)) {
        uint8_t sectors;
        uint16_t totalBlocks;
        classic_geometry(uid.sak, sectors, totalBlocks);
        report += "MIFARE Classic dump (" + std::to_string(totalBlocks) + " blocks, " +
                  std::to_string(sectors) + " sectors)\n";
        for (uint8_t sec = 0; sec < sectors; ++sec) {
            // Find a working key (A then B) from the dictionary for this sector.
            const uint8_t first = sector_first_block(sec);
            const uint8_t trailer = static_cast<uint8_t>(first + sector_block_count(sec) - 1);
            int keyIdx = -1;
            bool keyB = false;
            for (int k = 0; k < kKeyCount && keyIdx < 0; ++k) {
                if (authenticate(PICC_AUTH_KEY_A, trailer, kKeys[k], uid) == ST_OK) {
                    keyIdx = k; keyB = false; break;
                }
                // Auth failure desyncs Crypto1; re-select before the next try.
                stop_crypto1();
                halt();
                reqa_or_wupa(PICC_WUPA, atqa);
                select_tag(uid);
                if (authenticate(PICC_AUTH_KEY_B, trailer, kKeys[k], uid) == ST_OK) {
                    keyIdx = k; keyB = true; break;
                }
                stop_crypto1();
                halt();
                reqa_or_wupa(PICC_WUPA, atqa);
                select_tag(uid);
            }
            char hdr[80];
            if (keyIdx < 0) {
                std::snprintf(hdr, sizeof(hdr), "sector %2u: no key found (locked)\n", sec);
                report += hdr;
                continue;
            }
            std::snprintf(hdr, sizeof(hdr), "sector %2u: key %s[%d] = %s\n", sec, keyB ? "B" : "A",
                          keyIdx, to_hex(kKeys[keyIdx], 6).c_str());
            report += hdr;
            for (uint8_t b = first; b <= trailer; ++b) {
                uint8_t buf[18] = {0};
                uint8_t len = 0;
                if (mifare_read(b, buf, &len) == ST_OK && len >= 16) {
                    char line[64];
                    std::snprintf(line, sizeof(line), "  blk %3u: %s\n", b,
                                  to_hex(buf, 16, true).c_str());
                    report += line;
                } else {
                    char line[48];
                    std::snprintf(line, sizeof(line), "  blk %3u: read failed\n", b);
                    report += line;
                }
            }
            stop_crypto1();
        }
    } else {
        // Type-2 (Ultralight / NTAG): READ returns 4 pages (16 bytes) at a time.
        report += "Type-2 dump (Ultralight / NTAG)\n";
        for (uint8_t page = 0; page < 231; page += 4) {
            uint8_t buf[18] = {0};
            uint8_t len = 0;
            if (mifare_read(page, buf, &len) != ST_OK || len < 16) break;
            for (int i = 0; i < 4; ++i) {
                char line[48];
                std::snprintf(line, sizeof(line), "pg %3u: %s\n", page + i,
                              to_hex(&buf[i * 4], 4, true).c_str());
                report += line;
            }
        }
    }

    halt();
    close_chip();
    std::fputs(report.c_str(), stdout);
    std::fflush(stdout);
    return 0;
#else
    (void)argc; (void)argv;
    std::fputs("RFID2 is only available on Linux builds.\n", stdout);
    return 1;
#endif
}

int rfid2_write_main(int argc, char** argv) {
#if defined(__linux__)
    if (argc < 2) {
        std::fputs("usage: --rfid2-write <block> <hex> [spec]\n"
                   "  Classic: 16 bytes (32 hex) to a block; Ultralight/NTAG: 4 bytes (8 hex) to a page.\n",
                   stdout);
        return 2;
    }
    const int block = std::atoi(argv[0]);
    std::vector<uint8_t> data;
    if (block < 0 || block > 255 || !parse_hex(argv[1], data) ||
        (data.size() != 16 && data.size() != 4)) {
        std::fputs("error: block 0-255 and data either 16 bytes (Classic) or 4 bytes (UL/NTAG).\n",
                   stdout);
        return 2;
    }
    std::string report = "RFID2 write\n";
    if (!open_chip(arg_spec(argc, argv, 2), report)) {
        std::fputs(report.c_str(), stdout);
        return 1;
    }
    Uid uid;
    uint8_t atqa[2] = {0};
    bool ok = false;
    if (scan_once(report, uid, atqa) == ST_OK) {
        if (is_classic(uid.sak) && data.size() == 16) {
            // Authenticate the block's sector with a dictionary key, then write.
            const uint8_t sector = (block < 128) ? (block / 4) : static_cast<uint8_t>(32 + (block - 128) / 16);
            const uint8_t trailer = static_cast<uint8_t>(sector_first_block(sector) +
                                                         sector_block_count(sector) - 1);
            int auth = ST_ERROR;
            for (int k = 0; k < kKeyCount; ++k) {
                auth = authenticate(PICC_AUTH_KEY_A, trailer, kKeys[k], uid);
                if (auth == ST_OK) { report += "auth: key A[" + std::to_string(k) + "]\n"; break; }
                stop_crypto1(); halt(); reqa_or_wupa(PICC_WUPA, atqa); select_tag(uid);
                auth = authenticate(PICC_AUTH_KEY_B, trailer, kKeys[k], uid);
                if (auth == ST_OK) { report += "auth: key B[" + std::to_string(k) + "]\n"; break; }
                stop_crypto1(); halt(); reqa_or_wupa(PICC_WUPA, atqa); select_tag(uid);
            }
            if (auth == ST_OK) {
                report += "writing block " + std::to_string(block) + " = " +
                          to_hex(data.data(), 16, true) + " ...\n";
                ok = mifare_write_block(static_cast<uint8_t>(block), data.data()) == ST_OK;
            } else {
                report += "error: could not authenticate the sector with any dictionary key.\n";
            }
        } else if (data.size() == 4) {
            report += "writing page " + std::to_string(block) + " = " +
                      to_hex(data.data(), 4, true) + " (UL/NTAG) ...\n";
            ok = ul_write_page(static_cast<uint8_t>(block), data.data()) == ST_OK;
        } else {
            report += "error: MIFARE Classic needs 16 data bytes; Ultralight/NTAG needs 4.\n";
        }
        report += ok ? "Write OK (tag ACKed).\n" : "Write FAILED (no ACK / locked / wrong length).\n";
    }
    halt();
    close_chip();
    std::fputs(report.c_str(), stdout);
    std::fflush(stdout);
    return ok ? 0 : 1;
#else
    (void)argc; (void)argv;
    std::fputs("RFID2 is only available on Linux builds.\n", stdout);
    return 1;
#endif
}

} // namespace platform
