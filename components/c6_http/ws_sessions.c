/*
 * ws_sessions.c - WebSocket session table, LLDD 4.3
 */
#include "ws_sessions.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct
{
    ws_session_t sess[WS_MAX_SESSIONS];
    SemaphoreHandle_t mtx;
    ws_sess_change_cb_t change_cb;
} ws_ctx_t;

static ws_ctx_t s_ws;

void ws_sessions_init(void)
{
    memset(&s_ws, 0, sizeof(s_ws));
    s_ws.mtx = xSemaphoreCreateMutex();
}

static void notify_change(void)
{
    if (s_ws.change_cb != NULL)
    {
        s_ws.change_cb();
    }
}

void ws_on_change(ws_sess_change_cb_t cb)
{
    s_ws.change_cb = cb;
}

ws_session_t *ws_sess_open(int fd)
{
    ws_session_t *slot = NULL;

    if (xSemaphoreTake(s_ws.mtx, pdMS_TO_TICKS(20)) != pdTRUE)
    {
        return NULL;
    }
    for (int i = 0; i < WS_MAX_SESSIONS; i++)
    {
        if (s_ws.sess[i].used && (s_ws.sess[i].fd == fd))
        {
            slot = &s_ws.sess[i];                    /* re-entered handler */
            break;
        }
    }
    if (slot == NULL)
    {
        for (int i = 0; i < WS_MAX_SESSIONS; i++)
        {
            if (!s_ws.sess[i].used)
            {
                slot = &s_ws.sess[i];
                memset(slot, 0, sizeof(*slot));
                slot->used = true;
                slot->fd   = fd;
                slot->role = WS_ROLE_SPECTATOR;      /* default: observe only */
                break;
            }
        }
    }
    (void)xSemaphoreGive(s_ws.mtx);
    if (slot != NULL)
    {
        notify_change();
    }
    return slot;
}

void ws_sess_close(int fd)
{
    bool changed = false;

    if (xSemaphoreTake(s_ws.mtx, pdMS_TO_TICKS(20)) != pdTRUE)
    {
        return;
    }
    for (int i = 0; i < WS_MAX_SESSIONS; i++)
    {
        if (s_ws.sess[i].used && (s_ws.sess[i].fd == fd))
        {
            memset(&s_ws.sess[i], 0, sizeof(s_ws.sess[i]));
            changed = true;
            break;
        }
    }
    (void)xSemaphoreGive(s_ws.mtx);
    if (changed)
    {
        notify_change();
    }
}

void ws_sess_set_ws(int fd)
{
    ws_session_t *s = ws_sess_get(fd);
    if (s != NULL)
    {
        s->ws = true;
    }
}

ws_session_t *ws_sess_get(int fd)
{
    ws_session_t *slot = NULL;

    if (xSemaphoreTake(s_ws.mtx, pdMS_TO_TICKS(20)) != pdTRUE)
    {
        return NULL;
    }
    for (int i = 0; i < WS_MAX_SESSIONS; i++)
    {
        if (s_ws.sess[i].used && (s_ws.sess[i].fd == fd))
        {
            slot = &s_ws.sess[i];
            break;
        }
    }
    (void)xSemaphoreGive(s_ws.mtx);
    return slot;
}

void ws_sess_promote(int fd, const uint8_t token_hash[WS_TOKEN_HASH_LEN])
{
    ws_session_t *s = ws_sess_get(fd);

    if (s == NULL)
    {
        return;
    }
    if (xSemaphoreTake(s_ws.mtx, pdMS_TO_TICKS(20)) != pdTRUE)
    {
        return;
    }
    for (int i = 0; i < WS_MAX_SESSIONS; i++)
    {
        if (s_ws.sess[i].used && (s_ws.sess[i].role == WS_ROLE_CTRL) &&
            (s_ws.sess[i].fd != fd))
        {
            s_ws.sess[i].role = WS_ROLE_SPECTATOR;   /* old CTRL demoted (grace
                                                        handling is pair's job) */
        }
    }
    s->role = WS_ROLE_CTRL;
    if (token_hash != NULL)
    {
        memcpy(s->token_hash, token_hash, WS_TOKEN_HASH_LEN);
    }
    s->last_seq = 0u;
    (void)xSemaphoreGive(s_ws.mtx);
    notify_change();
}

