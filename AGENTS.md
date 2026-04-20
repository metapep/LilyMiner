# AGENTS.md (LilyMiner Firmware Rules)

## Scope
This repository contains LilyMiner firmware sources and release artifacts.

## Firmware Build + Flasher Sync (Mandatory)
- Use `bin/release-to-hc-flasher.sh` for NanoMiner V1 release syncs.
- The script must be the default path for producing new `HashCash_NanoMinerV1` binaries and copying them into `../hc-flasher/web/firmware`.
- Do not manually copy firmware binaries into `hc-flasher` when the script can be used.

## Standard Command
```bash
bin/release-to-hc-flasher.sh
```

## Optional Arguments
- `bin/release-to-hc-flasher.sh <platformio_env> <absolute_or_relative_hc_flasher_path>`
- Default env: `HashCash_NanoMinerV1`
- Default flasher path: `../hc-flasher`

## Verification Expectations
- The script must print SHA-256 checksums for source and destination binaries.
- The script must print `hc-flasher` git status so staged release deltas are visible.
