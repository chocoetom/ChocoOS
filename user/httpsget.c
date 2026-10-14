// /bin/httpsget - minimal HTTPS client (ring 3). Resolves and connects a TCP
// socket, runs a real BearSSL TLS handshake with certificate-chain validation
// against a CA given as a PEM file, sends a GET, and writes the response body
// to stdout. Built against musl + BearSSL (third_party/bearssl-0.6).
//
//   httpsget <url> [cafile.pem] [server_name]
//
// server_name defaults to the URL host; pass it explicitly when connecting to
// an IP literal but the certificate carries a DNS name (SNI + name check).
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "bearssl.h"

// --- X.509 trust anchor decoded from a PEM CA at run time -------------------
static br_x509_decoder_context g_dec;
static br_x509_trust_anchor g_ta;
static unsigned char g_dn[512];
static size_t g_dn_len;
static int g_have_ta;

static void dn_cb(void *ctx, const void *buf, size_t len) {
    (void)ctx;
    if (g_dn_len + len <= sizeof g_dn) {
        memcpy(g_dn + g_dn_len, buf, len);
        g_dn_len += len;
    }
}

static void pem_dest(void *ctx, const void *src, size_t len) {
    br_x509_decoder_push((br_x509_decoder_context *)ctx, src, len);
}

// Decode the first CERTIFICATE object of a PEM file into a CA trust anchor.
static int load_trust_anchor(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "httpsget: cannot open CA %s (errno=%d)\n", path, errno); return -1; }
    static unsigned char buf[65536];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);

    br_pem_decoder_context pd;
    br_pem_decoder_init(&pd);
    size_t off = 0;
    int started = 0;
    while (off < n) {
        off += br_pem_decoder_push(&pd, buf + off, n - off);
        for (;;) {
            int ev = br_pem_decoder_event(&pd);
            if (ev == 0) break;
            // The decoder keeps the trailing dashes in the object name, so match
            // the "CERTIFICATE" prefix rather than an exact string.
            if (ev == BR_PEM_BEGIN_OBJ && strncmp(br_pem_decoder_name(&pd), "CERTIFICATE", 11) == 0) {
                g_dn_len = 0;
                br_x509_decoder_init(&g_dec, dn_cb, NULL);
                br_pem_decoder_setdest(&pd, pem_dest, &g_dec);
                started = 1;
            } else if (ev == BR_PEM_END_OBJ && started) {
                off = n; // first cert is enough
                break;
            } else if (ev == BR_PEM_ERROR) {
                fprintf(stderr, "httpsget: bad PEM in %s\n", path);
                return -1;
            }        }
    }
    br_x509_pkey *pk = br_x509_decoder_get_pkey(&g_dec);
    if (!pk || g_dn_len == 0) { fprintf(stderr, "httpsget: no usable cert in %s\n", path); return -1; }
    // NOTE: g_dec owns the key buffers, so it must stay alive (it is static).
    g_ta.dn.data = g_dn;
    g_ta.dn.len = g_dn_len;
    g_ta.flags = BR_X509_TA_CA;
    g_ta.pkey.key_type = pk->key_type;
    if (pk->key_type == BR_KEYTYPE_RSA) g_ta.pkey.key.rsa = pk->key.rsa;
    else g_ta.pkey.key.ec = pk->key.ec;
    g_have_ta = 1;
    return 0;
}

// --- socket I/O callbacks for the simplified SSL API ------------------------
static int sock_read(void *ctx, unsigned char *buf, size_t len) {
    for (;;) {
        ssize_t r = read(*(int *)ctx, buf, len);
        if (r < 0 && errno == EINTR) continue;
        return r <= 0 ? -1 : (int)r;
    }
}
static int sock_write(void *ctx, const unsigned char *buf, size_t len) {
    for (;;) {
        ssize_t w = write(*(int *)ctx, buf, len);
        if (w < 0 && errno == EINTR) continue;
        return w <= 0 ? -1 : (int)w;
    }
}

