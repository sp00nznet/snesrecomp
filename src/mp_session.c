/*
 * mp_session.c — generic SNES lockstep netplay transport (see mp_session.h).
 * No game-specific code. TCP transport; async connect/accept via SDL threads.
 */
#include "snesrecomp/mp_session.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define SOCK_INVALID INVALID_SOCKET
  #define sock_close   closesocket
  #define sock_errno   WSAGetLastError()
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int sock_t;
  #define SOCK_INVALID (-1)
  #define sock_close   close
  #define sock_errno   errno
#endif

/* ---- module state ---- */
static volatile int s_state   = MP_IDLE;  /* MpState; written by worker, read by main */
static bool         s_is_host = false;
static sock_t       s_listen  = SOCK_INVALID;
static sock_t       s_peer    = SOCK_INVALID;
static SDL_Thread  *s_thread  = NULL;
static volatile int s_cancel  = 0;        /* set to stop the worker */
static char         s_status[96]    = "Idle";
static char         s_join_ip[64]    = "";
static int          s_port           = 0;
static bool         s_wsa_ready       = false;

/* Input-delay buffering: each peer applies inputs `s_delay` frames after they
 * were read, so the network has that many frames of slack and we never block
 * unless the peer falls more than `s_delay` frames behind. Both peers must use
 * the same delay (the host's value is sent to the client on connect). */
#define MP_RING 1024
static uint16_t *s_local_ring  = NULL;   /* heap-allocated in mp_init */
static uint16_t *s_remote_ring = NULL;
static int      s_frame_ctr  = 0;   /* local exchange frame counter */
static int      s_recv_count = 0;   /* remote inputs received (in order) */
static int      s_delay      = 2;
static uint8_t  s_rx[2];            /* partial-packet assembly */
static int      s_rxn        = 0;

static void set_status(const char *s) { SDL_strlcpy(s_status, s, sizeof(s_status)); }
static void set_state(MpState st)      { s_state = (int)st; }

/* ---- low-level send/recv (handle partial transfers) ---- */
static bool send_all(sock_t s, const void *buf, int len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        int n = (int)send(s, p, len, 0);
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}
static bool recv_all(sock_t s, void *buf, int len) {
    char *p = (char *)buf;
    while (len > 0) {
        int n = (int)recv(s, p, len, 0);
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}

static void set_nodelay(sock_t s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
}

static void set_nonblock(sock_t s, int nb) {
#ifdef _WIN32
    u_long m = nb ? 1 : 0; ioctlsocket(s, FIONBIO, &m);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

static bool was_wouldblock(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

/* Pull all currently-available remote input packets without blocking. */
static bool mp_drain(void) {
    set_nonblock(s_peer, 1);
    for (;;) {
        int n = (int)recv(s_peer, (char *)s_rx + s_rxn, 2 - s_rxn, 0);
        if (n > 0) {
            s_rxn += n;
            if (s_rxn == 2) { s_remote_ring[s_recv_count % MP_RING] = (uint16_t)(s_rx[0] | (s_rx[1] << 8)); s_recv_count++; s_rxn = 0; }
            continue;
        }
        if (n == 0) return false;          /* peer closed */
        return was_wouldblock();           /* no more data (true) / real error (false) */
    }
}

/* Block until at least `need` remote inputs have been received. */
static bool mp_recv_until(int need) {
    set_nonblock(s_peer, 0);
    while (s_recv_count < need) {
        int n = (int)recv(s_peer, (char *)s_rx + s_rxn, 2 - s_rxn, 0);
        if (n <= 0) return false;
        s_rxn += n;
        if (s_rxn == 2) { s_remote_ring[s_recv_count % MP_RING] = (uint16_t)(s_rx[0] | (s_rx[1] << 8)); s_recv_count++; s_rxn = 0; }
    }
    return true;
}

/* ---- worker: blocking accept (host) or connect (client) ---- */
static int worker(void *arg) {
    (void)arg;
    if (s_is_host) {
        struct sockaddr_in cli; socklen_t cl = sizeof(cli);
        sock_t c = accept(s_listen, (struct sockaddr *)&cli, &cl);
        if (s_cancel) { if (c != SOCK_INVALID) sock_close(c); return 0; }
        if (c == SOCK_INVALID) { set_status("Accept failed"); set_state(MP_DISCONNECTED); return 0; }
        set_nodelay(c);
        { uint8_t db = (uint8_t)s_delay; send_all(c, &db, 1); }   /* tell client the input delay */
        s_peer = c;
        set_status("Peer connected");
        set_state(MP_CONNECTED);
    } else {
        sock_t c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (c == SOCK_INVALID) { set_status("Socket failed"); set_state(MP_DISCONNECTED); return 0; }
        struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((unsigned short)s_port);
        if (inet_pton(AF_INET, s_join_ip, &addr.sin_addr) != 1) {
            set_status("Bad host address"); sock_close(c); set_state(MP_DISCONNECTED); return 0;
        }
        if (connect(c, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            if (!s_cancel) { set_status("Connect failed"); set_state(MP_DISCONNECTED); }
            sock_close(c); return 0;
        }
        if (s_cancel) { sock_close(c); return 0; }
        set_nodelay(c);
        { uint8_t db; if (recv_all(c, &db, 1)) s_delay = db; }   /* adopt host's input delay */
        s_peer = c;
        set_status("Connected to host");
        set_state(MP_CONNECTED);
    }
    return 0;
}

/* ---- public API ---- */
void mp_init(void) {
    static int once = 0;
    if (!once) { once = 1; s_state = MP_IDLE; s_cancel = 0; }
    if (!s_local_ring)  s_local_ring  = (uint16_t *)calloc(MP_RING, sizeof(uint16_t));
    if (!s_remote_ring) s_remote_ring = (uint16_t *)calloc(MP_RING, sizeof(uint16_t));
#ifdef _WIN32
    if (!s_wsa_ready) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) == 0) s_wsa_ready = true;
    }
#else
    s_wsa_ready = true;
#endif
}

bool mp_host(int port) {
    mp_init();
    mp_disconnect();
    s_frame_ctr = 0; s_recv_count = 0; s_rxn = 0;
    s_is_host = true;
    s_port = port;
    s_cancel = 0;
    s_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen == SOCK_INVALID) { set_status("Socket failed"); return false; }
    int one = 1;
    setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((unsigned short)port);
    if (bind(s_listen, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(s_listen, 1) != 0) {
        set_status("Bind/listen failed (port in use?)");
        sock_close(s_listen); s_listen = SOCK_INVALID; return false;
    }
    char buf[96]; snprintf(buf, sizeof(buf), "Hosting on port %d — waiting for peer...", port);
    set_status(buf);
    set_state(MP_HOSTING);
    s_thread = SDL_CreateThread(worker, "mp_accept", NULL);
    return s_thread != NULL;
}

bool mp_join(const char *ip, int port) {
    mp_init();
    mp_disconnect();
    s_frame_ctr = 0; s_recv_count = 0; s_rxn = 0;
    s_is_host = false;
    s_port = port;
    SDL_strlcpy(s_join_ip, ip ? ip : "127.0.0.1", sizeof(s_join_ip));
    s_cancel = 0;
    char buf[96]; snprintf(buf, sizeof(buf), "Connecting to %s:%d...", s_join_ip, port);
    set_status(buf);
    set_state(MP_CONNECTING);
    s_thread = SDL_CreateThread(worker, "mp_connect", NULL);
    return s_thread != NULL;
}

void mp_disconnect(void) {
    s_cancel = 1;
    if (s_listen != SOCK_INVALID) { sock_close(s_listen); s_listen = SOCK_INVALID; }
    if (s_peer   != SOCK_INVALID) { sock_close(s_peer);   s_peer   = SOCK_INVALID; }
    if (s_thread) { SDL_WaitThread(s_thread, NULL); s_thread = NULL; }
    set_state(MP_IDLE);
    set_status("Idle");
}

MpState     mp_get_state(void) { return (MpState)s_state; }
bool        mp_is_host(void)   { return s_is_host; }
const char *mp_status_text(void) { return s_status; }

bool mp_send_blob(const uint8_t *data, int size) {
    if (s_peer == SOCK_INVALID || size < 0) return false;
    set_nonblock(s_peer, 0);
    uint32_t len = (uint32_t)size;
    uint8_t hdr[4] = { (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24) };
    if (!send_all(s_peer, hdr, 4)) return false;
    return size == 0 ? true : send_all(s_peer, data, size);
}

bool mp_recv_blob(uint8_t **data, int *size) {
    if (s_peer == SOCK_INVALID) return false;
    set_nonblock(s_peer, 0);
    uint8_t hdr[4];
    if (!recv_all(s_peer, hdr, 4)) return false;
    uint32_t len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (len == 0 || len > (16u << 20)) return false;   /* sanity cap 16MB */
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) return false;
    if (!recv_all(s_peer, buf, (int)len)) { free(buf); return false; }
    *data = buf; *size = (int)len;
    return true;
}

