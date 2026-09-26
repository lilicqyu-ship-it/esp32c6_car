/*
 * mdns_lite.c - minimal mDNS responder (decision C2)
 *
 * Answers, on the softAP interface multicast group 224.0.0.251:5353:
 *   - A / AAAA queries for "<host>.local"      -> our IPv4
 *   - PTR queries for "_http._tcp.local"       -> "<inst>._http._tcp.local"
 *   - SRV/TXT/A additional records for that service
 * Enough for phones to resolve http://mycar.local/ from the captive portal.
 */
#include "mdns_lite.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "c6_mdns";

#define MDNS_PORT        5353
#define MDNS_GROUP       "224.0.0.251"
#define MDNS_TASK_PRIO   4
#define MDNS_TASK_STACK  3584

typedef struct __attribute__((packed))
{
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} mdns_hdr_t;

typedef struct
{
    char host[32];                       /* "mycar"                    */
    char fqdn[40];                       /* "mycar.local"              */
    char svc[32];                        /* "<inst>._http._tcp.local"  */
    uint32_t addr;                       /* network byte order         */
} mdns_ctx_t;

static mdns_ctx_t s_md;
static int s_sock = -1;

/* ---- name compression-free record writers ---------------------------------- */

static int put_name(uint8_t *p, int off, int cap, const char *name)
{
    /* name: dot-separated labels; "" means root */
    while (*name != '\0')
    {
        const char *dot = strchr(name, '.');
        int len = (dot != NULL) ? (int)(dot - name) : (int)strlen(name);
        if (len > 63)
        {
            len = 63;
        }
        if ((off + 1 + len) > cap)
        {
            return -1;
        }
        p[off++] = (uint8_t)len;
        memcpy(&p[off], name, (size_t)len);
        off += len;
        name += (dot != NULL) ? (size_t)len + 1u : (size_t)len;
    }
    if (off + 1 > cap)
    {
        return -1;
    }
    p[off++] = 0;
    return off;
}

static int put_rr_head(uint8_t *p, int off, int cap, const char *name,
                       uint16_t type, uint16_t rclass, uint32_t ttl, uint16_t rdlen)
{
    if ((off + 10) > cap)
    {
        return -1;
    }
    off = put_name(p, off, cap, name);
    if (off < 0)
    {
        return -1;
    }
    p[off++] = (uint8_t)(type >> 8);
    p[off++] = (uint8_t)type;
    p[off++] = (uint8_t)(rclass >> 8);
    p[off++] = (uint8_t)rclass;
    p[off++] = (uint8_t)(ttl >> 24);
    p[off++] = (uint8_t)(ttl >> 16);
    p[off++] = (uint8_t)(ttl >> 8);
    p[off++] = (uint8_t)ttl;
    p[off++] = (uint8_t)(rdlen >> 8);
    p[off++] = (uint8_t)rdlen;
    return off;
}

static int strcasecmp_local(const char *a, const char *b);

/* wire size of an encoded name: one length byte per label + root byte */
static int name_wire_len(const char *name)
{
    int labels = 0;
    const char *s = name;

    if (*s == '\0')
    {
        return 1;
    }
    while (*s != '\0')
    {
        if (*s++ == '.')
        {
            labels++;
        }
    }
    return labels + 1 + (int)strlen(name);
}

/* Decode one uncompressed wire name at *pos into out (lower-cased, dotted)
 * and park *pos on the trailing QTYPE. Returns 0 on malformed input. */
static int name_read(const uint8_t *q, int qlen, int *pos, char *out, size_t cap)
{
    int p = *pos;
    size_t gp = 0u;

    for (;;)
    {
        int len, i;
        if (p >= qlen)
        {
            return 0;
        }
        len = q[p++];
        if (len == 0)
        {
            break;                                   /* end of name */
        }
        if ((len & 0xC0) != 0)
        {
            return 0;                                /* compressed: not expected in queries */
        }
        if ((p + len) > qlen)
        {
            return 0;
        }
        if ((gp + (size_t)len + 1u) >= cap)
        {
            return 0;
        }
        if (gp > 0u)
        {
            out[gp++] = '.';
        }
        for (i = 0; i < len; i++)
        {
            out[gp++] = (char)tolower((unsigned char)q[p + i]);
        }
        p += len;
    }
    out[gp] = '\0';
    if ((p + 4) > qlen)
    {
        return 0;                                    /* QTYPE + QCLASS */
    }
    *pos = p;
    return 1;
}

