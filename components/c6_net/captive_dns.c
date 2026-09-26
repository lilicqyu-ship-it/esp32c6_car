/*
 * captive_dns.c - captive-portal wildcard DNS (UDP 53 -> AP IP), LLDD 4.2
 */
#include "captive_dns.h"

#include <string.h>

#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "c6_dns";

#define DNS_PORT     53
#define DNS_TASK_PRIO 4
#define DNS_TASK_STACK 3072

typedef struct __attribute__((packed))
{
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_hdr_t;

static volatile bool s_quit;
static char s_ip[16];
static int s_sock = -1;

static void dns_append_name_a(uint8_t *p, uint16_t *off, const char *ip)
{
    /* we answer the question with a compressed pointer to 0xC00C plus an A record */
    uint32_t a = ipaddr_addr(ip);
    p[(*off)++] = 0xC0;
    p[(*off)++] = 0x0C;                      /* ptr to question name */
    p[(*off)++] = 0x00; p[(*off)++] = 0x01;  /* type A */
    p[(*off)++] = 0x00; p[(*off)++] = 0x01;  /* class IN */
    p[(*off)++] = 0x00; p[(*off)++] = 0x00;
    p[(*off)++] = 0x00; p[(*off)++] = 0x3C;  /* TTL 60 s */
    p[(*off)++] = 0x00; p[(*off)++] = 0x04;  /* rdlen 4 */
    memcpy(&p[*off], &a, 4u);
    *off = (uint16_t)(*off + 4u);
}

static void dns_task(void *arg)
{
    uint8_t rx[512];
    uint8_t tx[512];
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);

    (void)arg;
    ESP_LOGI(TAG, "captive DNS on :53 -> %s", s_ip);

    while (!s_quit)
    {
        int n = recvfrom(s_sock, rx, sizeof(rx), 0,
                         (struct sockaddr *)&peer, &peer_len);
        if (n < (int)sizeof(dns_hdr_t))
        {
            continue;
        }
        dns_hdr_t *q = (dns_hdr_t *)rx;
        if ((q->qdcount == 0u) || (q->qdcount > 4u))
        {
            continue;
        }

        /* craft response: copy header + question, set QR/AA, one A answer */
        memcpy(tx, rx, (size_t)n);
        dns_hdr_t *r = (dns_hdr_t *)tx;
        r->flags   = 0x8180u | 0x0040u;      /* QR=1 AA=1 RD=1 RA=0 RCODE=0 */
        r->ancount = q->qdcount;             /* one answer per question     */
        r->nscount = 0u;
        r->arcount = 0u;

        uint16_t off = (uint16_t)n;
        for (uint16_t i = 0u; (i < q->qdcount) && ((off + 20u) < sizeof(tx)); i++)
        {
            dns_append_name_a(tx, &off, s_ip);
        }

        (void)sendto(s_sock, tx, off, 0, (struct sockaddr *)&peer, peer_len);
    }
    (void)closesocket(s_sock);
    s_sock = -1;
    vTaskDelete(NULL);
}

void captive_dns_set_ip(const char *ip)
{
    if ((ip != NULL) && (strlen(ip) < sizeof(s_ip)))
    {
        strcpy(s_ip, ip);
    }
}

esp_err_t captive_dns_start(const char *ap_ip)
{
    struct sockaddr_in bind_addr;

    captive_dns_set_ip(ap_ip);

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0)
    {
        return ESP_FAIL;
    }
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(DNS_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0)
    {
        (void)closesocket(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    if (xTaskCreate(dns_task, "captive_dns", DNS_TASK_STACK, NULL, DNS_TASK_PRIO, NULL) != pdPASS)
    {
        (void)closesocket(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
