/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"
#include "cc1101.h"

#include <cstring>

int main(int argc, char** argv) {
    // Hidden subcommand used by the "Use CC1101 CAP" menu entry: the app
    // re-execs itself to probe the CAP over spidev and stream the report.
    if (argc >= 2 && std::strcmp(argv[1], "--cc1101-detect") == 0) {
        return platform::cc1101_detect_main(argc - 2, argv + 2);
    }

    app::Application application;
    return application.run();
}