bool mp_exchange(uint16_t local, uint16_t *p1, uint16_t *p2) {
    if (s_peer == SOCK_INVALID) return false;
    int F = s_frame_ctr;
    s_local_ring[F % MP_RING] = local;

    /* Send this frame's local input (it will be APPLIED at frame F+delay on both
     * peers). Then pull any arrived remote inputs without blocking, and only
     * block if the input we must apply this frame (index F-delay) hasn't landed
     * yet — i.e. only when the peer has fallen more than `delay` frames behind. */
    set_nonblock(s_peer, 0);
    uint8_t out[2] = { (uint8_t)local, (uint8_t)(local >> 8) };
    if (!send_all(s_peer, out, 2)) goto disc;
    if (!mp_drain()) goto disc;
    if (F - s_delay >= 0 && !mp_recv_until(F - s_delay + 1)) goto disc;

    {
        /* First `delay` frames apply neutral input while the pipeline primes. */
        uint16_t la = (F >= s_delay) ? s_local_ring[(F - s_delay) % MP_RING] : 0;
        uint16_t ra = (F >= s_delay) ? s_remote_ring[(F - s_delay) % MP_RING] : 0;
        if (s_is_host) { *p1 = la; *p2 = ra; }
        else           { *p1 = ra; *p2 = la; }
    }
    s_frame_ctr++;
    return true;

disc:
    set_status("Peer disconnected");
    set_state(MP_DISCONNECTED);
    if (s_peer != SOCK_INVALID) { sock_close(s_peer); s_peer = SOCK_INVALID; }
    return false;
}

void mp_set_delay(int frames) {
    if (frames < 0)  frames = 0;
    if (frames > 30) frames = 30;
    s_delay = frames;
}
int mp_get_delay(void) { return s_delay; }

void mp_shutdown(void) {
    mp_disconnect();
#ifdef _WIN32
    if (s_wsa_ready) { WSACleanup(); s_wsa_ready = false; }
#endif
}
