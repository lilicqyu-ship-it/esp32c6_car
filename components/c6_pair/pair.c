/*
 * pair.c - pairing window follower + session token issuer (LLDD 4.4)
 */
#include "pair.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/md.h"

#include "factory.h"
#include "proto_frames.h"

static const char *TAG = "c6_pair";

typedef struct
{
    SemaphoreHandle_t mtx;
    SemaphoreHandle_t confirm;            /* given by pair_on_frame        */
    pair_output_fn    out;                /* bridge-injected link_send      */
    pair_state_t      state;
    int64_t           window_until_us;
    int               req_sd;             /* sd awaiting CONFIRM           */
    volatile bool     confirmed;
    bool              reject;
    pair_session_t    live;               /* current controller session    */
    pair_session_t    grace;              /* reconnect-without-button hash */
    char              token_hex[PAIR_TOKEN_HEX_LEN + 1u];
} pair_ctx_t;

static pair_ctx_t s_pair;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

esp_err_t pair_init(void)
{
    memset(&s_pair, 0, sizeof(s_pair));
    s_pair.mtx      = xSemaphoreCreateMutex();
    s_pair.confirm  = xSemaphoreCreateBinary();
    if ((s_pair.mtx == NULL) || (s_pair.confirm == NULL))
    {
        return ESP_ERR_NO_MEM;
    }
    s_pair.state = PAIR_IDLE;

    /* restore unexpired grace session (page reload / C6 reboot reconnect) */
    uint8_t hash[16];
    int64_t expiry = 0;
    if (factory_load_session(hash, &expiry) == ESP_OK)
    {
        if (expiry > now_us())
        {
            memcpy(s_pair.grace.hash, hash, sizeof(hash));
            s_pair.grace.expiry_us = expiry;
            ESP_LOGI(TAG, "grace session restored (%lld s left)",
                     (long long)((expiry - now_us()) / 1000000LL));
        }
        else
        {
            (void)factory_clear_session();
        }
    }
    return ESP_OK;
}

void pair_set_output(pair_output_fn out)
{
    s_pair.out = out;
}

