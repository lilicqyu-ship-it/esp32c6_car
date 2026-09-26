/*
 * pair.h - pairing window follower + session token issuer (LLDD 4.4)
 *
 * Responsibility split (decision D3):
 *   TC275 = authorization authority (button window, PAIR_CONFIRM verdict)
 *   C6    = session executor   (sd<->role binding, token issue, 30 s grace)
 */
#ifndef C6_PAIR_H
#define C6_PAIR_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "proto_frames.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAIR_TOKEN_HEX_LEN   64u     /* 32 random bytes, hex          */
#define PAIR_GRACE_US        (30LL * 1000000LL)
#define PAIR_CONFIRM_TMO_MS  3000

typedef enum
{
    PAIR_IDLE = 0,
    PAIR_OPEN,               /* TC275 window active, claiming allowed */
    PAIR_CLAIMED,            /* a controller owns the session token   */
} pair_state_t;

typedef enum
{
    PAIR_RES_OK = 0,
    PAIR_RES_NO_WINDOW,      /* window closed (TC275 reject)          */
    PAIR_RES_BUSY,           /* another CTRL holds a live session     */
    PAIR_RES_LINK_ERR,       /* LINK down / send failed               */
    PAIR_RES_TMO,            /* no CONFIRM inside 3 s                 */
    PAIR_RES_REJECTED,       /* explicit TC275 rejection              */
} pair_result_t;

typedef void (*pair_output_fn)(const proto_frame_t *f);

/* Token hash type: first 16 bytes of sha256. */
typedef struct
{
    uint8_t hash[16];
    int64_t expiry_us;
} pair_session_t;

esp_err_t pair_init(void);

/* Bridge injects the single LINK TX path here (LLDD 2.3: bridge is the only
 * LINK TX writer; pair only frames requests and hands them over). */
void pair_set_output(pair_output_fn out);

pair_state_t pair_state(void);
void pair_get_session(pair_session_t *out);

/* Bridge routes LINK PAIR_* frames here (CONFIRM/NOTIFY/REJECT). */
void pair_on_frame(const proto_frame_t *f);

/* Called from /api/pair handler (httpd task). Blocks up to 3 s on CONFIRM.
 * On success *token_hex receives the NUL-terminated 64-char session token. */
pair_result_t pair_request(int sd, char *token_hex, size_t cap);

/* The controller session ended (WS close / explicit). */
void pair_ctrl_gone(void);

/* Token check for WS upgrade (?token=) and /ota authorization.
 * Matches the live session hash or an unexpired NVS grace hash. */
bool pair_token_ok(const char *token_hex);

/* Token hash helper used by c6_http sessions. */
void pair_hash_token(const char *token_hex, uint8_t out_hash[16]);

#ifdef __cplusplus
}
#endif

#endif /* C6_PAIR_H */