static int strcasecmp_local(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb)
        {
            return ca - cb;
        }
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/* Build the answer set for one question. Returns packet length or 0. */
static int mdns_build_answer(const uint8_t *q, int qlen, uint16_t qtype,
                             const struct sockaddr_in *to, uint8_t *out, int cap)
{
    int off = (int)sizeof(mdns_hdr_t);

    if (qtype != 1u && qtype != 12u && qtype != 33u && qtype != 16u && qtype != 255u)
    {
        return 0;                                    /* A PTR SRV TXT ANY only */
    }

    /* which name is asked? decode the single question exactly once */
    char qname[80];
    int pos = (int)sizeof(mdns_hdr_t);

    if (name_read(q, qlen, &pos, qname, sizeof(qname)) == 0)
    {
        return 0;
    }
    int is_host = (strcasecmp_local(qname, s_md.fqdn) == 0);
    int is_svc  = (!is_host) &&
                  ((strcasecmp_local(qname, "_http._tcp.local") == 0) ||
                   (strcasecmp_local(qname, s_md.svc) == 0));
    (void)to;

    if (!is_host && !is_svc)
    {
        return 0;
    }

    mdns_hdr_t *h = (mdns_hdr_t *)out;
    memset(out, 0, (size_t)sizeof(mdns_hdr_t));
    h->flags   = lwip_htons(0x8400u);                  /* QR=1 AA=1, on the wire */
    h->qdcount = 0u;                                 /* legacy unicast query gets no echo */

    if (is_host)
    {
        /* A record for mycar.local */
        int rd_off = put_rr_head(out, off, cap, s_md.fqdn, 1u, 0x8001u, 120u, 4u);
        if (rd_off < 0)
        {
            return 0;
        }
        memcpy(&out[rd_off], &s_md.addr, 4u);
        off = rd_off + 4;
        h->ancount = lwip_htons(1u);
    }
    else
    {
        /* PTR "_http._tcp.local" -> "<inst>._http._tcp.local" */
        int rd_off = put_rr_head(out, off, cap, "_http._tcp.local", 12u, 0x8001u, 4500u,
                                 (uint16_t)name_wire_len(s_md.svc));
        if (rd_off < 0)
        {
            return 0;
        }
        rd_off = put_name(out, rd_off, cap, s_md.svc);
        if (rd_off < 0)
        {
            return 0;
        }
        h->ancount = lwip_htons(1u);
        off = rd_off;

        /* SRV for the service */
        int srv_len = 6 + name_wire_len(s_md.fqdn);
        rd_off = put_rr_head(out, off, cap, s_md.svc, 33u, 0x8001u, 120u, (uint16_t)srv_len);
        if (rd_off < 0)
        {
            return 0;
        }
        out[rd_off++] = 0u; out[rd_off++] = 0u;      /* prio */
        out[rd_off++] = 0u; out[rd_off++] = 0u;      /* weight */
        out[rd_off++] = 0u; out[rd_off++] = 80u;     /* port 80 */
        rd_off = put_name(out, rd_off, cap, s_md.fqdn);
        if (rd_off < 0)
        {
            return 0;
        }
        off = rd_off;
        uint16_t an = lwip_ntohs(h->ancount);
        h->ancount = lwip_htons((uint16_t)(an + 1u));

        /* A (additional) */
        rd_off = put_rr_head(out, off, cap, s_md.fqdn, 1u, 0x8001u, 120u, 4u);
        if (rd_off < 0)
        {
            return 0;
        }
        memcpy(&out[rd_off], &s_md.addr, 4u);
        off = rd_off + 4;
        an = lwip_ntohs(h->ancount);
        h->ancount = lwip_htons((uint16_t)(an + 1u));

        /* TXT (additional) "path=/" as one length-prefixed character-string */
        static const char txt[] = "path=/";
        int txt_len = (int)sizeof(txt);              /* count byte + text */
        rd_off = put_rr_head(out, off, cap, s_md.svc, 16u, 0x8001u, 4500u,
                             (uint16_t)txt_len);
        if (rd_off < 0)
        {
            return 0;
        }
        out[rd_off++] = (uint8_t)(sizeof(txt) - 1u);
        memcpy(&out[rd_off], txt, sizeof(txt) - 1u);
        off = rd_off + (int)(sizeof(txt) - 1u);
        an = lwip_ntohs(h->ancount);
        h->ancount = lwip_htons((uint16_t)(an + 1u));
    }
    return off;
}