pair_state_t pair_state(void)
{
    pair_state_t st;

    if (xSemaphoreTake(s_pair.mtx, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        if ((s_pair.state == PAIR_OPEN) && (now_us() > s_pair.window_until_us))
        {
            s_pair.state = PAIR_IDLE;
        }
        st = s_pair.state;
        (void)xSemaphoreGive(s_pair.mtx);
    }
    else
    {
        st = s_pair.state;
    }
    return st;
}

void pair_get_session(pair_session_t *out)
{
    if (out == NULL)
    {
        return;
    }
    if (xSemaphoreTake(s_pair.mtx, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        *out = s_pair.live;
        if (s_pair.grace.expiry_us > s_pair.live.expiry_us)
        {
            *out = s_pair.grace;
        }
        (void)xSemaphoreGive(s_pair.mtx);
    }
}

void pair_hash_token(const char *token_hex, uint8_t out_hash[16])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t digest[32];

    if ((token_hex == NULL) || (out_hash == NULL))
    {
        return;
    }
    if (mbedtls_md(md, (const unsigned char *)token_hex, strlen(token_hex), digest) == 0)
    {
        memcpy(out_hash, digest, 16u);
    }
}

/* TC275 -> C6 frames */
void pair_on_frame(const proto_frame_t *f)
{
    if (f->len < 1u)
    {
        return;
    }
    switch (f->data[0])
    {
        case PROTO_PAIR_OP_NOTIFY:
        {
            uint8_t window_s = (f->len >= 2u) ? f->data[1] : 60u;
            if (xSemaphoreTake(s_pair.mtx, pdMS_TO_TICKS(20)) == pdTRUE)
            {
                s_pair.state           = PAIR_OPEN;
                s_pair.window_until_us = now_us() + (int64_t)window_s * 1000000LL;
                (void)xSemaphoreGive(s_pair.mtx);
            }
            ESP_LOGI(TAG, "pair window open %u s", (unsigned)window_s);
            break;
        }
        case PROTO_PAIR_OP_CONFIRM:
            s_pair.confirmed = true;
            (void)xSemaphoreGive(s_pair.confirm);
            break;
        case PROTO_PAIR_OP_REJECT:
            s_pair.reject = true;
            (void)xSemaphoreGive(s_pair.confirm);
            break;
        default:
            break;
    }
}

static void pair_send_req(int sd)
{
    proto_frame_t f;

    if (s_pair.out == NULL)
    {
        return;
    }
    f.ver = PROTO_VER;
    f.cmd = PROTO_CMD_PAIR;
    f.seq = 0u;
    f.len = 3u;
    f.data[0] = PROTO_PAIR_OP_REQ;
    f.data[1] = (uint8_t)(sd & 0xFFu);        /* opaque request id for the page */
    f.data[2] = (uint8_t)(sd >> 8);
    s_pair.out(&f);
}

pair_result_t pair_request(int sd, char *token_hex, size_t cap)
{
    pair_result_t res = PAIR_RES_TMO;

    if ((token_hex == NULL) || (cap < PAIR_TOKEN_HEX_LEN + 1u))
    {
        return PAIR_RES_LINK_ERR;
    }

    if (xSemaphoreTake(s_pair.mtx, pdMS_TO_TICKS(50)) != pdTRUE)
    {
        return PAIR_RES_BUSY;
    }

    if (s_pair.state == PAIR_CLAIMED)
    {
        (void)xSemaphoreGive(s_pair.mtx);
        return PAIR_RES_BUSY;
    }
    if ((s_pair.state != PAIR_OPEN) || (now_us() > s_pair.window_until_us))
    {
        s_pair.state = PAIR_IDLE;
        (void)xSemaphoreGive(s_pair.mtx);
        return PAIR_RES_NO_WINDOW;
    }

    s_pair.req_sd   = sd;
    s_pair.confirmed = false;
    s_pair.reject    = false;
    (void)xSemaphoreGive(s_pair.mtx);

    /* drain a CONFIRM that arrived after an earlier round timed out —
     * otherwise it would adjudicate this round immediately */
    while (xSemaphoreTake(s_pair.confirm, 0) == pdTRUE) {}

    pair_send_req(sd);

    if (xSemaphoreTake(s_pair.confirm, pdMS_TO_TICKS(PAIR_CONFIRM_TMO_MS)) != pdTRUE)
    {
        res = PAIR_RES_TMO;                          /* TC275 silent - window may be gone */
    }
    else if (s_pair.reject)
    {
        res = PAIR_RES_REJECTED;
    }
    else if (s_pair.confirmed)
    {
        uint8_t raw[32];

        /* issue session token: 32 random bytes, hex on the wire, hash on disk */
        esp_fill_random(raw, sizeof(raw));
        for (int i = 0; i < 32; i++)
        {
            (void)snprintf(&token_hex[i * 2], 3u, "%02x", raw[i]);
        }
        token_hex[PAIR_TOKEN_HEX_LEN] = '\0';

        uint8_t hash[16];
        pair_hash_token(token_hex, hash);

        if (xSemaphoreTake(s_pair.mtx, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            memcpy(s_pair.live.hash, hash, sizeof(hash));
            s_pair.live.expiry_us = now_us() + PAIR_GRACE_US;
            s_pair.grace          = s_pair.live;   /* RAM mirror of the NVS grace */
            strncpy(s_pair.token_hex, token_hex, sizeof(s_pair.token_hex));
            s_pair.state = PAIR_CLAIMED;
            (void)xSemaphoreGive(s_pair.mtx);

            /* 30 s grace in NVS (only hash, LLDD 4.4/7) */
            if (factory_save_session(hash, s_pair.live.expiry_us) != ESP_OK)
            {
                ESP_LOGW(TAG, "grace session persist failed");
            }
            res = PAIR_RES_OK;
        }
        else
        {
            res = PAIR_RES_BUSY;
        }
    }

    /* window is single-shot: consumed regardless of outcome */
    if (xSemaphoreTake(s_pair.mtx, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        if (s_pair.state == PAIR_OPEN)
        {
            s_pair.state = PAIR_IDLE;
        }
        (void)xSemaphoreGive(s_pair.mtx);
    }
    return res;
}

void pair_ctrl_gone(void)
{
    if (xSemaphoreTake(s_pair.mtx, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        s_pair.state = PAIR_IDLE;     /* LINK_STATE=0 goes out immediately; the
                                         30 s NVS grace keeps serving the token */
        (void)xSemaphoreGive(s_pair.mtx);
    }
}

bool pair_token_ok(const char *token_hex)
{
    uint8_t hash[16];
    bool ok = false;

    if (token_hex == NULL)
    {
        return false;
    }
    pair_hash_token(token_hex, hash);

    if (xSemaphoreTake(s_pair.mtx, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        /* live session always matches its own token */
        if (s_pair.state == PAIR_CLAIMED)
        {
            ok = (memcmp(hash, s_pair.live.hash, 16u) == 0);
        }
        /* grace path: fresh boot / new socket reusing the unexpired token */
        if (!ok && (s_pair.grace.expiry_us > now_us()))
        {
            ok = (memcmp(hash, s_pair.grace.hash, 16u) == 0);
            if (ok)
            {
                s_pair.live = s_pair.grace;
                s_pair.state = PAIR_CLAIMED;
            }
        }
        (void)xSemaphoreGive(s_pair.mtx);
    }
    return ok;
}
