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
#include "gps_config.h"
#include "gps_tracker.h"
#include "gpx_writer.h"
#include "root_sh.h"
#include "rfid_st25r.h"
#include "rfid_mfrc522.h"
#include "wardrive.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

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
    // M5 RFID2 (MFRC522 / WS1850S over I2C): detect / read / dump / write.
    if (argc >= 2 && std::strcmp(argv[1], "--rfid2-detect") == 0) {
        return platform::rfid2_detect_main(argc - 2, argv + 2);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--rfid2-read") == 0) {
        return platform::rfid2_read_main(argc - 2, argv + 2);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--rfid2-dump") == 0) {
        return platform::rfid2_dump_main(argc - 2, argv + 2);
    }
    if (argc >= 2 && std::strcmp(argv[1], "--rfid2-write") == 0) {
        return platform::rfid2_write_main(argc - 2, argv + 2);
    }
    // Bruce GPS Tracker on the console (same core as the GPS Tracker screen):
    // `--gps-tracker [port] [baud]`, GPX to ~/BruceGPS. For SSH testing.
    if (argc >= 2 && std::strcmp(argv[1], "--gps-tracker") == 0) {
        // Missing [port] [baud] come from GPS > Config (~/.bruce/gps.conf) with
        // $GPS_UART / $GPS_BAUD on top; without a saved config the console
        // keeps its LoRa+GNSS Cap default (/dev/serial0 @ 115200) - no CAP
        // detection runs on this path. Shutdown (incl. the bounded storage
        // drain) is handled inside, under the one process-wide deadline.
        const auto eff = model::gps_config::resolve(
            model::gps_config::load(model::gps_config::board_default(/*cap_lora=*/true)));
        std::vector<std::string> args(argv + 2, argv + argc);
        if (args.empty() || args[0].empty()) {
            args.resize(std::max<size_t>(args.size(), 1));
            args[0] = eff.port;
        }
        if (args.size() < 2) {
            args.push_back(std::to_string(eff.baud));
        }
        std::vector<char*> cargs;
        for (auto& a : args) {
            cargs.push_back(a.data());
        }
        cargs.push_back(nullptr);
        return platform::gps::gps_tracker_main(static_cast<int>(args.size()), cargs.data());
    }
    // Bruce Wardriving: `--wardrive wifi|ble|both ...`, Wi-Fi / BLE scans with
    // the in-app GPS fix -> WiGLE 1.6 CSV in ~/BruceWardriving (wigle.net).
    if (argc >= 2 && std::strcmp(argv[1], "--wardrive") == 0) {
        // A missing --port / --baud comes from GPS > Config, as for
        // --gps-tracker: explicit options > $GPS_UART / $GPS_BAUD >
        // ~/.bruce/gps.conf > /dev/serial0 @ 115200. The GPS menu always
        // passes both, so its root child never reads a config from HOME.
        std::vector<std::string> args(argv + 2, argv + argc);
        bool has_port = false;
        bool has_baud = false;
        for (const auto& a : args) {
            has_port = has_port || a == "--port";
            has_baud = has_baud || a == "--baud";
        }
        if (!has_port || !has_baud) {
            const auto eff = model::gps_config::resolve(
                model::gps_config::load(model::gps_config::board_default(/*cap_lora=*/true)));
            std::vector<std::string> opts;
            if (!has_port) {
                opts.insert(opts.end(), {"--port", eff.port});
            }
            if (!has_baud) {
                opts.insert(opts.end(), {"--baud", std::to_string(eff.baud)});
            }
            args.insert(args.begin(), opts.begin(), opts.end());
        }
        std::vector<char*> cargs;
        for (auto& a : args) {
            cargs.push_back(a.data());
        }
        cargs.push_back(nullptr);
        return platform::wardrive_main(static_cast<int>(args.size()), cargs.data());
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

    int rc = 0;
    {
        app::Application application;
        rc = application.run(); // screens (and their GPS trackers) are gone after this
    }
    // Orderly shutdown: finish and join pending GPX writes (Back on slow storage
    // leaves the storage worker running past its screen), bounded by the one
    // process-wide deadline. Unfinished writes are logged and fail the exit.
    if (platform::gps::drain_writers() > 0) {
        return rc != 0 ? rc : 1;
    }
    return rc;
}
