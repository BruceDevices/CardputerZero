/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Compile-time guard for cross builds: the libc headers being compiled against
 * must be the BSP sysroot's, i.e. the same glibc the binary links and runs
 * against. Ubuntu's gcc-cross puts the host's /usr/aarch64-linux-gnu/include
 * (libc6-dev-arm64-cross, glibc 2.43) on the default search path, which once
 * let 2.43 headers pair with the sysroot's 2.41 libc. Since glibc 2.42 the
 * termios B* constants equal the numeric baud (B115200 == 115200U) while
 * cfsetispeed@GLIBC_2.17 expects the legacy 0010002 encoding, so every speed
 * change silently failed (the GPS probe never left 9600 baud).
 *
 * The cp0 cross toolchain (cmake/toolchains/cp0-aarch64-linux-gnu.cmake) adds
 * CP0_TOOLCHAIN_MANAGED_FLAGS and CP0_SYSROOT_GLIBC_MINOR (read from the
 * sysroot's features.h) on every configure. Under that toolchain the checks
 * fail closed; native/desktop builds define neither and skip them.
 */

#if defined(__linux__) && defined(CP0_TOOLCHAIN_MANAGED_FLAGS)

#include <termios.h>

#if !defined(CP0_SYSROOT_GLIBC_MINOR)
#error "cp0 toolchain did not provide CP0_SYSROOT_GLIBC_MINOR (sysroot features.h unreadable?)"
#endif

#if !defined(__GLIBC__)
#error "cp0 cross build expects glibc headers from the BSP sysroot"
#endif

static_assert(__GLIBC_MINOR__ == CP0_SYSROOT_GLIBC_MINOR,
              "libc headers do not come from the BSP sysroot (glibc minor version mismatch): "
              "check the -nostdinc/-isystem setup in cmake/toolchains/cp0-aarch64-linux-gnu.cmake");

#if defined(__aarch64__) && __GLIBC__ == 2 && CP0_SYSROOT_GLIBC_MINOR < 42
// aarch64 + pre-2.42 sysroot glibc: B* must be the legacy CBAUD codes that
// cfset*speed@GLIBC_2.17 expects.
static_assert(B115200 == 0010002, "termios B115200 does not match the sysroot glibc's legacy encoding");
#endif

#endif // __linux__ && CP0_TOOLCHAIN_MANAGED_FLAGS

namespace platform {
// Keeps the TU non-empty for -Wpedantic builds.
extern const int kSysrootAbiGuard = 1;
} // namespace platform
