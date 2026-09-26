/*
 * ed25519v.c - ed25519 verify-only (RFC 8032), 8x32 limb field arithmetic.
 *
 * Value encoding: fe = 8 uint32 limbs, little-endian, reduced < p after ops.
 * 2^256 == 38 (mod p) drives the multiply fold.  All curve constants come from
 * the generated c6_consts.h - nothing here is hand-typed.
 *
 * Non-constant-time on purpose: verification operates on public data only.
 */
#include "ed25519v.h"

#include <string.h>

#include "c6_consts.h"
#include "sha512.h"

typedef struct
{
    uint32_t v[8];
} fe;

typedef struct
{
    fe X, Y, Z, T;                 /* extended homogeneous coordinates */
} ge;

static const fe FE_P = { C6_FE_P };
static const fe FE_D = { C6_ED_D };
static const fe FE_SQRT_M1 = { C6_ED_SQRT_M1 };
static const fe FE_ONE = { { 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } };
static const fe FE_ZERO = { { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u } };

/* exponent (p-5)/8 = 2^252 - 3, LE limbs */
static const uint32_t EXP_P58[8] = {
    0xFFFFFFFDu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x0FFFFFFFu
};

/* exponent p-2 = 2^255 - 21, LE limbs */
static const uint32_t EXP_PM2[8] = {
    0xFFFFFFEBu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x7FFFFFFFu
};

/* group order L, big-endian byte form */
static const uint8_t SC_L[32] = {
    0xedU, 0xd3U, 0xf5U, 0x5cU, 0x1aU, 0x63U, 0x12U, 0x58U,
    0xd6U, 0x9cU, 0xf7U, 0xa2U, 0xdeU, 0xf9U, 0xdeU, 0x14U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x10U
};

/* ---- field arithmetic -------------------------------------------------------*/

/* conditional subtract of p while r >= p (bounded: r < 2p + 38 after ops) */
static void fe_reduce(fe *r)
{
    int round;

    for (round = 0; round < 3; round++)
    {
        uint32_t borrow = 0u;
        uint32_t diff[8];
        int ge;
        int i;

        ge = 0;
        for (i = 7; i >= 0; i--)
        {
            if (r->v[i] > FE_P.v[i]) { ge = 1; break; }
            if (r->v[i] < FE_P.v[i]) { ge = 0; break; }
            if (i == 0) { ge = 1; }               /* exactly p */
        }
        if (ge == 0)
        {
            break;
        }
        for (i = 0; i < 8; i++)
        {
            uint32_t d = (uint32_t)(r->v[i] - FE_P.v[i] - borrow);
            borrow = (r->v[i] < FE_P.v[i]) ||
                     ((borrow != 0u) && (r->v[i] == FE_P.v[i]));
            diff[i] = d;
        }
        memcpy(r->v, diff, sizeof(diff));
    }
}

static void fe_add(fe *r, const fe *a, const fe *b)
{
    uint64_t c = 0u;
    uint32_t carry = 0u;
    int i;

    for (i = 0; i < 8; i++)
    {
        c = (uint64_t)a->v[i] + (uint64_t)b->v[i] + (uint64_t)carry;
        r->v[i] = (uint32_t)c;
        carry   = (uint32_t)(c >> 32);
    }
    if (carry != 0u)                              /* fold 2^256 == 38 */
    {
        uint64_t t = (uint64_t)r->v[0] + (38ull * (uint64_t)carry);
        r->v[0] = (uint32_t)t;
        carry   = (uint32_t)(t >> 32);
        for (i = 1; (carry != 0u) && (i < 8); i++)
        {
            t = (uint64_t)r->v[i] + (uint64_t)carry;
            r->v[i] = (uint32_t)t;
            carry   = (uint32_t)(t >> 32);
        }
    }
    fe_reduce(r);
}

static void fe_sub(fe *r, const fe *a, const fe *b)
{
    fe t;
    uint32_t borrow = 0u;
    int i;

    /* t = 2p - b, with 2p = 2^256 - 38 */
    for (i = 0; i < 8; i++)
    {
        uint32_t p2 = (i == 0) ? 0xFFFFFFDAu : 0xFFFFFFFFu;
        t.v[i] = (uint32_t)(p2 - b->v[i] - borrow);
        borrow = (p2 < b->v[i]) || ((borrow != 0u) && (p2 == b->v[i]));
    }
    fe_add(r, a, &t);
}

