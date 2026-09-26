/*
 * net.h - softAP + captive portal plumbing + mDNS (LLDD 4.2, FR-1)
 */
#ifndef C6_NET_H
#define C6_NET_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    const char *ssid;                /* "SD-xxxxxx", factory-derived     */
    const char *password;            /* >= 8 chars factory random        */
    uint8_t     channel;             /* 1 / 6 / 11                       */
    uint8_t     max_conn;            /* LLDD: 4                          */
} net_cfg_t;

typedef void (*net_clients_cb_t)(int sta_count);     /* AP station change */

/* Bring up softAP (AP mode), captive DNS and mDNS responder. */
esp_err_t net_start(const net_cfg_t *cfg);

/* Register the station-count change hook (bridge turns it into 0x42). */
void net_on_ap_clients(net_clients_cb_t cb);

/* AP IPv4 as string ("192.168.4.1"), for portal/redirects/diag. */
const char *net_ip_str(void);

/* Current number of connected stations (cached, no wifi calls). */
int net_sta_count(void);

/* Reserved route-B uplink credentials (NVS, LLDD 4.2; unused in V1.0). */
esp_err_t net_load_uplink(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap);

#ifdef __cplusplus
}
#endif

#endif /* C6_NET_H */
