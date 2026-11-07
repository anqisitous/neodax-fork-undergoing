#include "nx_inspect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libproc.h>
#include <sys/stat.h>

/* ================================================================== */
/* 小さなヘルパ                                                        */
/* ================================================================== */

/* C 文字列を JSON 文字列リテラル (前後の " 含む) に。呼び出し側 free。 */
static char *json_escape(const char *s) {
    if (!s) return NULL;
    size_t cap = strlen(s) * 2 + 8, o = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    out[o++] = '"';
    for (const char *q = s; *q; q++) {
        unsigned char c = (unsigned char)*q;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else out[o++] = (char)c;
    }
    out[o++] = '"';
    out[o] = 0;
    return out;
}

static int ascii_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int ci_prefix(const char *s, const char *p) {
    while (*p) { if (ascii_lower((unsigned char)*s) != ascii_lower((unsigned char)*p)) return 0; s++; p++; }
    return 1;
}
static char *find_ci(const char *hay, const char *needle) {
    for (const char *p = hay; *p; p++) if (ci_prefix(p, needle)) return (char *)p;
    return NULL;
}

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ================================================================== */
/* プロセス解決 (macOS libproc)                                        */
/* ================================================================== */

long nx_inspect_find_pid(const char *app_name) {
    if (!app_name || !*app_name) app_name = "Electron";
    int npid = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (npid <= 0) return -1;
    pid_t *pids = (pid_t *)malloc((size_t)npid + 1024);
    if (!pids) return -1;
    npid = proc_listpids(PROC_ALL_PIDS, 0, pids, npid + 1024);
    if (npid <= 0) { free(pids); return -1; }
    int count = npid / (int)sizeof(pid_t);
    pid_t best = -1, fallback = -1;
    for (int i = 0; i < count; i++) {
        pid_t pid = pids[i];
        if (pid <= 0) continue;
        char name[256] = {0};
        if (proc_name(pid, name, sizeof(name)) <= 0) continue;
        if (!strstr(name, app_name)) continue;
        if (!strstr(name, "Helper")) { best = pid; break; }   /* 本体を優先 */
        if (fallback < 0) fallback = pid;
    }
    free(pids);
    return (long)(best > 0 ? best : fallback);
}

/* ================================================================== */
/* HTTP (Content-Length 準拠)                                          */
/* ================================================================== */

static char *http_get(int port, const char *path, size_t *out_len) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return NULL; }

    char req[512];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nAccept: application/json\r\n\r\n",
             path, port);
    if (write(fd, req, strlen(req)) < 0) { close(fd); return NULL; }

    size_t cap = 65536, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { close(fd); return NULL; }
    size_t hdr_end = 0;
    while (hdr_end == 0 && len + 1 < cap) {
        ssize_t r = read(fd, buf + len, cap - len - 1);
        if (r <= 0) break;
        len += (size_t)r;
        buf[len] = 0;
        char *hp = strstr(buf, "\r\n\r\n");
        if (hp) hdr_end = (size_t)(hp - buf) + 4;
    }
    if (!hdr_end) { free(buf); close(fd); return NULL; }

    long clen = -1;
    char *cl = find_ci(buf, "content-length:");
    if (cl && cl < buf + hdr_end) clen = atol(cl + strlen("content-length:"));
    if (clen >= 0) {
        while ((len - hdr_end) < (size_t)clen) {
            if (len + 1 >= cap) { cap *= 2; char *nb = realloc(buf, cap); if (!nb) break; buf = nb; }
            ssize_t r = read(fd, buf + len, cap - len - 1);
            if (r <= 0) break;
            len += (size_t)r;
        }
    }
    close(fd);
    size_t blen = len > hdr_end ? len - hdr_end : 0;
    char *body = (char *)malloc(blen + 1);
    if (!body) { free(buf); return NULL; }
    memcpy(body, buf + hdr_end, blen);
    body[blen] = 0;
    free(buf);
    if (out_len) *out_len = blen;
    return body;
}

static char *extract_ws_url(const char *json) {
    const char *k = strstr(json, "\"webSocketDebuggerUrl\"");
    if (!k) return NULL;
    const char *p = k + strlen("\"webSocketDebuggerUrl\"");
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return NULL;
    size_t n = (size_t)(end - p);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, p, n);
    out[n] = 0;
    return out;
}

/* ================================================================== */
/* WebSocket (クライアント)                                            */
/* ================================================================== */

