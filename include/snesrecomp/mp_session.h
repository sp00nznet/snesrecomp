/*
 * mp_session — generic SNES lockstep netplay transport.
 *
 * NOT game-specific. Both peers run the same deterministic emulation and
 * exchange the two controller pads each frame, starting from a save-state the
 * host sends on connect. Host's local pad drives controller 1, the client's
 * drives controller 2 (both peers reconstruct the same {P1,P2} pair).
 *
 * Transport: TCP (reliable/ordered — ideal for lockstep). Connecting/accepting
 * runs on a background thread so the UI never blocks; once connected the
 * per-frame exchange is a short blocking round-trip on the main thread.
 */
#ifndef SNESRECOMP_MP_SESSION_H
#define SNESRECOMP_MP_SESSION_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MP_IDLE,          /* no session */
    MP_HOSTING,       /* host: listening for a peer */
    MP_CONNECTING,    /* client: connecting */
    MP_CONNECTED,     /* connected (ready to sync + play) */
    MP_DISCONNECTED   /* peer dropped / error */
} MpState;

void mp_init(void);       /* one-time (Winsock startup). Safe to call repeatedly. */
void mp_shutdown(void);

/* Begin hosting on `port` / joining `ip`:`port`. Returns false on immediate
 * setup failure; otherwise the connection completes asynchronously (poll
 * mp_get_state() for MP_CONNECTED). */
bool mp_host(int port);
bool mp_join(const char *ip, int port);
void mp_disconnect(void);

MpState     mp_get_state(void);
bool        mp_is_host(void);
const char *mp_status_text(void);   /* short human-readable status for the menu */

/* Initial state sync over the connected socket (host sends, client receives).
 * Call once after MP_CONNECTED, before the first mp_exchange. */
bool mp_send_blob(const uint8_t *data, int size);
bool mp_recv_blob(uint8_t **data, int *size);   /* allocates *data; caller frees */

/* One frame of lockstep: send this peer's 16-bit pad, block for the remote
 * peer's, and return the synchronized controller pads (identical on both
 * peers): host -> *p1=local, *p2=remote; client -> *p1=remote, *p2=local.
 * Returns false on disconnect (state becomes MP_DISCONNECTED). */
bool mp_exchange(uint16_t local, uint16_t *p1, uint16_t *p2);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_MP_SESSION_H */
