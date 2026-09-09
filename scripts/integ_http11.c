/* Loopback API-parity proof (pre-M7): H2O-shaped HTTP/1.x exchange over TCP.
 *
 * A single program plays both ends of real socket traffic, mirroring H2O's
 * call pattern (request parse + chunked decode server-side, response parse
 * client-side) — but the consumer is self-authored, NOT H2O itself (full
 * H2O build is out of scope on this machine; see docs). What this proves is
 * scoped to the exercised fixtures/splits below:
 *   server side (cf. h2o lib/http1.c): phr_parse_request (streaming, with
 *                last_len) + phr_decode_chunked chunked-upload loop;
 *   client side (cf. h2o lib/common/http1client.c): phr_parse_response
 *                (streaming) + phr_decode_chunked with consume_trailer.
 * Plus one standalone phr_parse_headers call so all five entry points run.
 *
 * Build twice from THE SAME source — once against reference/picohttpparser.c
 * (oracle), once against the Rust cdylib — and diff the transcripts. The
 * transcripts must be byte-identical on these fixtures. That is API parity
 * on exercised paths — not a claim about untested shapes (see header).
 *
 * No pointers, ports, timings, or addresses are printed: output is fully
 * deterministic. Exit nonzero on any failed assertion.
 *
 * Windows/MSVC+MinGW portable (Winsock2, link -lws2_32).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#define SOCKERR WSAGetLastError()
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define CLOSESOCK close
#define SOCKERR -1
#define INVALID_SOCKET -1
#endif

#include "picohttpparser.h"

#define FAIL(...) do { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); return 1; } while (0)

/* Fixed traffic fixtures (deterministic). */
static const char REQ_HEAD[] =
    "POST /submit?q=1 HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 11\r\n"
    "X-Note: abc\r\n"
    " def\r\n"
    "Connection: close\r\n"
    "\r\n";
/* 11 body bytes follow the head on the wire. */
static const char REQ_BODY[] = "hello world";
/* Chunked upload the server decodes (extension + split delivery). */
static const char UPLOAD[] = "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\n\r\n";

