#!/usr/bin/env bash
set -euo pipefail

log() {
  printf '[%s] %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')" "$*"
}

die() {
  log "ERROR: $*"
  exit 1
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LILY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
ENV_NAME="${1:-HashCash_NanoMinerV1}"
FLASHER_DIR="${2:-$(cd "${LILY_ROOT}/.." && pwd)/hc-flasher}"

command -v pio >/dev/null 2>&1 || die "Missing PlatformIO CLI (pio)"
command -v jq >/dev/null 2>&1 || die "Missing jq"

[[ -f "${LILY_ROOT}/platformio.ini" ]] || die "Invalid LilyMiner root: ${LILY_ROOT}"
[[ -d "${FLASHER_DIR}" ]] || die "Flasher repo not found: ${FLASHER_DIR}"
[[ -d "${FLASHER_DIR}/web/firmware" ]] || die "Missing firmware directory: ${FLASHER_DIR}/web/firmware"

log "Building ${ENV_NAME} in ${LILY_ROOT}"
(
  cd "${LILY_ROOT}"
  pio run -e "${ENV_NAME}"
)

factory_src="$(ls -1t "${LILY_ROOT}"/firmware/*/"${ENV_NAME}_factory.bin" 2>/dev/null | head -n 1 || true)"
firmware_src="$(ls -1t "${LILY_ROOT}"/firmware/*/"${ENV_NAME}_firmware.bin" 2>/dev/null | head -n 1 || true)"

[[ -n "${factory_src}" && -f "${factory_src}" ]] || die "Factory binary not found for ${ENV_NAME}"
[[ -n "${firmware_src}" && -f "${firmware_src}" ]] || die "Firmware binary not found for ${ENV_NAME}"

factory_dst="${FLASHER_DIR}/web/firmware/HashCash_NanoMinerV1_factory.bin"
firmware_dst="${FLASHER_DIR}/web/firmware/HashCash_NanoMinerV1_firmware.bin"

cp -f "${factory_src}" "${factory_dst}"
cp -f "${firmware_src}" "${firmware_dst}"

full_version="$(jq -r '.version // empty' "${FLASHER_DIR}/web/manifest_hashcash_nanominerv1_full.json")"
update_version="$(jq -r '.version // empty' "${FLASHER_DIR}/web/manifest_hashcash_nanominerv1_update.json")"

if [[ -n "${full_version}" ]]; then
  perl -0pi -e "s|manifest_hashcash_nanominerv1_full\\.json\\?v=[0-9A-Za-z._-]+|manifest_hashcash_nanominerv1_full.json?v=${full_version}|g" \
    "${FLASHER_DIR}/index.html" "${FLASHER_DIR}/dist/index.html"
fi

if [[ -n "${update_version}" ]]; then
  perl -0pi -e "s|manifest_hashcash_nanominerv1_update\\.json\\?v=[0-9A-Za-z._-]+|manifest_hashcash_nanominerv1_update.json?v=${update_version}|g" \
    "${FLASHER_DIR}/index.html" "${FLASHER_DIR}/dist/index.html"
fi

log "Release sync complete"
log "Source binaries:"
log "  ${factory_src}"
log "  ${firmware_src}"
log "Destination binaries:"
log "  ${factory_dst}"
log "  ${firmware_dst}"

shasum -a 256 "${factory_src}" "${firmware_src}" "${factory_dst}" "${firmware_dst}"

log "Flasher repo status:"
git -C "${FLASHER_DIR}" status --short