static void mdns_task(void *arg)
{
    uint8_t rx[900];
    uint8_t tx[900];
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    struct ip_mreq mreq;

    (void)arg;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(MDNS_GROUP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    (void)setsockopt(s_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    ESP_LOGI(TAG, "mDNS lite up: %s.local", s_md.host);

    for (;;)
    {
        int n = recvfrom(s_sock, rx, sizeof(rx), 0, (struct sockaddr *)&peer, &peer_len);
        if (n < (int)(sizeof(mdns_hdr_t) + 5))
        {
            continue;
        }
        mdns_hdr_t *q = (mdns_hdr_t *)rx;
        /* never answer a response (echo storm), and only single-question queries */
        if ((q->qdcount != lwip_htons(1u)) ||
            ((lwip_ntohs(q->flags) & 0x8000u) != 0u))
        {
            continue;
        }

        /* skip the question name: labels end at the root (0x00) byte;
         * QTYPE/QCLASS may legitimately start with 0x00 so stop here */
        int qoff = (int)sizeof(mdns_hdr_t);
        while ((qoff < n) && (rx[qoff] != 0u))
        {
            if ((rx[qoff] & 0xC0) != 0u)
            {
                qoff = n;                            /* compressed: reject */
                break;
            }
            qoff += rx[qoff] + 1;
        }
        qoff += 1;                                   /* root label */
        if ((qoff + 4) > n)
        {
            continue;
        }
        uint16_t qtype = (uint16_t)(((uint16_t)rx[qoff] << 8) | rx[qoff + 1]);

        int alen = mdns_build_answer(rx, n, qtype, &peer, tx, (int)sizeof(tx));
        if (alen > 0)
        {
            (void)sendto(s_sock, tx, alen, 0, (struct sockaddr *)&peer, peer_len);
        }
    }
}

esp_err_t mdns_lite_start(const char *hostname, const char *instance, const char *ip)
{
    struct sockaddr_in bind_addr;
    char full_host[40];
    char full_svc[48];

    if ((hostname == NULL) || (ip == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(s_md.host, hostname, sizeof(s_md.host) - 1u);
    (void)snprintf(full_host, sizeof(full_host), "%s.local", hostname);
    strncpy(s_md.fqdn, full_host, sizeof(s_md.fqdn) - 1u);
    (void)snprintf(full_svc, sizeof(full_svc), "%s._http._tcp.local",
                   (instance != NULL) ? instance : hostname);
    strncpy(s_md.svc, full_svc, sizeof(s_md.svc) - 1u);
    s_md.addr = inet_addr(ip);

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0)
    {
        return ESP_FAIL;
    }
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = lwip_htons(MDNS_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    int reuse = 1;
    (void)setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0)
    {
        (void)closesocket(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    if (xTaskCreate(mdns_task, "mdns_lite", MDNS_TASK_STACK, NULL, MDNS_TASK_PRIO, NULL) != pdPASS)
    {
        (void)closesocket(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