static void fe_mul(fe *r, const fe *a, const fe *b)
{
    uint32_t t[16];
    uint32_t out[8];
    uint64_t acc;
    uint32_t carry;
    int i, j;

    memset(t, 0, sizeof(t));
    for (i = 0; i < 8; i++)
    {
        uint64_t row = 0u;
        for (j = 0; j < 8; j++)
        {
            uint64_t cur = (uint64_t)t[i + j] +
                           ((uint64_t)a->v[i] * (uint64_t)b->v[j]) + row;
            t[i + j] = (uint32_t)cur;
            row      = cur >> 32;
        }
        {
            int k = i + 8;
            while ((row != 0u) && (k < 16))
            {
                uint64_t cur = (uint64_t)t[k] + row;
                t[k] = (uint32_t)cur;
                row  = cur >> 32;
                k++;
            }
        }
    }

    carry = 0u;
    for (i = 0; i < 8; i++)
    {
        acc = (uint64_t)t[i] + (38ull * (uint64_t)t[i + 8]) + (uint64_t)carry;
        out[i] = (uint32_t)acc;
        carry  = (uint32_t)(acc >> 32);
    }
    if (carry != 0u)                              /* fold top: carry*38 */
    {
        uint64_t t2 = (uint64_t)out[0] + (38ull * (uint64_t)carry);
        out[0] = (uint32_t)t2;
        carry  = (uint32_t)(t2 >> 32);
        for (i = 1; (carry != 0u) && (i < 8); i++)
        {
            t2 = (uint64_t)out[i] + (uint64_t)carry;
            out[i] = (uint32_t)t2;
            carry  = (uint32_t)(t2 >> 32);
        }
    }
    memcpy(r->v, out, sizeof(out));
    fe_reduce(r);
}

static void fe_sq(fe *r, const fe *a)
{
    fe_mul(r, a, a);
}

static void fe_copy(fe *r, const fe *a)
{
    memcpy(r->v, a->v, sizeof(r->v));
}

static int fe_is_zero(const fe *a)
{
    uint32_t bits = 0u;
    int i;

    for (i = 0; i < 8; i++)
    {
        bits |= a->v[i];
    }
    return (bits == 0u) ? 1 : 0;
}

static int fe_equal(const fe *a, const fe *b)
{
    uint32_t bits = 0u;
    int i;

    for (i = 0; i < 8; i++)
    {
        bits |= (uint32_t)(a->v[i] ^ b->v[i]);
    }
    return (bits == 0u) ? 1 : 0;
}

/* r = a^exp (8x32 LE exponent), simple square-and-multiply */
static void fe_pow(fe *r, const fe *a, const uint32_t exp[8])
{
    fe result;
    fe base;
    int started = 0;
    int i, b;

    fe_copy(&result, &FE_ONE);
    fe_copy(&base, a);
    for (i = 7; i >= 0; i--)
    {
        for (b = 31; b >= 0; b--)
        {
            if (started != 0)
            {
                fe_sq(&result, &result);
            }
            if (((exp[i] >> b) & 1u) != 0u)
            {
                if (started != 0)
                {
                    fe_mul(&result, &result, &base);
                }
                else
                {
                    fe_copy(&result, &base);
                    started = 1;
                }
            }
        }
    }
    fe_copy(r, &result);
}

static void fe_invert(fe *r, const fe *a)
{
    fe_pow(r, a, EXP_PM2);
}

static void fe_frombytes(fe *r, const uint8_t b[32])
{
    int i;

    for (i = 0; i < 8; i++)
    {
        r->v[i] = (uint32_t)b[i * 4] |
                  ((uint32_t)b[i * 4 + 1] << 8) |
                  ((uint32_t)b[i * 4 + 2] << 16) |
                  ((uint32_t)b[i * 4 + 3] << 24);
    }
    fe_reduce(r);
}

static void fe_tobytes(uint8_t out[32], const fe *a)
{
    fe t;
    int i;

    fe_copy(&t, a);
    fe_reduce(&t);
    for (i = 0; i < 8; i++)
    {
        out[i * 4]     = (uint8_t)(t.v[i]);
        out[i * 4 + 1] = (uint8_t)(t.v[i] >> 8);
        out[i * 4 + 2] = (uint8_t)(t.v[i] >> 16);
        out[i * 4 + 3] = (uint8_t)(t.v[i] >> 24);
    }
}

/* ---- point operations (RFC 8032 5.1.3 / 5.1.4) -------------------------------*/

static void ge_zero(ge *r)
{
    fe_copy(&r->X, &FE_ZERO);
    fe_copy(&r->Y, &FE_ONE);
    fe_copy(&r->Z, &FE_ONE);
    fe_copy(&r->T, &FE_ZERO);
}

/* forward: doubling reuses the unified addition */
static void ge_add(ge *r, const ge *p, const ge *q);

