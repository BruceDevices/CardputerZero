#!/usr/bin/env bash
set -euo pipefail

CMAKE_VERSION="3.31.12"
INSTALL_DIR="/opt/cmake-${CMAKE_VERSION}"
ARCHIVE="cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz"
URL="https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/${ARCHIVE}"

echo "Installing CMake ${CMAKE_VERSION}..."

# Download
cd /tmp
wget -q "${URL}" -O "${ARCHIVE}"

# Extract
tar -xzf "${ARCHIVE}"

# Install
sudo rm -rf "${INSTALL_DIR}"
sudo mv "cmake-${CMAKE_VERSION}-linux-x86_64" "${INSTALL_DIR}"

# Configure PATH
sudo ln -sf "${INSTALL_DIR}/bin/cmake" /usr/local/bin/cmake
sudo ln -sf "${INSTALL_DIR}/bin/ctest" /usr/local/bin/ctest
sudo ln -sf "${INSTALL_DIR}/bin/cpack" /usr/local/bin/cpack

# Cleanup
rm -rf "/tmp/${ARCHIVE}"

echo "Installed!"
cmake --version
