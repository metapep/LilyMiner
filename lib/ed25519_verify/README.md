# ed25519_verify (vendor slot)

This directory is reserved for a vendored Ed25519 signature **verifier**
that `src/drivers/devicePolicy.cpp` falls back to when the espressif32
framework's mbedtls build does NOT enable `MBEDTLS_PK_ED25519`.

## When you need this

Run a `pio run -e HashCash_NanoMinerV1` build first. If it succeeds and
device-side OTA signature verification works, leave this directory empty
— mbedtls handled it.

If the build fails at link time with errors mentioning Ed25519 / EdDSA,
or if `verifyEd25519Signature()` logs `mbedtls Ed25519 unavailable;
vendor a verifier`, you need to vendor a verifier here.

## Recommended verifier: orlp/ed25519

Public-domain, single-purpose, used by many embedded projects.

```bash
cd lib/ed25519_verify
git clone https://github.com/orlp/ed25519.git src
# Optional: keep only verify.c + headers; you don't need keygen/sign on-device.
```

PlatformIO auto-discovers `lib/ed25519_verify/` and links it.

## Wiring into devicePolicy.cpp

After vendoring, replace the body of `verifyEd25519Signature()` in
`src/drivers/devicePolicy.cpp` (the `#else` branch of the
`MBEDTLS_PK_HAVE_ECC_KEYS` guard) with:

```cpp
extern "C" int ed25519_verify(const unsigned char *signature,
                              const unsigned char *message,
                              size_t message_len,
                              const unsigned char *public_key);

uint8_t pubkey[32];
if (!decodeHexFixed(POLICY_SIGNING_PUBKEY, pubkey, 32)) return false;
return ed25519_verify(sig, msg, msgLen, pubkey) == 1;
```

Add `-D USE_VENDORED_ED25519=1` to the production env in `platformio.ini`
to skip the mbedtls path entirely.

## Why this isn't pre-shipped

Vendoring a crypto library blindly into the repo without operator
review is a security anti-pattern. The existing mbedtls path is the
preferred path; this slot exists for the case where it doesn't work.
