/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"
#include "ble_spam.h"
#include "bruce_module.h"
#include "cc1101.h"
#include "evil_portal.h"
#include "gps.h"
#include "root_sh.h"
#include "rfid_st25r.h"
#include "wardrive.h"

#include <cstring>

int main(int argc, char** argv) {
    // Hidden subcommand used by the "Use CC1101 CAP" menu entry: the app
    // re-execs itself to probe the CAP over spidev and stream the report.
    if (argc >= 2 && std::strcmp(argv[1], "--cc1101-detect") == 0) {
        // Dynamic path: full CAP auto-detection fills the BruceModule pin
        // registry, then the CC1101 pins are read from there (zero hardcoding).
        return hal::cc1101_detect_cli(argc - 2, argv + 2);
    }
    // Sub-GHz (M6): configure the CC1101 for OOK async RX and stream VERSION+RSSI.
    if (argc >= 2 && std::strcmp(argv[1], "--rf-info") == 0) {
        return hal::rf_info_cli(argc - 2, argv + 2);
    }
    // Sub-GHz (M7): capture GDO0 edges and print each frame's signed-µs durations.
    if (argc >= 2 && std::strcmp(argv[1], "--rf-record") == 0) {
        return hal::rf_record_cli(argc - 2, argv + 2);
    }
    // Sub-GHz (M9): transmit an OOK PT2262 code (bit-bang GDO0 in async TX).
    if (argc >= 2 && std::strcmp(argv[1], "--rf-send") == 0) {
        return hal::rf_send_cli(argc - 2, argv + 2);
    }
    // Sub-GHz (M9): replay a Flipper/Bruce RAW .sub recording.
    if (argc >= 2 && std::strcmp(argv[1], "--rf-replay") == 0) {
        return hal::rf_replay_cli(argc - 2, argv + 2);
    }
    // Sub-GHz protocol engine (ported Bruce): list registry / transmit by name.
    if (argc >= 2 && std::strcmp(argv[1], "--rf-protos") == 0) {
        return hal::rf_protos_cli(argc - 2, argv + 2);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--rf-tx-proto") == 0) {
        return hal::rf_tx_proto_cli(argc - 2, argv + 2);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--rf-selftest") == 0) {
        return hal::rf_selftest_cli(argc - 2, argv + 2);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--rf-raw") == 0) {
        return hal::rf_raw_cli(argc - 2, argv + 2);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--rf-carrier") == 0) {
        return hal::rf_carrier_cli(argc - 2, argv + 2);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--rf-decode-sub") == 0) {
        return hal::rf_decode_sub_cli(argc - 2, argv + 2);
    }
    // NFC half of the same CAP: probe the ST25R3916 over its own spidev CS.
    if (argc >= 2 && std::strcmp(argv[1], "--st25r-detect") == 0) {
        return platform::st25r_detect_main(argc - 2, argv + 2);
    }
    // GPS (CAP LORA): scan the HAT UART for NMEA across baud rates.
    if (argc >= 2 && std::strcmp(argv[1], "--gps-detect") == 0) {
        return platform::gps_detect_main(argc - 2, argv + 2);
    }
    // BLE advertisement spam: re-exec with a target family and optional hci index.
    if (argc >= 2 && std::strcmp(argv[1], "--ble-spam") == 0) {
        return platform::ble_spam_main(argc - 2, argv + 2);
    }
    // RFID (ISO14443A via the CAP's ST25R3916): read / dump / write / emulate.
    if (argc >= 2 && std::strcmp(argv[1], "--rfid-read") == 0) {
        return platform::rfid_read_main(argc - 2, argv + 2);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--rfid-dump") == 0) {
        return platform::rfid_dump_main(argc - 2, argv + 2);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--rfid-write") == 0) {
        return platform::rfid_write_main(argc - 2, argv + 2);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--rfid-emulate") == 0) {
        return platform::rfid_emulate_main(argc - 2, argv + 2);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--rfid-clone") == 0) {
        return platform::rfid_clone_main(argc - 2, argv + 2);
    }
    // Wi-Fi wardriving: scan + gpsd fix -> WiGLE CSV for wigle.net upload.
    if (argc >= 2 && std::strcmp(argv[1], "--wardrive") == 0) {
        return platform::wardrive_main(argc - 2, argv + 2);
    }
    // Evil Portal: AP + DNS-hijacked captive portal capturing credentials
    // (Linux port of the ESP32 Bruce feature; see platform/evil_portal.h).
    if (argc >= 2 && std::strcmp(argv[1], "--evil-portal") == 0) {
        return platform::evil_portal_main(argc - 2, argv + 2);
    }
    // Internal passthrough for root-needing catalog rows that are not native
    // re-execs (shell scripts, responder): `sudo -n <self> --root-sh <script>`.
    // See model::launch_argv and platform/root_sh.h.
    if (argc >= 2 && std::strcmp(argv[1], "--root-sh") == 0) {
        return platform::root_sh_main(argc - 2, argv + 2);
    }

    app::Application application;
    return application.run();
}