static void ge_add(ge *r, const ge *p, const ge *q)
{
    fe A, B, C, D, E, F, G, H, t1, t2;

    fe_sub(&t1, &p->Y, &p->X);
    fe_sub(&t2, &q->Y, &q->X);
    fe_mul(&A, &t1, &t2);

    fe_add(&t1, &p->Y, &p->X);
    fe_add(&t2, &q->Y, &q->X);
    fe_mul(&B, &t1, &t2);

    fe_mul(&t1, &p->T, &q->T);
    fe_mul(&C, &t1, &FE_D);
    fe_add(&C, &C, &C);

    fe_mul(&t1, &p->Z, &q->Z);
    fe_add(&D, &t1, &t1);

    fe_sub(&E, &B, &A);
    fe_sub(&F, &D, &C);
    fe_add(&G, &D, &C);
    fe_add(&H, &B, &A);

    fe_mul(&r->X, &E, &F);
    fe_mul(&r->Y, &G, &H);
    fe_mul(&r->T, &E, &H);
    fe_mul(&r->Z, &F, &G);
}

/*
 * Doubling uses the same complete unified formulas as addition: the RFC
 * 8032 dedicated dbl formulas do not map the neutral element to itself,
 * which breaks double-and-add from the identity.
 */
static void ge_double(ge *r, const ge *p)
{
    ge_add(r, p, p);
}

/* decode compressed point (y + sign bit). 0 = ok. */
static int ge_unpack(ge *r, const uint8_t in[32])
{
    fe y, x, u, v, v3, v7, xx, chk;
    uint8_t sign = (uint8_t)(in[31] >> 7);
    uint8_t ybytes[32];
    int i;
    int gt = 0;

    memcpy(ybytes, in, 32u);
    ybytes[31] &= 0x7Fu;
    fe_frombytes(&y, ybytes);

    for (i = 7; i >= 0; i--)                      /* reject y >= p */
    {
        if (y.v[i] > FE_P.v[i]) { gt = 1; break; }
        if (y.v[i] < FE_P.v[i]) { break; }
    }
    if (gt != 0)
    {
        return -1;
    }

    fe_sq(&u, &y);
    fe_sub(&u, &u, &FE_ONE);                      /* u = y^2 - 1 */
    fe_sq(&v, &y);
    fe_mul(&v, &v, &FE_D);
    fe_add(&v, &v, &FE_ONE);                      /* v = d*y^2 + 1 */

    fe_sq(&v3, &v);
    fe_mul(&v3, &v3, &v);
    fe_sq(&v7, &v3);
    fe_mul(&v7, &v7, &v);
    fe_mul(&xx, &u, &v7);
    fe_pow(&x, &xx, EXP_P58);
    fe_mul(&x, &x, &v3);
    fe_mul(&x, &x, &u);

    fe_sq(&chk, &x);
    fe_mul(&chk, &chk, &v);
    if (fe_equal(&chk, &u) == 0)
    {
        fe negu;
        fe_sub(&negu, &FE_ZERO, &u);              /* -u mod p */
        if (fe_equal(&chk, &negu) != 0)
        {
            fe_mul(&x, &x, &FE_SQRT_M1);          /* v*x^2 == -u branch */
            fe_sq(&chk, &x);
            fe_mul(&chk, &chk, &v);
            if (fe_equal(&chk, &u) == 0)
            {
                return -1;                        /* not a square: reject */
            }
        }
        else
        {
            return -1;
        }
    }
    if ((fe_is_zero(&x) != 0) && (sign != 0u))
    {
        return -1;
    }
    if ((x.v[0] & 1u) != (uint32_t)sign)
    {
        fe t;
        fe_sub(&t, &FE_P, &x);
        fe_copy(&x, &t);
    }

    fe_copy(&r->X, &x);
    fe_copy(&r->Y, &y);
    fe_copy(&r->Z, &FE_ONE);
    fe_mul(&r->T, &x, &y);
    return 0;
}

static void ge_tobytes(uint8_t out[32], const ge *p)
{
    fe zinv, x, y;

    fe_invert(&zinv, &p->Z);
    fe_mul(&x, &p->X, &zinv);
    fe_mul(&y, &p->Y, &zinv);
    fe_tobytes(out, &y);
    out[31] = (uint8_t)(out[31] | (uint8_t)((x.v[0] & 1u) << 7));
}

/* point negation: (-X, Y, Z, -T) */
static void ge_neg(ge *r, const ge *p)
{
    fe t;

    fe_sub(&t, &FE_ZERO, &p->X);
    fe_copy(&r->X, &t);
    fe_copy(&r->Y, &p->Y);
    fe_copy(&r->Z, &p->Z);
    fe_sub(&t, &FE_ZERO, &p->T);
    fe_copy(&r->T, &t);
}

