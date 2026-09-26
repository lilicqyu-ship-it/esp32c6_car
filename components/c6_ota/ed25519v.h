/*
 * ed25519v.h - ed25519 signature VERIFICATION only (coding-plan decision C3).
 *
 * Self-contained port of the RFC 8032 reference flow on top of an 8x32-bit
 * limb field arithmetic (rv32 has no __uint128_t).  Non-constant-time on
 * purpose: verification operates on public data only.
 */
#ifndef C6_ED25519V_H
#define C6_ED25519V_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0 = valid signature, -1 = invalid (any step) */
int c6_ed25519_verify(const uint8_t pk[32], const uint8_t sig[64],
                      const uint8_t *msg, size_t msg_len);

#ifdef __cplusplus
}
#endif

#endif /* C6_ED25519V_H */
