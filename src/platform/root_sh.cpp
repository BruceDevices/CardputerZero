/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include "root_sh.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <unistd.h>

namespace platform {

int root_sh_main(int argc, char** argv) {
    if (argc < 1 || !argv[0] || !argv[0][0]) {
        std::fprintf(stderr, "usage: bruce_cardputerzero --root-sh '<script>'\n");
        return 127;
    }
    execl("/bin/sh", "sh", "-c", argv[0], static_cast<char*>(nullptr));
    std::fprintf(stderr, "exec /bin/sh failed: %s\n", std::strerror(errno));
    return 127;
}

} // namespace platform
