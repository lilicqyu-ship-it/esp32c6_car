/*
 * ws_sessions.h - WebSocket session table (internal to c6_http), LLDD 4.3
 */
#ifndef C6_WS_SESSIONS_H
#define C6_WS_SESSIONS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WS_MAX_SESSIONS   4
#define WS_TOKEN_HASH_LEN 16
#define WS_SLOW_THRESHOLD 3        /* consecutive failed sends before demotion */
#define WS_SLOW_DIVIDER   4        /* demoted clients get every 4th frame      */

typedef enum
{
    WS_ROLE_NONE = 0,
    WS_ROLE_SPECTATOR,
    WS_ROLE_CTRL,
} ws_role_t;

typedef struct
{
    bool     used;
    bool     ws;                          /* upgraded to WebSocket           */
    int      fd;
    ws_role_t role;
    uint8_t  token_hash[WS_TOKEN_HASH_LEN];
    uint32_t last_seq;                    /* replay gate (C6 first line)     */
    uint8_t  hello_sent;                  /* HELLO JSON pushed once          */
    uint8_t  slow_count;                  /* consecutive send failures       */
    uint8_t  dead_count;                  /* >= 2*threshold: keepalive only  */
    uint32_t tx_frames;
} ws_session_t;

typedef void (*ws_sess_change_cb_t)(void);   /* client set changed -> 0x42  */

void ws_sessions_init(void);

/* Register/unregister a socket (HTTP at first, may upgrade to ws). */
ws_session_t *ws_sess_open(int fd);
void ws_sess_close(int fd);

/* Mark a socket as upgraded WebSocket. */
void ws_sess_set_ws(int fd);

ws_session_t *ws_sess_get(int fd);

/* Assign CTRL role with the given token hash; demotes any previous CTRL. */
void ws_sess_promote(int fd, const uint8_t token_hash[WS_TOKEN_HASH_LEN]);

/* Command gate: returns true when role==CTRL and seq is strictly monotonic. */
bool ws_sess_check_cmd(int fd, uint8_t seq);

/* Pacing bookkeeping for the telemetry broadcaster (LLDD 4.3):
 *  - ws_sess_send_ok(): reset failure counters
 *  - ws_sess_send_fail(): slow_count++, returns false when the client must
 *    be skipped entirely (keepalive-only). */
void ws_sess_send_ok(int fd);
void ws_sess_send_fail(int fd);
bool ws_sess_skip(int fd, uint32_t tick);    /* true = no telemetry this tick */

/* Pacing send used by ws_broadcast_binary: snapshot the ws sessions, then for
 * each: skip-check (slow/dead client) -> send -> bookkeeping.  Returns the
 * number of clients the frame actually reached. */
typedef int (*ws_send_fn)(int fd, const uint8_t *payload, size_t len, bool text);
int ws_sessions_foreach_send(ws_send_fn send, const uint8_t *payload, size_t len,
                             bool text, uint32_t tick);

int ws_sess_count(void);                     /* ws-upgraded sockets          */
int ws_sess_ctrl_fd(void);                   /* -1 = no controller           */

/* Client-set aggregation for 0x42 LINK_STATE: 0 none / 1 spectators / 2 ctrl. */
uint8_t ws_sess_link_state(void);

void ws_on_change(ws_sess_change_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* C6_WS_SESSIONS_H */
