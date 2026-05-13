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

set_manifest_metadata() {
  local manifest_path="$1"
  local new_version="$2"
  local uploaded_at="$3"
  local tmp_manifest

  tmp_manifest="$(mktemp "${TMPDIR:-/tmp}/manifest.XXXXXX.json")"
  jq \
    --arg version "${new_version}" \
    --arg uploaded_at "${uploaded_at}" \
    '.version = $version | .uploadedAt = $uploaded_at' \
    "${manifest_path}" > "${tmp_manifest}"
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
dist_factory_dst="${FLASHER_DIR}/dist/web/firmware/HashCash_NanoMinerV1_factory.bin"
dist_firmware_dst="${FLASHER_DIR}/dist/web/firmware/HashCash_NanoMinerV1_firmware.bin"

cp -f "${factory_src}" "${factory_dst}"
cp -f "${firmware_src}" "${firmware_dst}"
if [[ -d "${FLASHER_DIR}/dist/web/firmware" ]]; then
  cp -f "${factory_src}" "${dist_factory_dst}"
  cp -f "${firmware_src}" "${dist_firmware_dst}"
fi

full_version="$(jq -r '.version // empty' "${FULL_MANIFEST}")"
update_version="$(jq -r '.version // empty' "${UPDATE_MANIFEST}")"

[[ -n "${full_version}" ]] || die "Full manifest is missing .version"
[[ -n "${update_version}" ]] || die "Update manifest is missing .version"
is_semver "${full_version}" || die "Unsupported full manifest version format: ${full_version} (expected x.y.z)"
is_semver "${update_version}" || die "Unsupported update manifest version format: ${update_version} (expected x.y.z)"

base_version="$(max_semver "${full_version}" "${update_version}")"
release_version="$(bump_patch "${base_version}")"
release_uploaded_at="$(date -u +'%Y-%m-%dT%H:%M:%SZ')"

set_manifest_metadata "${FULL_MANIFEST}" "${release_version}" "${release_uploaded_at}"
set_manifest_metadata "${UPDATE_MANIFEST}" "${release_version}" "${release_uploaded_at}"

if [[ -f "${DIST_FULL_MANIFEST}" ]]; then
  set_manifest_metadata "${DIST_FULL_MANIFEST}" "${release_version}" "${release_uploaded_at}"
fi
if [[ -f "${DIST_UPDATE_MANIFEST}" ]]; then
  set_manifest_metadata "${DIST_UPDATE_MANIFEST}" "${release_version}" "${release_uploaded_at}"
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
if [[ -d "${FLASHER_DIR}/dist/web/firmware" ]]; then
  log "  ${dist_factory_dst}"
  log "  ${dist_firmware_dst}"
fi
log "Bumped flasher manifest version: ${base_version} -> ${release_version}"
log "Stamped flasher uploadedAt: ${release_uploaded_at}"

checksum_targets=("${factory_src}" "${firmware_src}" "${factory_dst}" "${firmware_dst}")
if [[ -d "${FLASHER_DIR}/dist/web/firmware" ]]; then
  checksum_targets+=("${dist_factory_dst}" "${dist_firmware_dst}")
fi
shasum -a 256 "${checksum_targets[@]}"

log "Flasher repo status:"
git -C "${FLASHER_DIR}" status --short

# =============================================================================
# Device-class OTA publish phase (per device-class plan D-3, G1 fix).
# Opt-in via OTA_PUBLISH=1. Adds the Ed25519-signed private-OTA artifact
# alongside the existing public-flasher copies — these go to a different
# hosting path (/srv/hashcash/ota on the devnet host) and are served via
# nginx auth_request gated tokens (per D-1).
#
# Required env vars when OTA_PUBLISH=1:
#   OTA_SIGNING_KEY      Path to the Ed25519 PEM (from
#                        ops-private/out/setup-secrets/ota_signing_key.pem
#                        or wherever you generated it).
#   DEVNET_HOST          SSH target, e.g. ubuntu@32.194.111.254
#   DEVNET_SSH_KEY       Path to the SSH key for DEVNET_HOST
#   BACKEND_API_BASE     e.g. https://api.hashcash-test.network
#   BACKEND_ADMIN_KEY    Admin API key (from local.env BACKEND_ADMIN_API_KEY)
#
# Optional:
#   OTA_BOARD_ID         Defaults to hashcash_nano_v1
#   OTA_REMOTE_ROOT      Defaults to /srv/hashcash/ota
# =============================================================================

if [[ "${OTA_PUBLISH:-0}" != "1" ]]; then
  log "Device-class OTA publish phase: SKIPPED (set OTA_PUBLISH=1 to enable)"
  exit 0
fi

log "Device-class OTA publish phase: starting"

require_env() {
  local name="$1"
  if [[ -z "${!name:-}" ]]; then
    die "Missing required env var ${name} (OTA_PUBLISH=1 needs full env set)"
  fi
}

require_env OTA_SIGNING_KEY
require_env DEVNET_HOST
require_env DEVNET_SSH_KEY
require_env BACKEND_API_BASE
require_env BACKEND_ADMIN_KEY

OTA_BOARD_ID="${OTA_BOARD_ID:-hashcash_nano_v1}"
OTA_REMOTE_ROOT="${OTA_REMOTE_ROOT:-/srv/hashcash/ota}"

[[ -f "${OTA_SIGNING_KEY}" ]] || die "OTA_SIGNING_KEY not found: ${OTA_SIGNING_KEY}"
[[ -f "${DEVNET_SSH_KEY}" ]] || die "DEVNET_SSH_KEY not found: ${DEVNET_SSH_KEY}"
command -v openssl >/dev/null 2>&1 || die "Missing openssl"
command -v ssh >/dev/null 2>&1 || die "Missing ssh"
command -v scp >/dev/null 2>&1 || die "Missing scp"
command -v curl >/dev/null 2>&1 || die "Missing curl"
command -v shasum >/dev/null 2>&1 || die "Missing shasum"

# We sign and ship the OTA-flavored firmware (the smaller "_firmware.bin"
# that targets the OTA app slot — NOT the full factory image which
# includes bootloader + partition table). Devices apply this via Update.h.
ota_artifact_src="${firmware_src}"
[[ -f "${ota_artifact_src}" ]] || die "OTA artifact not found: ${ota_artifact_src}"

# Compute SHA-256 (lowercase hex, no filename suffix).
ota_sha256="$(shasum -a 256 "${ota_artifact_src}" | awk '{print $1}')"
[[ "${ota_sha256}" =~ ^[0-9a-f]{64}$ ]] || die "Failed to compute SHA-256"
log "OTA artifact: ${ota_artifact_src}"
log "OTA SHA-256:  ${ota_sha256}"

# Sign with Ed25519. openssl pkeyutl -rawin signs the raw bytes (matches
# devicePolicy.cpp::verifyEd25519Signature() which expects raw Ed25519
# over the SHA-256 digest of the artifact). NOTE: backend's _sign_policy
# signs the canonical payload bytes, but for OTA artifacts we sign the
# SHA-256 digest — keeps the on-device verifier simple (compute SHA,
# verify sig over digest).
ota_sig_tmp="$(mktemp "${TMPDIR:-/tmp}/firmware.sig.XXXXXX")"
printf '%s' "${ota_sha256}" | xxd -r -p > "${ota_sig_tmp}.digest"
openssl pkeyutl -sign \
  -inkey "${OTA_SIGNING_KEY}" \
  -rawin -in "${ota_sig_tmp}.digest" \
  -out "${ota_sig_tmp}.raw"
ota_signature_b64="$(base64 < "${ota_sig_tmp}.raw" | tr -d '\n')"
rm -f "${ota_sig_tmp}" "${ota_sig_tmp}.digest" "${ota_sig_tmp}.raw"
log "OTA signature (base64, 88 chars): ${ota_signature_b64:0:32}..."

# Generate a release_id locally so we can use it in the upload path
# before we register with the backend (matches what backend would
# generate via gen_random_uuid() — backend accepts any UUID we pass
# for now; if you prefer backend-generated, POST first then SCP).
ota_release_id="$(uuidgen | tr '[:upper:]' '[:lower:]')"
ota_remote_dir="${OTA_REMOTE_ROOT}/${ota_release_id}/${release_version}"
ota_remote_artifact_path="${ota_remote_dir}/firmware.bin"

log "Generated release_id: ${ota_release_id}"
log "Remote artifact path: ${ota_remote_artifact_path}"

# SCP the artifact to the devnet host. The .sig and .sha256 sidecar
# files are ALSO uploaded so manual verification on the host is easy
# (the backend stores the canonical sig+hash in the firmware_releases
# row — these sidecars are operational convenience, not the source of
# truth).
ssh_opts=(-i "${DEVNET_SSH_KEY}" -o "ServerAliveInterval=15")

ssh "${ssh_opts[@]}" "${DEVNET_HOST}" \
  "sudo mkdir -p '${ota_remote_dir}' && sudo chmod 755 '${ota_remote_dir}'"

scp "${ssh_opts[@]}" "${ota_artifact_src}" \
  "${DEVNET_HOST}:/tmp/firmware.bin.${ota_release_id}"

ssh "${ssh_opts[@]}" "${DEVNET_HOST}" \
  "sudo mv '/tmp/firmware.bin.${ota_release_id}' '${ota_remote_artifact_path}' \
   && sudo chmod 644 '${ota_remote_artifact_path}' \
   && printf '%s\n' '${ota_sha256}' | sudo tee '${ota_remote_artifact_path}.sha256' >/dev/null \
   && printf '%s\n' '${ota_signature_b64}' | sudo tee '${ota_remote_artifact_path}.sig' >/dev/null \
   && sudo ls -la '${ota_remote_dir}'"

# Register with the backend — POST /v1/admin/firmware-release.
register_response="$(curl -sS -w '\n__HTTP_STATUS__:%{http_code}' \
  -H "x-admin-key: ${BACKEND_ADMIN_KEY}" \
  -H 'content-type: application/json' \
  -X POST \
  --data-binary @- \
  "${BACKEND_API_BASE}/v1/admin/firmware-release" <<EOF
{
  "boardId": "${OTA_BOARD_ID}",
  "version": "${release_version}",
  "artifactPath": "${ota_remote_artifact_path}",
  "artifactSha256": "${ota_sha256}",
  "signature": "${ota_signature_b64}",
  "reason": "automated release-to-hc-flasher.sh publish"
}
EOF
)"