static int host_connect(const char *host, const char *port) {
    struct addrinfo hints, *si, *p;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int err = getaddrinfo(host, port, &hints, &si);
    if (err != 0) { fprintf(stderr, "httpsget: getaddrinfo(%s): %s\n", host, gai_strerror(err)); return -1; }
    int fd = -1;
    for (p = si; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(si);
    if (fd < 0) fprintf(stderr, "httpsget: cannot connect %s:%s\n", host, port);
    return fd;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: httpsget <https-url> [cafile.pem] [server_name]\n");
        return 2;
    }
    const char *url = argv[1];
    if (strncmp(url, "https://", 8) != 0) { fprintf(stderr, "httpsget: only https:// URLs\n"); return 2; }
    url += 8;

    char host[256], port[16] = "443", path[512] = "/";
    const char *slash = strchr(url, '/');
    const char *hostend = slash ? slash : url + strlen(url);
    const char *colon = memchr(url, ':', (size_t)(hostend - url));
    const char *rend = colon ? colon : hostend;
    if ((size_t)(rend - url) >= sizeof host) { fprintf(stderr, "httpsget: host too long\n"); return 2; }
    memcpy(host, url, (size_t)(rend - url));
    host[rend - url] = 0;
    if (colon) {
        size_t pl = (size_t)(hostend - colon - 1);
        if (pl >= sizeof port) pl = sizeof port - 1;
        memcpy(port, colon + 1, pl);
        port[pl] = 0;
    }
    if (slash) snprintf(path, sizeof path, "%s", slash);

    const char *ca = (argc >= 3) ? argv[2] : "/etc/ssl/cacert.pem";
    const char *sni = (argc >= 4) ? argv[3] : host;

    if (load_trust_anchor(ca) != 0) return 1;

    signal(SIGPIPE, SIG_IGN);
    int fd = host_connect(host, port);
    if (fd < 0) return 1;

    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    br_sslio_context ioc;

    br_ssl_client_init_full(&sc, &xc, &g_ta, 1);
    br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof iobuf, 1);

    // Seed the engine's HMAC-DRBG from the kernel CSPRNG (getrandom). BearSSL's
    // own OS seeder expects /dev/urandom, which ChocoOS does not expose.
    unsigned char seed[32];
    if (getrandom(seed, sizeof seed, 0) != (ssize_t)sizeof seed) {
        fprintf(stderr, "httpsget: getrandom failed\n");
        close(fd);
        return 1;
    }
    br_ssl_engine_inject_entropy(&sc.eng, seed, sizeof seed);

    // Certificate validity uses the wall clock. BearSSL's X.509 engine fetches
    // it via time(NULL) when its internal day counter is left at 0 (BR_USE_UNIX_TIME),
    // which musl serves from the kernel RTC (syscall 201 -> rtc::now()). Do NOT
    // call br_x509_minimal_set_time here: it expects days since year 1, not 1970.

    br_ssl_client_reset(&sc, sni, 0);
    br_sslio_init(&ioc, &sc.eng, sock_read, &fd, sock_write, &fd);

    br_sslio_write_all(&ioc, "GET ", 4);
    br_sslio_write_all(&ioc, path, strlen(path));
    br_sslio_write_all(&ioc, " HTTP/1.0\r\nHost: ", 17);
    br_sslio_write_all(&ioc, sni, strlen(sni));
    br_sslio_write_all(&ioc, "\r\nConnection: close\r\n\r\n", 24);
    br_sslio_flush(&ioc);

    for (;;) {
        unsigned char tmp[1024];
        int rlen = br_sslio_read(&ioc, tmp, sizeof tmp);
        if (rlen < 0) break;
        fwrite(tmp, 1, rlen, stdout);
    }
    close(fd);

    int state = br_ssl_engine_current_state(&sc.eng);
    int lerr = br_ssl_engine_last_error(&sc.eng);
    if (state == BR_SSL_CLOSED && (lerr == 0 || lerr == BR_ERR_OK)) return 0;
    fprintf(stderr, "\nhttpsget: TLS ended, engine state=%d err=%d\n", state, lerr);
    return (lerr == 0 || lerr == BR_ERR_OK) ? 0 : 1;
}
