/*
 * mdns_lite.h - minimal mDNS responder: mycar.local + _http._tcp (FR-1)
 *
 * IDF v6.x removed the in-tree mdns component (moved to the component
 * registry, which would require network access at build time), so the A/PTR/
 * SRV/TXT answers we need are served by this ~250-line responder instead
 * (coding-plan decision C2).
 */
#ifndef C6_MDNS_LITE_H
#define C6_MDNS_LITE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* hostname without ".local", e.g. "mycar" */
esp_err_t mdns_lite_start(const char *hostname, const char *instance, const char *ip);

#ifdef __cplusplus
}
#endif

#endif /* C6_MDNS_LITE_H */
