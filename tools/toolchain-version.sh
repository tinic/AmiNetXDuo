# shellcheck shell=sh
# shellcheck disable=SC2034 # Shared values consumed by the sourcing scripts.
# Toolchain package identity. Keep GCC's upstream version intact; the suffix
# versions AmiNetXDuo packaging independently of upstream releases.
# SPDX-License-Identifier: MIT
TC_GCC_VERSION="16.2.1b"
TC_VENDOR_REVISION="anx.2"
TC_ASSET_VERSION="${TC_GCC_VERSION}-${TC_VENDOR_REVISION}"
TC_NDK_VERSION="3.2"