static int ws_connect(const char *host, int port, const char *path) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &sa.sin_addr);
    struct timeval tv = { 8, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return -1; }

    char req[1024];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: bnh4YXR0YWNoZGtleTEyMzQ1Ng==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n", path, host, port);
    if (write(fd, req, strlen(req)) < 0) { close(fd); return -1; }
    char buf[4096]; size_t got = 0;
    while (got < sizeof(buf) - 1) {
        ssize_t r = read(fd, buf + got, 1);
        if (r <= 0) { close(fd); return -1; }
        got++; buf[got] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    if (!strstr(buf, "101")) { close(fd); return -1; }
    return fd;
}

static int ws_send_text(int fd, const char *text) {
    size_t len = strlen(text);
    unsigned char mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    unsigned char hdr[14]; size_t hn = 0;
    hdr[hn++] = 0x81;
    if (len < 126) hdr[hn++] = 0x80 | (unsigned char)len;
    else if (len < 65536) { hdr[hn++] = 0x80 | 126; hdr[hn++] = (len >> 8) & 0xff; hdr[hn++] = len & 0xff; }
    else { hdr[hn++] = 0x80 | 127; for (int i = 7; i >= 0; i--) hdr[hn++] = (len >> (8 * i)) & 0xff; }
    unsigned char *out = (unsigned char *)malloc(hn + 4 + len);
    if (!out) return -1;
    memcpy(out, hdr, hn);
    memcpy(out + hn, mask, 4);
    for (size_t i = 0; i < len; i++) out[hn + 4 + i] = (unsigned char)text[i] ^ mask[i % 4];
    ssize_t w = write(fd, out, hn + 4 + len);
    free(out);
    return w < 0 ? -1 : 0;
}

static char *ws_recv_text(int fd) {
    unsigned char h[2];
    ssize_t r = read(fd, h, 2);
    if (r < 2) return NULL;
    int opcode = h[0] & 0x0f;
    uint64_t len = h[1] & 0x7f;
    int masked = h[1] & 0x80;
    if (len == 126) { unsigned char e[2]; if (read(fd, e, 2) < 2) return NULL; len = (e[0] << 8) | e[1]; }
    else if (len == 127) { unsigned char e[8]; if (read(fd, e, 8) < 8) return NULL; len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | e[i]; }
    unsigned char m[4] = {0,0,0,0};
    if (masked) { if (read(fd, m, 4) < 4) return NULL; }
    char *payload = (char *)malloc((size_t)len + 1);
    if (!payload) return NULL;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, payload + got, (size_t)len - got);
        if (n <= 0) { free(payload); return NULL; }
        got += (size_t)n;
    }
    if (masked) for (size_t i = 0; i < len; i++) payload[i] ^= m[i % 4];
    payload[len] = 0;
    if (opcode == 0x8) { free(payload); return NULL; }
    if (opcode == 0x9) { free(payload); return ws_recv_text(fd); }
    return payload;
}

/* ================================================================== */
/* 公開 API                                                            */
/* ================================================================== */

/* 既に inspector が応答しているか。応答するならシグナルは不要。 */
static int nx_inspect_ready(int port) {
    size_t n = 0;
    char *list = http_get(port, "/json/list", &n);
    if (!list) return 0;
    int ok = strstr(list, "webSocketDebuggerUrl") != NULL;
    free(list);
    return ok;
}

/* SIGUSR1 は Node/Electron の inspector を起動するシグナルだが、
 * macOS の既定動作は「プロセス終了」。Electron/Node でない相手に送ると
 * アプリを殺す。送る前に、相手が inspector を持つ種類かを必ず確かめる。 */
static int nx_inspect_is_inspectable(pid_t pid) {
    char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
    if (proc_pidpath(pid, path, sizeof(path)) <= 0) return 0;
    if (strstr(path, "Electron Framework") != NULL) return 1;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strncmp(base, "node", 4) == 0) return 1;
    char *app = strstr(path, ".app/");
    if (app) {
        size_t n = (size_t)(app - path) + 4;
        char probe[PROC_PIDPATHINFO_MAXSIZE];
        snprintf(probe, sizeof(probe),
                 "%.*s/Contents/Frameworks/Electron Framework.framework", (int)n, path);
        struct stat st;
        if (stat(probe, &st) == 0) return 1;
    }
    return 0;
}

