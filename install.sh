#!/usr/bin/env bash
# Download and install the latest Bruce CardputerZero beta build.
# Run it ON the CardputerZero:
#
#   curl -fsSL https://github.com/BruceDevices/CardputerZero/releases/download/beta/install.sh | bash
#
# Overrides: DEB_URL=<url> to install another package, KEEP_DEB=1 to keep the
# downloaded file in the current directory.
set -euo pipefail

DEB_NAME="bruce-cardputerzero_beta_arm64.deb"
DEB_URL="${DEB_URL:-https://github.com/BruceDevices/CardputerZero/releases/download/beta/${DEB_NAME}}"

log() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

arch="$(dpkg --print-architecture 2>/dev/null || uname -m)"
case "$arch" in
  arm64|aarch64) ;;
  *) die "this package is for arm64 (CardputerZero); this machine is '$arch'." ;;
esac

command -v apt-get >/dev/null || die "apt-get not found - is this a Debian-based system?"

if [[ $EUID -eq 0 ]]; then SUDO=""; else
  command -v sudo >/dev/null || die "run as root or install sudo."
  SUDO="sudo"
fi

# World-readable temp dir so apt's sandbox user (_apt) can read the .deb.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
chmod 755 "$tmp"
deb="$tmp/$DEB_NAME"

log "Downloading $DEB_URL"
if command -v curl >/dev/null; then
  curl -fL --retry 3 -o "$deb" "$DEB_URL"
elif command -v wget >/dev/null; then
  wget -O "$deb" "$DEB_URL"
else
  die "need curl or wget to download the package."
fi
chmod 644 "$deb"

# apt (not dpkg -i) so the Recommends - the CLI tools the app drives - come too.
# --reinstall: every beta build carries the same version number, so without it
# apt would see "already the newest version" and skip the update.
log "Installing (sudo apt install)"
$SUDO apt-get update
$SUDO apt-get install -y --reinstall "$deb"

if [[ "${KEEP_DEB:-0}" == "1" ]]; then
  cp "$deb" "./$DEB_NAME"
  log "Kept a copy at ./$DEB_NAME"
fi

log "Done. Launch \"BruceCardputerZero\" from APPLaunch, or run: bruce_cardputerzero"
