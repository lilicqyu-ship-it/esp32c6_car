/*
 * legacy_tcp.h - TCP 8080 raw-frame bridge (LLDD FR-10, CONFIG_C6_LEGACY_TCP)
 *
 * Demo clients connect to :8080 and speak proto v2 frames directly; valid
 * frames are relayed to the LINK, LINK frames are fanned out to every TCP
 * peer.  Non-frame bytes are dropped (CRC-checked, no blind piping).
 */
#ifndef C6_LEGACY_TCP_H
#define C6_LEGACY_TCP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t legacy_tcp_start(void);

#ifdef __cplusplus
}
#endif

#endif /* C6_LEGACY_TCP_H */