int nx_inspect_start(long pid, int port, int wait_ms, FILE *out) {
    if (!out) out = stdout;
    if (pid <= 0) return -1;
    if (port <= 0) port = 9229;
    if (wait_ms <= 0) wait_ms = 4000;

    /* 既に inspector が居るなら、シグナルは一切送らない。 */
    if (!nx_inspect_ready(port)) {
        if (!nx_inspect_is_inspectable((pid_t)pid)) {
            fprintf(out, "  [inspect] 対象は Electron/Node ではないため SIGUSR1 を送りません"
                         " (macOS の既定動作は終了)。pid=%ld\n", pid);
            return -1;
        }
        if (kill((pid_t)pid, SIGUSR1) != 0) {
            fprintf(out, "  [inspect] SIGUSR1 failed: %s\n", strerror(errno));
            return -1;
        }
    }
    int waited = 0;
    while (waited < wait_ms) {
        sleep_ms(150);
        waited += 150;
        size_t n = 0;
        char *list = http_get(port, "/json/list", &n);
        if (list) {
            int ok = strstr(list, "webSocketDebuggerUrl") != NULL;
            free(list);
            if (ok) return 0;
        }
    }
    return -1;
}

int nx_inspect_eval(int port, const char *expression, FILE *out) {
    if (!out) out = stdout;
    if (port <= 0) port = 9229;
    if (!expression) return -1;

    size_t n = 0;
    char *list = http_get(port, "/json/list", &n);
    if (!list) { fprintf(out, "  [inspect] no /json/list on :%d\n", port); return -1; }
    char *wsurl = extract_ws_url(list);
    free(list);
    if (!wsurl) { fprintf(out, "  [inspect] no websocket url\n"); return -1; }

    char host[64] = "127.0.0.1"; int wport = port; char path[512] = "/";
    char *p = wsurl;
    if (strncmp(p, "ws://", 5) == 0) p += 5;
    char *slash = strchr(p, '/');
    if (slash) { snprintf(path, sizeof(path), "%s", slash); *slash = 0; }
    char *colon = strrchr(p, ':');
    if (colon) { wport = atoi(colon + 1); *colon = 0; }
    snprintf(host, sizeof(host), "%s", p);

    int fd = ws_connect(host, wport, path);
    free(wsurl);
    if (fd < 0) { fprintf(out, "  [inspect] websocket connect failed\n"); return -1; }

    char *exjson = json_escape(expression);
    if (!exjson) { close(fd); return -1; }
    char *req = (char *)malloc(strlen(exjson) + 256);
    if (!req) { free(exjson); close(fd); return -1; }
    sprintf(req,
        "{\"id\":1,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":%s,\"returnByValue\":true,\"awaitPromise\":true}}",
        exjson);
    free(exjson);

    if (ws_send_text(fd, req) != 0) { free(req); close(fd); return -1; }
    free(req);

    /* 応答 (id:1) を読むまで数回ポーリング */
    char *result = NULL;
    for (int i = 0; i < 40; i++) {
        char *resp = ws_recv_text(fd);
        if (!resp) break;
        if (strstr(resp, "\"id\":1")) { result = resp; break; }
        free(resp);
        sleep_ms(100);
    }
    close(fd);
    if (!result) { fprintf(out, "  [inspect] no response\n"); return -1; }
    fprintf(out, "  [inspect] %s\n", result);
    free(result);
    return 0;
}

int nx_inspect_run(const nx_inspect_opts_t *opts, const char *expression,
                   char **result_out, FILE *out) {
    if (!out) out = stdout;
    if (result_out) *result_out = NULL;
    nx_inspect_opts_t o;
    if (opts) o = *opts; else { memset(&o, 0, sizeof(o)); }
    if (!o.app_name) o.app_name = "Electron";

    long pid = nx_inspect_find_pid(o.app_name);
    if (pid <= 0) { fprintf(out, "  [inspect] process not found: %s\n", o.app_name); return -1; }
    fprintf(out, "  [inspect] target pid=%ld (%s)\n", pid, o.app_name);
    if (nx_inspect_start(pid, o.port, o.wait_ms, out) != 0) {
        fprintf(out, "  [inspect] inspector did not start on :%d\n", o.port ? o.port : 9229);
        return -1;
    }
    fprintf(out, "  [inspect] inspector up on :%d\n", o.port ? o.port : 9229);
    return nx_inspect_eval(o.port, expression, out);
}
