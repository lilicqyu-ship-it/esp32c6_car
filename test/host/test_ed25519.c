/*
 * test_ed25519.c - RFC 8032 section 7.3 TEST 1/2 vectors (positive + tamper)
 * plus a signed-message round trip using the RFC keypair.
 */
#include <stdio.h>
#include <string.h>

#include "../../components/c6_ota/ed25519v.h"
#include "minunit.h"

static int unhex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = strlen(hex) / 2;
    int i;

    if (n > cap)
    {
        return -1;
    }
    for (i = 0; i < (int)n; i++)
    {
        unsigned v = 0;
        for (int j = 0; j < 2; j++)
        {
            char c = hex[i * 2 + j];
            v <<= 4;
            if (c >= '0' && c <= '9') { v |= (unsigned)(c - '0'); }
            else if (c >= 'a' && c <= 'f') { v |= (unsigned)(c - 'a' + 10); }
            else { return -1; }
        }
        out[i] = (uint8_t)v;
    }
    return (int)n;
}

/* RFC 8032 test 1: empty message */
static int test_rfc8032_vector1(void)
{
    uint8_t pk[32], sig[64];
    const char *pk_hex  = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
    const char *sig_hex = "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                          "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";

    MU_CHECK_EQ(unhex(pk_hex, pk, sizeof(pk)), 32);
    MU_CHECK_EQ(unhex(sig_hex, sig, sizeof(sig)), 64);
    MU_CHECK_EQ(c6_ed25519_verify(pk, sig, NULL, 0), 0);

    sig[0] ^= 0x01;
    MU_CHECK_EQ(c6_ed25519_verify(pk, sig, NULL, 0), -1);
    sig[0] ^= 0x01;
    MU_CHECK_EQ(c6_ed25519_verify(pk, sig, NULL, 0), 0);
    return 0;
}

/* RFC 8032 test 2: one byte 0x72 */
static int test_rfc8032_vector2(void)
{
    uint8_t pk[32], sig[64], msg[1];
    const char *pk_hex  = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";
    const char *sig_hex = "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
                          "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00";

    MU_CHECK_EQ(unhex(pk_hex, pk, sizeof(pk)), 32);
    MU_CHECK_EQ(unhex(sig_hex, sig, sizeof(sig)), 64);
    msg[0] = 0x72;
    MU_CHECK_EQ(c6_ed25519_verify(pk, sig, msg, 1), 0);

    /* tampered message must fail */
    msg[0] = 0x73;
    MU_CHECK_EQ(c6_ed25519_verify(pk, sig, msg, 1), -1);
    return 0;
}

/* dev keypair end-to-end: signature produced by tools/ed25519_ref.py
 * (tools/keys/ed25519_dev.seed, message "SmartDrive dev probe") */
static int test_dev_keypair_roundtrip(void)
{
    static const char *pub_hex =
        "74510999f74248144463958abfbdd6fc559f9ab247c311fb58f74c5d6d468b5f";
    static const char *sig_hex =
        "b1353e2f6af3a61b2c1f7132511210c04f7a3294a8ed35dab93f850b615abd58"
        "ec298a9e101ce7bad637cb46f17b8b5029e1f0e268ee609b8a5837e402c71506";
    const char *msg = "SmartDrive dev probe";
    uint8_t pk[32], sig[64];

    MU_CHECK_EQ(unhex(pub_hex, pk, sizeof(pk)), 32);
    MU_CHECK_EQ(unhex(sig_hex, sig, sizeof(sig)), 64);
    MU_CHECK_EQ(c6_ed25519_verify(pk, sig, (const uint8_t *)msg, strlen(msg)), 0);

    /* flip one message byte (same length): must fail */
    MU_CHECK_EQ(c6_ed25519_verify(pk, sig, (const uint8_t *)"SmertDrive dev probe",
                                  strlen(msg)), -1);
    return 0;
}

/* garbage public keys must be rejected (decode failure paths) */
static int test_bad_pubkey_rejected(void)
{
    uint8_t pk[32] = { 0 };
    uint8_t sig[64] = { 0 };

    MU_CHECK_EQ(c6_ed25519_verify(pk, sig, NULL, 0), -1);   /* y=0 not on curve */
    memset(pk, 0xFF, sizeof(pk));                            /* y >= p          */
    MU_CHECK_EQ(c6_ed25519_verify(pk, sig, NULL, 0), -1);
    return 0;
}

int main(void)
{
    MU_RUN(test_rfc8032_vector1);
    MU_RUN(test_rfc8032_vector2);
    MU_RUN(test_dev_keypair_roundtrip);
    MU_RUN(test_bad_pubkey_rejected);
    MU_REPORT("ed25519");
    return (mu_failed != 0) ? 1 : 0;
}
