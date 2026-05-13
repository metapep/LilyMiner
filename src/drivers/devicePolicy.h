/**
 * Device-class policy client (per device-class plan F-5..F-11).
 *
 * Polls the pool's /api/policy proxy (which forwards to backend
 * /v1/device/policy per P-2b) every POLL_INTERVAL_SECONDS with jitter,
 * verifies Ed25519 signature with the compiled-in POLICY_SIGNING_PUBKEY
 * constant, persists to TSettings.Policy* fields, and exposes the
 * current target hashrate to the mining workers via getCurrentTargetHs().
 *
 * Lifecycle:
 *   - Started after WiFi connects + NTP synced (per F-4 strict gate).
 *   - Runs in a separate task; can poll even before activation completes
 *     so the device knows its class on first activation.
 *   - On signature verify failure or backend unreachable, retains
 *     last-known cap. After POLICY_TTL_SECONDS without success, falls
 *     back to SAFEST_CAP_HS (per F-6 / C13).
 *
 * STATUS (per execution plan): header-only skeleton. The implementation
 * (devicePolicy.cpp), HTTP client wiring, Ed25519 verification, OTA
 * download/verify/install, ESP-IDF rollback integration, and display
 * driver updates remain to land in a follow-up firmware sprint.
 */
#ifndef _DEVICE_POLICY_H_
#define _DEVICE_POLICY_H_

#include <Arduino.h>
#include <stdint.h>

// Returns the policy-derived hashrate cap (Hz). Falls back to
// SAFEST_CAP_HS when the cached policy has aged out
// (now - PolicyFetchedAt > POLICY_TTL_SECONDS) or no policy has ever
// been fetched.
uint32_t getCurrentTargetHs();

// Returns the policy-derived class label (e.g., "J50"), or empty string
// when no policy is cached. Used by display drivers (per F-10).
const char* getCurrentClassId();

// True when the cached policy has been refreshed within
// POLICY_TTL_SECONDS, false otherwise. Mining workers can use this to
// reduce hash rate on extended outages independently of the throttle.
bool isPolicyFresh();

// Start the background policy-fetch task. Idempotent. Called from the
// main setup after WiFi + NTP are ready.
void startPolicyFetchTask();

// Report an OTA lifecycle state to the backend (per F-8 / B-10).
// `state` is one of: downloading, verifying, installing, success, failed.
// Returns true on successful POST, false on transport failure (caller
// can retry with backoff).
bool reportOtaState(const char* releaseId, const char* state, const char* errorCode, uint8_t attempt);

#endif // _DEVICE_POLICY_H_