static const char RESP_HEAD[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n";
static const char RESP_CHUNKS[] = "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nX-Trailer: yes\r\n\r\n";

/* CI guard: a parser defect that stalls delivery must FAIL, never hang the
 * run forever. 15 s per recv is eons for loopback fixtures. */
static int set_timeout(sock_t s)
{
#ifdef _WIN32
    DWORD ms = 15000;
    return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = 15;
    tv.tv_usec = 0;
    return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

static int send_all(sock_t s, const char *b, size_t n)
{
    while (n > 0) {
        int r = send(s, b, (int)n, 0);
        if (r <= 0)
            return -1;
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

/* Server side: read head incrementally (streaming, like an event loop),
 * parse request, standalone-parse the header block, decode chunked upload. */
static int run_server(sock_t s)
{
    static char buf[4096];
    size_t have = 0, prev = 0;
    const char *method, *path;
    size_t method_len, path_len;
    int minor_version, ret, i;
    size_t num_headers;
    struct phr_header headers[32];
    struct phr_header h2[32];
    size_t n2;

    /* Fixed 32-byte reads: delivery splits at deterministic boundaries so
     * the -2/continue streaming path (and last_len chaining) actually
     * executes instead of completing on the first recv. */
    for (;;) {
        size_t want = sizeof(buf) - have;
        int r;
        if (want > 32)
            want = 32;
        r = recv(s, buf + have, (int)want, 0);
        if (r <= 0)
            FAIL("server recv");
        have += (size_t)r;
        num_headers = sizeof(headers) / sizeof(headers[0]);
        method = path = NULL;
        method_len = path_len = 0;
        minor_version = -1;
        ret = phr_parse_request(buf, have, &method, &method_len, &path, &path_len,
                                &minor_version, headers, &num_headers, prev);
        prev = have;
        if (ret == -2)
            continue;
        if (ret < 0)
            FAIL("server head parse ret=%d", ret);
        break;
    }
    printf("server: method=%.*s path=%.*s version=1.%d headers=%lu headlen=%d\n",
           (int)method_len, method, (int)path_len, path, minor_version,
           (unsigned long)num_headers, ret);
    for (i = 0; i < (int)num_headers; i++) {
        if (headers[i].name)
            printf("server: h[%d] %.*s: %.*s\n", i, (int)headers[i].name_len, headers[i].name,
                   (int)headers[i].value_len, headers[i].value);
        else
            printf("server: h[%d] (cont): %.*s\n", i, (int)headers[i].value_len, headers[i].value);
    }
    /* Standalone header-block parse of the same bytes (all five entry
     * points run in this traffic proof). NOTE: headlen is saved first —
     * the call below reuses `ret`, which the carry-over math needs. */
    int headlen = ret;
    {
        const char *hs = strstr(buf, "\r\n") + 2;
        size_t hlen = (size_t)(buf + (size_t)headlen - hs);
        int hret;
        n2 = sizeof(h2) / sizeof(h2[0]);
        hret = phr_parse_headers(hs, hlen, h2, &n2, 0);
        printf("server: standalone-headers ret=%d count=%lu\n", hret, (unsigned long)n2);
        if (hret != (int)hlen || n2 != num_headers)
            FAIL("standalone header parse disagrees");
        /* Content check, not just count: first header must match. */
        if (n2 == 0 || h2[0].name == NULL ||
            h2[0].name_len != headers[0].name_len ||
            memcmp(h2[0].name, headers[0].name, h2[0].name_len) != 0 ||
            h2[0].value_len != headers[0].value_len ||
            memcmp(h2[0].value, headers[0].value, h2[0].value_len) != 0)
            FAIL("standalone header content disagrees");
        printf("server: standalone-headers h[0] matches request parse\n");
    }
    /* Anything already read past the head (Content-Length body + chunked
     * upload bytes) carries over — like a real event loop, never re-recv'd. */
    {
        size_t body_skip = 0;
        int k;
        for (k = 0; k < (int)num_headers; k++) {
            if (headers[k].name && headers[k].name_len == 14 &&
                memcmp(headers[k].name, "Content-Length", 14) == 0) {
                size_t v = 0, j;
                for (j = 0; j < headers[k].value_len; j++) {
                    char c = headers[k].value[j];
                    if (c < '0' || c > '9')
                        FAIL("bad content-length");
                    v = v * 10 + (size_t)(c - '0');
                }
                body_skip = v;
            }
        }
        if ((size_t)headlen + body_skip > have)
            FAIL("head+body exceed buffered bytes");
        memmove(buf, buf + headlen + body_skip, have - (size_t)headlen - body_skip);
        have -= (size_t)headlen + body_skip;
        prev = 0; /* stream offsets restart on the carried tail (documented) */
    }
    /* Chunked upload decodes next on the same connection. */
    {
        struct phr_chunked_decoder dec;
        static char cbuf[4096];
        size_t have2 = 0, out = 0;
        char decoded[256];
        size_t dlen = 0;
        int r2;
        memset(&dec, 0, sizeof(dec));
        dec.consume_trailer = 0;
        /* seed the stage with already-buffered tail bytes, then stream */
        if (have > sizeof(cbuf))
            FAIL("carried tail overflow");
        memcpy(cbuf, buf, have);
        have2 = have;
        /* Contract: each call supplies only NEWLY arrived bytes. A -2
         * means framing state advanced in the decoder; nothing is
         * carried — the next call gets fresh recv bytes only. */
        for (;;) {
            size_t n = have2;
            int r = 0;
            if (have2 == 0) {
                /* 24-byte reads: force multi-call decode continuation. */
                r = recv(s, cbuf, 24, 0);
                if (r <= 0)
                    FAIL("server upload recv");
                have2 = n = (size_t)r;
            }
            r2 = phr_decode_chunked(&dec, cbuf, &n);
            if (n > 0) {
                if (dlen + n >= sizeof(decoded))
                    FAIL("decoded overflow");
                memcpy(decoded + dlen, cbuf, n);
                dlen += n;
            }
            have2 = 0; /* every supplied byte is consumed per call */
            if (r2 == -2)
                continue;
            if (r2 < 0)
                FAIL("server upload decode ret=%d", r2);
            out = n;
            break;
        }
        decoded[dlen] = '\0';
        printf("server: upload decoded=%lu leftover=%d body=%s in_data=%d\n",
               (unsigned long)dlen, r2, decoded, phr_decode_chunked_is_in_data(&dec));
        if (strcmp(decoded, "hello world") != 0)
            FAIL("upload body mismatch");
        (void)out;
    }
    /* Reply: response head + chunked body with trailer. */
    if (send_all(s, RESP_HEAD, strlen(RESP_HEAD)) != 0)
        FAIL("server send head");
    if (send_all(s, RESP_CHUNKS, strlen(RESP_CHUNKS)) != 0)
        FAIL("server send chunks");
    return 0;
}

/* Client send phase: split delivery on the wire. */
int client_send_phase(sock_t s)
{
    /* Split delivery on the wire: head in two chunks, then the body. */
    if (send_all(s, REQ_HEAD, 30) != 0)
        FAIL("client send 1");
    if (send_all(s, REQ_HEAD + 30, strlen(REQ_HEAD) - 30) != 0)
        FAIL("client send 2");
    if (send_all(s, REQ_BODY, strlen(REQ_BODY)) != 0)
        FAIL("client send body");
    if (send_all(s, UPLOAD, strlen(UPLOAD)) != 0)
        FAIL("client send upload");
    return 0;
}

/* Client read phase: stream-parse the response head in 7-byte reads,
 * decode the chunked body with trailers. */
int client_read_phase(sock_t s)
{
    static char buf[4096];
    size_t have = 0, prev = 0;
    int minor_version, status, ret;
    const char *msg;
    size_t msg_len, num_headers, i;
    struct phr_header headers[32];
    char rbuf[16];
    int r;

    for (;;) {
        r = recv(s, rbuf, (int)sizeof(rbuf), 0);
        if (r <= 0)
            FAIL("client head recv");
        if (have + (size_t)r > sizeof(buf))
            FAIL("client head overflow");
        memcpy(buf + have, rbuf, (size_t)r);
        have += (size_t)r;
        num_headers = sizeof(headers) / sizeof(headers[0]);
        msg = NULL;
        msg_len = 0;
        minor_version = status = -1;
        ret = phr_parse_response(buf, have, &minor_version, &status, &msg, &msg_len,
                                 headers, &num_headers, prev);
        prev = have;
        if (ret == -2)
            continue;
        if (ret < 0)
            FAIL("client head parse ret=%d", ret);
        break;
    }
    printf("client: version=1.%d status=%d reason=%.*s headers=%lu headlen=%d\n",
           minor_version, status, (int)msg_len, msg ? msg : "", (unsigned long)num_headers, ret);
    for (i = 0; i < num_headers; i++)
        if (headers[i].name)
            printf("client: h[%lu] %.*s: %.*s\n", (unsigned long)i, (int)headers[i].name_len,
                   headers[i].name, (int)headers[i].value_len, headers[i].value);
        else
            printf("client: h[%lu] (cont): %.*s\n", (unsigned long)i,
                   (int)headers[i].value_len, headers[i].value);
    /* Remainder of the head buffer is already-received body bytes. */
    {
        struct phr_chunked_decoder dec;
        static char cbuf[4096];
        size_t have2 = have - (size_t)ret, dlen = 0;
        char decoded[256];
        int r2, saw_data = 0;
        memset(&dec, 0, sizeof(dec));
        dec.consume_trailer = 1;
        memcpy(cbuf, buf + ret, have2);
        /* Contract: each call supplies only NEWLY arrived bytes (a -2
         * means framing state advanced; decoded bytes are output). */
        for (;;) {
            size_t n = have2;
            int r = 0;
            if (have2 == 0) {
                /* 7-byte reads: force split delivery */
                r = recv(s, rbuf, 7, 0);
                if (r <= 0)
                    FAIL("client body EOF while incomplete");
                have2 = n = (size_t)r;
                memcpy(cbuf, rbuf, n);
            }
            r2 = phr_decode_chunked(&dec, cbuf, &n);
            if (phr_decode_chunked_is_in_data(&dec))
                saw_data = 1;
            if (n > 0) {
                if (dlen + n >= sizeof(decoded))
                    FAIL("client decoded overflow");
                memcpy(decoded + dlen, cbuf, n);
                dlen += n;
            }
            have2 = 0; /* every supplied byte is consumed per call */
            if (r2 == -2)
                continue;
            if (r2 < 0)
                FAIL("client body decode ret=%d", r2);
            break;
        }
        decoded[dlen] = '\0';
        printf("client: body decoded=%lu leftover=%d body=%s saw_data=%d\n",
               (unsigned long)dlen, r2, decoded, saw_data);
        if (strcmp(decoded, "hello world") != 0)
            FAIL("client body mismatch");
        if (r2 != 0)
            FAIL("client leftover=%d, want 0 (trailer consumed)", r2);
    }
    return 0;
}

int main(void)
{
    sock_t listen_s = INVALID_SOCKET, server_s = INVALID_SOCKET;
    sock_t client_s = INVALID_SOCKET;
    struct sockaddr_in addr;
    socklen_t addrlen = (socklen_t)sizeof(addr);
    int rc = 0;
    int wsa_ok = 0;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("FAIL: WSAStartup\n");
        return 1;
    }
    wsa_ok = 1;
#else
    (void)wsa_ok;
#endif

    listen_s = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_s == INVALID_SOCKET) {
        printf("FAIL: socket %d\n", SOCKERR);
        rc = 1;
        goto done;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* ephemeral; never printed (deterministic transcript) */
    if (bind(listen_s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("FAIL: bind %d\n", SOCKERR);
        rc = 1;
        goto done;
    }
    if (listen(listen_s, 1) != 0) {
        printf("FAIL: listen\n");
        rc = 1;
        goto done;
    }
    if (getsockname(listen_s, (struct sockaddr *)&addr, &addrlen) != 0) {
        printf("FAIL: getsockname\n");
        rc = 1;
        goto done;
    }
    client_s = socket(AF_INET, SOCK_STREAM, 0);
    if (client_s == INVALID_SOCKET) {
        printf("FAIL: client socket\n");
        rc = 1;
        goto done;
    }
    if (connect(client_s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("FAIL: connect %d\n", SOCKERR);
        rc = 1;
        goto done;
    }
    server_s = accept(listen_s, NULL, NULL);
    if (server_s == INVALID_SOCKET) {
        printf("FAIL: accept\n");
        rc = 1;
        goto done;
    }
    if (set_timeout(server_s) != 0 || set_timeout(client_s) != 0) {
        printf("FAIL: set_timeout\n");
        rc = 1;
        goto done;
    }
    /* Fixtures are tiny so no deadlock: client sends are buffered by the
     * stack while the server runs, then the client reads the reply. */
    if (client_send_phase(client_s) != 0) {
        rc = 1;
        goto done;
    }
    if (run_server(server_s) != 0) {
        rc = 1;
        goto done;
    }
    /* Reply fully sent: close so the client sees EOF after the bytes. */
    CLOSESOCK(server_s);
    server_s = INVALID_SOCKET;
    if (client_read_phase(client_s) != 0)
        rc = 1;
done:
    if (server_s != INVALID_SOCKET)
        CLOSESOCK(server_s);
    if (client_s != INVALID_SOCKET)
        CLOSESOCK(client_s);
    if (listen_s != INVALID_SOCKET)
        CLOSESOCK(listen_s);
#ifdef _WIN32
    if (wsa_ok)
        WSACleanup();
#endif
    if (rc == 0)
        printf("INTEGRATION OK\n");
    return rc;
}
