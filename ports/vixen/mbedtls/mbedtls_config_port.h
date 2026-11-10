/* SPDX-License-Identifier: MIT */
#ifndef MICROPY_INCLUDED_MBEDTLS_CONFIG_H
#define MICROPY_INCLUDED_MBEDTLS_CONFIG_H

// Set MicroPython-specific options.
#define MICROPY_MBEDTLS_CONFIG_BARE_METAL (1)

// Include common mbedtls configuration.
#include "extmod/mbedtls/mbedtls_config_common.h"

// VIXEN-32 has no real-time clock: there is no wall-clock date to check a
// certificate's validity period against, so mbedtls is built without time.
#undef MBEDTLS_HAVE_TIME
#undef MBEDTLS_HAVE_TIME_DATE

// cryptolib.aes(key, MODE_CTR, nonce) (MICROPY_PY_CRYPTOLIB_CTR), e.g. for
// aes128-ctr in an SSH server.
#define MBEDTLS_CIPHER_MODE_CTR

#endif /* MICROPY_INCLUDED_MBEDTLS_CONFIG_H */