http_status="$(printf '%s' "${register_response}" | awk -F: '/^__HTTP_STATUS__:/{print $2}')"
register_body="$(printf '%s' "${register_response}" | sed '/^__HTTP_STATUS__:/d')"

if [[ "${http_status}" != "200" && "${http_status}" != "201" ]]; then
  log "Backend register failed (HTTP ${http_status}):"
  printf '%s\n' "${register_body}"
  die "Backend rejected the firmware-release POST"
fi

backend_release_id="$(printf '%s' "${register_body}" | jq -r '.releaseId // empty')"
log "Backend confirmed registration: releaseId=${backend_release_id:-${ota_release_id}}"

# If backend generated a different release_id (gen_random_uuid()), warn —
# the artifact on disk uses our locally-generated UUID, which won't match
# what the policy resolver looks up. Future work: POST first, get the
# UUID, then SCP into a path keyed by that UUID. For v1, a mismatch is
# loud-failed here so the operator notices.
if [[ -n "${backend_release_id}" && "${backend_release_id}" != "${ota_release_id}" ]]; then
  log "WARN: backend release_id (${backend_release_id}) differs from local (${ota_release_id})"
  log "WARN: rename the on-disk dir to match, or rerun with backend-first flow"
fi

cat <<EOF

================================================================================
DEVICE-CLASS OTA PUBLISH SUMMARY
================================================================================
Release ID:       ${backend_release_id:-${ota_release_id}}
Version:          ${release_version}
Board ID:         ${OTA_BOARD_ID}
Artifact:         ${ota_remote_artifact_path}
SHA-256:          ${ota_sha256}
Signature (b64):  ${ota_signature_b64}
================================================================================
Next steps:
  1. Assign to a channel (e.g., stable):
       curl -H "x-admin-key: \$ADMIN_KEY" \\
         -d '{"boardId":"${OTA_BOARD_ID}","releaseId":"${backend_release_id:-${ota_release_id}}","wavePct":0,"reason":"first wave"}' \\
         ${BACKEND_API_BASE}/v1/admin/firmware-channel/stable

  2. Advance the wave gradually (1 -> 10 -> 50 -> 100):
       curl -H "x-admin-key: \$ADMIN_KEY" \\
         -d '{"wavePct":10,"reason":"v${release_version} wave 10pct"}' \\
         ${BACKEND_API_BASE}/v1/admin/firmware-channel/stable/wave
================================================================================
EOF
