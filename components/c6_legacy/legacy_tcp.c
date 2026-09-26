/*
 * legacy_tcp.c - TCP 8080 <-> LINK v2 frame passthrough (default off)
 */
#include "legacy_tcp.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "link.h"
#include "proto_frames.h"

static const char *TAG = "c6_legacy";

#define LEGACY_PORT     8080
#define LEGACY_MAX_PEERS 2

static SemaphoreHandle_t s_peer_mtx;
static int s_peers[LEGACY_MAX_PEERS] = { -1, -1 };

/* false when the peer table is full (caller must reject the connection) */
static bool peer_add(int fd)
{
    bool added = false;

    if (xSemaphoreTake(s_peer_mtx, portMAX_DELAY) == pdTRUE)
    {
        for (int i = 0; i < LEGACY_MAX_PEERS; i++)
        {
            if (s_peers[i] < 0)
            {
                s_peers[i] = fd;
                added = true;
                break;
            }
        }
        (void)xSemaphoreGive(s_peer_mtx);
    }
    return added;
}

static void peer_del(int fd)
{
    if (xSemaphoreTake(s_peer_mtx, portMAX_DELAY) == pdTRUE)
    {
        for (int i = 0; i < LEGACY_MAX_PEERS; i++)
        {
            if (s_peers[i] == fd)
            {
                s_peers[i] = -1;
            }
        }
        (void)xSemaphoreGive(s_peer_mtx);
    }
}

/* LINK -> all TCP peers: registered as a link tap (mirror only).
 * Runs in the link_rx task: sends must never block on a stalled peer. */
static void legacy_tap(const proto_frame_t *f)
{
    uint8_t wire[PROTO_MAX_FRAME];
    size_t n;

    n = proto_encode(f, wire, sizeof(wire));
    if (n == 0u)
    {
        return;
    }
    if (xSemaphoreTake(s_peer_mtx, pdMS_TO_TICKS(0)) == pdTRUE)
    {
        for (int i = 0; i < LEGACY_MAX_PEERS; i++)
        {
            if (s_peers[i] >= 0)
            {
                ssize_t sent = send(s_peers[i], wire, n, MSG_DONTWAIT);
                if (sent < 0)
                {
                    /* backpressured or dead: drop for this peer, RX path stays live */
                }
            }
        }
        (void)xSemaphoreGive(s_peer_mtx);
    }
}

static void client_task(void *arg)
{
    int fd = (int)(intptr_t)arg;
    proto_parser_t parser;
    proto_frame_t f;
    uint8_t buf[256];
    int n;

    proto_parser_init(&parser);
    for (;;)
    {
        n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            break;
        }
        for (int i = 0; i < n; i++)
        {
            if (proto_parser_feed(&parser, buf[i], &f) == PROTO_RX_FRAME)
            {
                (void)link_send(&f);
            }
        }
    }
    /* unregister before close: a reused fd must not be sent stale frames */
    peer_del(fd);
    (void)closesocket(fd);
    vTaskDelete(NULL);
}

static void legacy_task(void *arg)
{
    int lst = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr;
    int opt = 1;

    (void)arg;
    if (lst < 0)
    {
        return;
    }
    (void)setsockopt(lst, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(LEGACY_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if ((bind(lst, (struct sockaddr *)&addr, sizeof(addr)) != 0) ||
        (listen(lst, 2) != 0))
    {
        (void)closesocket(lst);
        return;
    }
    ESP_LOGI(TAG, "legacy TCP bridge on :%d", LEGACY_PORT);

    for (;;)
    {
        int fd = accept(lst, NULL, NULL);
        if (fd < 0)
        {
            /* EMFILE etc.: without a pause this loop spins at httpd's cost */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!peer_add(fd))
        {
            (void)closesocket(fd);                 /* table full: reject 3rd peer */
            continue;
        }
        if (xTaskCreate(client_task, "legacy_cli", 3072,
                        (void *)(intptr_t)fd, 4, NULL) != pdPASS)
        {
            peer_del(fd);
            (void)closesocket(fd);
        }
    }
}

esp_err_t legacy_tcp_start(void)
{
    s_peer_mtx = xSemaphoreCreateMutex();
    if (s_peer_mtx == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    link_set_tap(legacy_tap);
    if (xTaskCreate(legacy_task, "legacy", 3072, NULL, 4, NULL) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
