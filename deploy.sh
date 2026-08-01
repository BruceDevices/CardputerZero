#!/usr/bin/env bash
set -euo pipefail

WORKFLOW_PRESET="${WORKFLOW_PRESET:-cp0-cross-package}"
REMOTE_USER="${REMOTE_USER:-}"
REMOTE_HOST="${REMOTE_HOST:-}"
REMOTE_DIR="${REMOTE_DIR:-/home/${REMOTE_USER}}"
PACKAGE_GLOB="${PACKAGE_GLOB:-dist/BruceCardputerZero_*_arm64.deb}"

log() {
  printf '\n==> %s\n' "$*"
}

log "Workflow: cmake --workflow --preset ${WORKFLOW_PRESET}"
cmake --workflow --preset "${WORKFLOW_PRESET}"

shopt -s nullglob
packages=( ${PACKAGE_GLOB} )
shopt -u nullglob

if (( ${#packages[@]} == 0 )); then
  printf 'No package matched: %s\n' "${PACKAGE_GLOB}" >&2
  exit 1
fi

latest_package="$(ls -t "${packages[@]}" | head -n 1)"

if [[ -z "${REMOTE_USER}" || -z "${REMOTE_HOST}" ]]; then
  printf 'Package ready: %s\n' "${latest_package}" >&2
  printf 'Set REMOTE_USER and REMOTE_HOST to deploy, for example:\n' >&2
  printf '  REMOTE_USER=pi REMOTE_HOST=192.168.1.10 ./deploy.sh\n' >&2
  exit 1
fi

remote_target="${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_DIR}"

log "Deploy: scp ${latest_package} ${remote_target}"
scp "${latest_package}" "${remote_target}"

# Install over SSH with apt (NOT dpkg -i) so the tools the app shells out to -
# smbclient, iw, bluez, curl, netcat-openbsd, rfkill, iproute2 - get pulled in
# automatically. Set REMOTE_INSTALL=0 to only copy the package.
remote_package="${REMOTE_DIR%/}/$(basename "${latest_package}")"
if [[ "${REMOTE_INSTALL:-1}" == "1" ]]; then
  log "Install: apt install ${remote_package} on ${REMOTE_HOST}"
  ssh -t "${REMOTE_USER}@${REMOTE_HOST}" \
    "sudo apt install -y '${remote_package}'"
  log "Done: installed on ${REMOTE_HOST}"
else
  log "Copied ${remote_package} (skipped install; REMOTE_INSTALL=0)"
  printf 'To install: ssh %s@%s then sudo apt install ./%s\n' \
    "${REMOTE_USER}" "${REMOTE_HOST}" "$(basename "${latest_package}")" >&2
fi
