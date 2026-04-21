#!/usr/bin/env bash
set -euo pipefail

log() {
  printf '[%s] %s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')" "$*"
}

die() {
  log "ERROR: $*"
  exit 1
}

is_semver() {
  local version="$1"
  [[ "${version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
}

bump_patch() {
  local version="$1"
  local major minor patch
  IFS='.' read -r major minor patch <<<"${version}"
  printf '%s\n' "${major}.${minor}.$((patch + 1))"
}

max_semver() {
  local left="$1"
  local right="$2"
  printf '%s\n%s\n' "${left}" "${right}" | sort -V | tail -n 1
}

set_manifest_version() {
  local manifest_path="$1"
  local new_version="$2"
  local tmp_manifest

  tmp_manifest="$(mktemp "${TMPDIR:-/tmp}/manifest.XXXXXX.json")"
  jq --arg version "${new_version}" '.version = $version' "${manifest_path}" > "${tmp_manifest}"
  mv "${tmp_manifest}" "${manifest_path}"
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LILY_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
ENV_NAME="${1:-HashCash_NanoMinerV1}"
FLASHER_DIR="${2:-$(cd "${LILY_ROOT}/.." && pwd)/hc-flasher}"

FULL_MANIFEST="${FLASHER_DIR}/web/manifest_hashcash_nanominerv1_full.json"
UPDATE_MANIFEST="${FLASHER_DIR}/web/manifest_hashcash_nanominerv1_update.json"
DIST_FULL_MANIFEST="${FLASHER_DIR}/dist/web/manifest_hashcash_nanominerv1_full.json"
DIST_UPDATE_MANIFEST="${FLASHER_DIR}/dist/web/manifest_hashcash_nanominerv1_update.json"

command -v pio >/dev/null 2>&1 || die "Missing PlatformIO CLI (pio)"
command -v jq >/dev/null 2>&1 || die "Missing jq"

[[ -f "${LILY_ROOT}/platformio.ini" ]] || die "Invalid LilyMiner root: ${LILY_ROOT}"
[[ -d "${FLASHER_DIR}" ]] || die "Flasher repo not found: ${FLASHER_DIR}"
[[ -d "${FLASHER_DIR}/web/firmware" ]] || die "Missing firmware directory: ${FLASHER_DIR}/web/firmware"
[[ -f "${FULL_MANIFEST}" ]] || die "Missing full manifest: ${FULL_MANIFEST}"
[[ -f "${UPDATE_MANIFEST}" ]] || die "Missing update manifest: ${UPDATE_MANIFEST}"

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

full_version="$(jq -r '.version // empty' "${FULL_MANIFEST}")"
update_version="$(jq -r '.version // empty' "${UPDATE_MANIFEST}")"

[[ -n "${full_version}" ]] || die "Full manifest is missing .version"
[[ -n "${update_version}" ]] || die "Update manifest is missing .version"
is_semver "${full_version}" || die "Unsupported full manifest version format: ${full_version} (expected x.y.z)"
is_semver "${update_version}" || die "Unsupported update manifest version format: ${update_version} (expected x.y.z)"

base_version="$(max_semver "${full_version}" "${update_version}")"
release_version="$(bump_patch "${base_version}")"

set_manifest_version "${FULL_MANIFEST}" "${release_version}"
set_manifest_version "${UPDATE_MANIFEST}" "${release_version}"

if [[ -f "${DIST_FULL_MANIFEST}" ]]; then
  set_manifest_version "${DIST_FULL_MANIFEST}" "${release_version}"
fi
if [[ -f "${DIST_UPDATE_MANIFEST}" ]]; then
  set_manifest_version "${DIST_UPDATE_MANIFEST}" "${release_version}"
fi

html_targets=()
[[ -f "${FLASHER_DIR}/index.html" ]] && html_targets+=("${FLASHER_DIR}/index.html")
[[ -f "${FLASHER_DIR}/dist/index.html" ]] && html_targets+=("${FLASHER_DIR}/dist/index.html")

if (( ${#html_targets[@]} > 0 )); then
  perl -0pi -e "s|manifest_hashcash_nanominerv1_full\\.json\\?v=[0-9A-Za-z._-]+|manifest_hashcash_nanominerv1_full.json?v=${release_version}|g" \
    "${html_targets[@]}"
  perl -0pi -e "s|manifest_hashcash_nanominerv1_update\\.json\\?v=[0-9A-Za-z._-]+|manifest_hashcash_nanominerv1_update.json?v=${release_version}|g" \
    "${html_targets[@]}"
fi

log "Release sync complete"
log "Source binaries:"
log "  ${factory_src}"
log "  ${firmware_src}"
log "Destination binaries:"
log "  ${factory_dst}"
log "  ${firmware_dst}"
log "Bumped flasher manifest version: ${base_version} -> ${release_version}"

shasum -a 256 "${factory_src}" "${firmware_src}" "${factory_dst}" "${firmware_dst}"

log "Flasher repo status:"
git -C "${FLASHER_DIR}" status --short