/* ---- scalars -----------------------------------------------------------------*/

static int sc_less(const uint8_t a[32], const uint8_t b[32])
{
    int i;

    for (i = 31; i >= 0; i--)
    {
        if (a[i] < b[i]) { return 1; }
        if (a[i] > b[i]) { return 0; }
    }
    return 0;
}

static void sc_reduce(uint8_t out[32], const uint8_t in[64])
{
    uint8_t r[32];
    int i, b;

    memset(r, 0, sizeof(r));
    for (i = 63; i >= 0; i--)
    {
        for (b = 7; b >= 0; b--)
        {
            uint32_t carry = 0u;
            uint32_t bit = (uint32_t)((in[i] >> b) & 1u);
            int ge;
            int j;

            for (j = 0; j < 32; j++)              /* r = 2r + bit */
            {
                uint32_t nc = (uint32_t)((r[j] >> 7) & 1u);
                r[j] = (uint8_t)((uint32_t)(r[j] << 1) | carry);
                carry = nc;
            }
            r[0] = (uint8_t)(r[0] | bit);

            ge = 0;                               /* if r >= L: r -= L */
            for (j = 31; j >= 0; j--)
            {
                if (r[j] > SC_L[j]) { ge = 1; break; }
                if (r[j] < SC_L[j]) { ge = 0; break; }
                if (j == 0) { ge = 1; }
            }
            if (ge != 0)
            {
                uint32_t borrow = 0u;
                uint8_t diff[32];
                for (j = 0; j < 32; j++)
                {
                    uint32_t d = (uint32_t)((uint32_t)r[j] - SC_L[j] - borrow);
                    borrow = ((uint32_t)r[j] < SC_L[j]) ||
                             ((borrow != 0u) && ((uint32_t)r[j] == SC_L[j]));
                    diff[j] = (uint8_t)d;
                }
                memcpy(r, diff, sizeof(diff));
            }
        }
    }
    memcpy(out, r, 32u);
}

static void ge_scalarmult(ge *r, const uint8_t scalar[32], const ge *p)
{
    ge acc;
    int i, b;

    ge_zero(&acc);
    for (i = 31; i >= 0; i--)
    {
        for (b = 7; b >= 0; b--)
        {
            ge_double(&acc, &acc);
            if (((scalar[i] >> b) & 1u) != 0u)
            {
                ge_add(&acc, &acc, p);
            }
        }
    }
    *r = acc;
}

/* base point, built lazily from the generated byte constants */
static const ge *base_point(void)
{
    static ge b;
    static int ready = 0;

    if (ready == 0)
    {
        static const uint8_t bx[32] = C6_ED_BX;
        static const uint8_t by[32] = C6_ED_BY;

        fe_frombytes(&b.X, bx);
        fe_frombytes(&b.Y, by);
        fe_copy(&b.Z, &FE_ONE);
        fe_mul(&b.T, &b.X, &b.Y);
        ready = 1;
    }
    return &b;
}

/* ---- verification (RFC 8032 5.1.7) --------------------------------------------*/

int c6_ed25519_verify(const uint8_t pk[32], const uint8_t sig[64],
                      const uint8_t *msg, size_t msg_len)
{
    ge a_pt, h_pt, b_pt, sum;
    uint8_t h[C6_SHA512_DIGEST_LEN];
    uint8_t hmod[32];
    uint8_t enc[32];
    c6_sha512_ctx_t hc;

    if ((pk == NULL) || (sig == NULL) || ((msg == NULL) && (msg_len != 0u)))
    {
        return -1;
    }
    if (ge_unpack(&a_pt, pk) != 0)
    {
        return -1;
    }
    if (sc_less(&sig[32], SC_L) == 0)
    {
        return -1;                                /* non-canonical s */
    }

    c6_sha512_init(&hc);
    c6_sha512_update(&hc, sig, 32u);
    c6_sha512_update(&hc, pk, 32u);
    c6_sha512_update(&hc, msg, msg_len);
    c6_sha512_final(&hc, h);
    sc_reduce(hmod, h);

    ge_scalarmult(&h_pt, hmod, &a_pt);
    ge_scalarmult(&b_pt, &sig[32], base_point());
    /* RFC 8032: [s]B = R + [h]A  <=>  encode([s]B - [h]A) == R */
    ge_neg(&h_pt, &h_pt);
    ge_add(&sum, &b_pt, &h_pt);
    ge_tobytes(enc, &sum);

    return (memcmp(enc, sig, 32u) == 0) ? 0 : -1;
}