bool ws_sess_check_cmd(int fd, uint8_t seq)
{
    ws_session_t *s = ws_sess_get(fd);
    bool ok = false;

    if (s == NULL)
    {
        return false;
    }
    if (xSemaphoreTake(s_ws.mtx, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        if ((s->role == WS_ROLE_CTRL) && (seq != s->last_seq))
        {
            s->last_seq = seq;                       /* monotonic gate (LLDD 4.3) */
            ok = true;
        }
        (void)xSemaphoreGive(s_ws.mtx);
    }
    return ok;
}

void ws_sess_send_ok(int fd)
{
    ws_session_t *s = ws_sess_get(fd);
    if (s != NULL)
    {
        s->slow_count = 0u;
        s->dead_count = 0u;
        s->tx_frames++;
    }
}

void ws_sess_send_fail(int fd)
{
    ws_session_t *s = ws_sess_get(fd);
    if (s != NULL)
    {
        if (s->slow_count < 255u)
        {
            s->slow_count++;
        }
        if (s->slow_count >= (2u * WS_SLOW_THRESHOLD))
        {
            s->dead_count = s->slow_count;           /* keepalive-only state */
        }
    }
}

bool ws_sess_skip(int fd, uint32_t tick)
{
    ws_session_t *s = ws_sess_get(fd);
    bool skip = false;

    if (s == NULL)
    {
        return true;
    }
    if (s->dead_count != 0u)
    {
        return true;                                 /* keepalive only */
    }
    if (s->slow_count >= WS_SLOW_THRESHOLD)
    {
        skip = ((tick % WS_SLOW_DIVIDER) != 0u);     /* 12.5 Hz instead of 50 */
    }
    return skip;
}

int ws_sess_count(void)
{
    int n = 0;

    if (xSemaphoreTake(s_ws.mtx, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        for (int i = 0; i < WS_MAX_SESSIONS; i++)
        {
            if (s_ws.sess[i].used && s_ws.sess[i].ws)
            {
                n++;
            }
        }
        (void)xSemaphoreGive(s_ws.mtx);
    }
    return n;
}

int ws_sess_ctrl_fd(void)
{
    int fd = -1;

    if (xSemaphoreTake(s_ws.mtx, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        for (int i = 0; i < WS_MAX_SESSIONS; i++)
        {
            if (s_ws.sess[i].used && s_ws.sess[i].ws && (s_ws.sess[i].role == WS_ROLE_CTRL))
            {
                fd = s_ws.sess[i].fd;
                break;
            }
        }
        (void)xSemaphoreGive(s_ws.mtx);
    }
    return fd;
}

uint8_t ws_sess_link_state(void)
{
    uint8_t st = 0u;

    if (xSemaphoreTake(s_ws.mtx, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        for (int i = 0; i < WS_MAX_SESSIONS; i++)
        {
            if (s_ws.sess[i].used && s_ws.sess[i].ws)
            {
                if (s_ws.sess[i].role == WS_ROLE_CTRL)
                {
                    st = 2u;
                    break;
                }
                st = 1u;
            }
        }
        (void)xSemaphoreGive(s_ws.mtx);
    }
    return st;
}

int ws_sessions_foreach_send(ws_send_fn send, const uint8_t *payload, size_t len,
                             bool text, uint32_t tick)
{
    int fds[WS_MAX_SESSIONS];
    int n = 0;
    int reached = 0;

    if (send == NULL)
    {
        return 0;
    }
    /* snapshot under lock, send outside it (async send never blocks long, but
     * the table must stay free for close/open events) */
    if (xSemaphoreTake(s_ws.mtx, pdMS_TO_TICKS(20)) == pdTRUE)
    {
        for (int i = 0; i < WS_MAX_SESSIONS; i++)
        {
            if (s_ws.sess[i].used && s_ws.sess[i].ws)
            {
                fds[n++] = s_ws.sess[i].fd;
            }
        }
        (void)xSemaphoreGive(s_ws.mtx);
    }
    for (int i = 0; i < n; i++)
    {
        if (ws_sess_skip(fds[i], tick))
        {
            continue;
        }
        if (send(fds[i], payload, len, text) == 0)
        {
            ws_sess_send_ok(fds[i]);
            reached++;
        }
        else
        {
            ws_sess_send_fail(fds[i]);
        }
    }
    return reached;
}
