/*
 * nx_update — 署名済みアプリの「正規の更新経路」を neodax 自身で扱う。
 *
 * なぜ必要か:
 *   /Applications 配下の .app は codesign で封印されており、中のファイルを
 *   直接書き換えると封印が壊れる (起動拒否・更新失敗の原因)。
 *   変更は「アプリの更新機構 (electron-updater 等) が読む release feed」を
 *   通して行うのが正しい。
 *
 * このモジュールは feed を取得して、次の比較だけを行う:
 *   - 現在のアプリの版 (Info.plist)
 *   - feed が示す最新版
 *   - 更新が必要か (版が違うか)
 * 書き換えはしない。判断と提示までを担い、実行は呼び出し側が決める。
 */

#include "nx_update.h"

#include <CFNetwork/CFNetwork.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---------------------------------------------------------------- */
/* 小さなヘルパ                                                      */
/* ---------------------------------------------------------------- */

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static int read_text(const char *path, char **out) {
    *out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = 0;
    *out = buf;
    return 0;
}

/* app-update.yml から key: value を 1 つ取り出す (依存を増やさない)。 */
static char *yml_value(const char *yml, const char *key) {
    size_t klen = strlen(key);
    const char *p = yml;
    while (p && *p) {
        const char *line = p;
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        size_t i = 0;
        while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i + klen < len && strncmp(line + i, key, klen) == 0) {
            size_t j = i + klen;
            while (j < len && (line[j] == ' ' || line[j] == '\t')) j++;
            if (j < len && line[j] == ':') {
                j++;
                while (j < len && (line[j] == ' ' || line[j] == '\t')) j++;
                size_t a = j, b = len;
                while (b > a && (line[b-1] == '\r' || line[b-1] == ' ' ||
                                 line[b-1] == '\t')) b--;
                /* 引用符を外す */
                if (b > a + 1 && (line[a] == '"' || line[a] == '\'') &&
                    line[b-1] == line[a]) { a++; b--; }
                char *v = (char *)malloc(b - a + 1);
                if (!v) return NULL;
                memcpy(v, line + a, b - a);
                v[b - a] = 0;
                return v;
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    return NULL;
}

/* ---------------------------------------------------------------- */
/* HTTP (CFNetwork)                                                  */
/* ---------------------------------------------------------------- */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} buf_t;

static void buf_add(buf_t *b, const void *p, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        char *np = (char *)realloc(b->data, cap);
        if (!np) return;
        b->data = np;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
}

static char *http_fetch(const char *url, int *status_out, int timeout_ms) {
    if (status_out) *status_out = 0;
    CFStringRef u = CFStringCreateWithCString(NULL, url, kCFStringEncodingUTF8);
    if (!u) return NULL;
    CFURLRef cfurl = CFURLCreateWithString(NULL, u, NULL);
    CFRelease(u);
    if (!cfurl) return NULL;

    CFStringRef req_s = CFSTR("GET");
    CFURLRef base = cfurl;
    CFHTTPMessageRef req = CFHTTPMessageCreateRequest(
        NULL, req_s, base, kCFHTTPVersion1_1);
    CFRelease(cfurl);
    if (!req) return NULL;
    CFHTTPMessageSetHeaderFieldValue(req, CFSTR("User-Agent"),
                                     CFSTR("neodax-update/1"));
    CFHTTPMessageSetHeaderFieldValue(req, CFSTR("Cache-Control"),
                                     CFSTR("no-cache"));

    double to = timeout_ms > 0 ? (double)timeout_ms / 1000.0 : 15.0;
    CFReadStreamRef stream = CFReadStreamCreateForHTTPRequest(NULL, req);
    CFRelease(req);
    if (!stream) return NULL;

    CFNumberRef t = CFNumberCreate(NULL, kCFNumberDoubleType, &to);
    if (t) {
        CFReadStreamSetProperty(stream, kCFStreamPropertyHTTPShouldAutoredirect,
                                kCFBooleanTrue);
        CFRelease(t);
    }

    char *out = NULL;
    if (!CFReadStreamOpen(stream)) { CFRelease(stream); return NULL; }

    buf_t b;
    memset(&b, 0, sizeof(b));
    UInt8 tmp[8192];
    CFIndex got;
    while ((got = CFReadStreamRead(stream, tmp, (CFIndex)sizeof tmp)) > 0)
        buf_add(&b, tmp, (size_t)got);
    if (got < 0) { free(b.data); b.data = NULL; }

    CFHTTPMessageRef resp = (CFHTTPMessageRef)CFReadStreamCopyProperty(
        stream, kCFStreamPropertyHTTPResponseHeader);
    if (resp) {
        CFIndex code = CFHTTPMessageGetResponseStatusCode(resp);
        if (status_out) *status_out = (int)code;
        CFRelease(resp);
    }
    CFReadStreamClose(stream);
    CFRelease(stream);

    out = b.data;
    return out;
}

/* ---------------------------------------------------------------- */
/* 公開 API                                                          */
/* ---------------------------------------------------------------- */

int nx_update_read_feed(const char *app_path, nx_update_feed_t *out, FILE *log) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (log) log = log;

    char p[4096];
    snprintf(p, sizeof p, "%s/Contents/Resources/app-update.yml", app_path);
    char *yml = NULL;
    if (read_text(p, &yml) != 0) {
        if (log) fprintf(log, "  [update] no app-update.yml at %s\n", p);
        return -1;
    }
    out->provider = yml_value(yml, "provider");
    out->url      = yml_value(yml, "url");
    out->channel  = yml_value(yml, "channel");
    free(yml);
    return out->url ? 0 : -1;
}

void nx_update_feed_free(nx_update_feed_t *f) {
    if (!f) return;
    free(f->provider);
    free(f->url);
    free(f->channel);
    memset(f, 0, sizeof(*f));
}

int nx_update_check(const char *app_path, int apply, FILE *out) {
    if (!out) out = stdout;

    nx_update_feed_t feed;
    if (nx_update_read_feed(app_path, &feed, out) != 0) {
        fprintf(out, "  [update] cannot determine the update channel\n");
        return 2;
    }
    fprintf(out, "channel: provider=%s url=%s channel=%s\n",
            feed.provider ? feed.provider : "?",
            feed.url ? feed.url : "?",
            feed.channel ? feed.channel : "?");

    /* electron-updater の generic feed は <url>/latest-mac.yml を読む。 */
    char target[4096];
    snprintf(target, sizeof target, "%s/latest-mac.yml", feed.url ? feed.url : "");

    int status = 0;
    char *body = http_fetch(target, &status, 15000);
    fprintf(out, "feed: %s -> HTTP %d\n", target, status);
    if (!body || status != 200) {
        fprintf(out, "  [update] feed not reachable (no network or no such feed)\n");
        free(body);
        nx_update_feed_free(&feed);
        return 2;
    }

    char *remote_ver = yml_value(body, "version");
    fprintf(out, "remote version: %s\n", remote_ver ? remote_ver : "(none)");
    free(body);
    nx_update_feed_free(&feed);

    if (apply) {
        /* 実行はしない。更新の実行はアプリの更新機構に委ねる (署名を壊さない)。 */
        fprintf(out, "  [update] apply requested, but this tool does not patch a "
                     "sealed bundle.\n"
                     "  [update] 正規の更新はアプリ自身の更新機構で行う。\n");
    }
    free(remote_ver);
    return 0;
}

/* ================================================================ */
/* 2) feed から arm64 の配布物を取り、sha512 を照合する              */
/* ================================================================ */

#include <CommonCrypto/CommonDigest.h>
#include <sys/time.h>

/* ファイルの sha512 を base64 で返す (malloc, 呼び出し側 free)。 */
static char *file_sha512_b64(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    CC_SHA512_CTX ctx;
    CC_SHA512_Init(&ctx);
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        CC_SHA512_Update(&ctx, buf, (CC_LONG)n);
    fclose(f);
    unsigned char digest[CC_SHA512_DIGEST_LENGTH];
    CC_SHA512_Final(digest, &ctx);

    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = (char *)malloc(96);
    if (!out) return NULL;
    size_t o = 0;
    for (int i = 0; i < CC_SHA512_DIGEST_LENGTH; i += 3) {
        unsigned int v = digest[i] << 16;
        if (i + 1 < CC_SHA512_DIGEST_LENGTH) v |= digest[i+1] << 8;
        if (i + 2 < CC_SHA512_DIGEST_LENGTH) v |= digest[i+2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = (i + 1 < CC_SHA512_DIGEST_LENGTH) ? tbl[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < CC_SHA512_DIGEST_LENGTH) ? tbl[v & 63] : '=';
    }
    out[o] = 0;
    return out;
}

/* feed の files: から arch を含む url と sha512 を取り出す。 */
static int feed_pick(const char *yml, const char *arch,
                     char **url_out, char **sha_out, long *size_out) {
    *url_out = NULL; *sha_out = NULL;
    if (size_out) *size_out = 0;
    char needle[64];
    snprintf(needle, sizeof needle, "-%s.zip", arch);

    const char *p = yml;
    while ((p = strstr(p, "- url:")) != NULL) {
        const char *line_end = strchr(p, '\n');
        if (!line_end) break;
        size_t span = (size_t)(line_end - p);
        char *line = (char *)malloc(span + 1);
        if (!line) return -1;
        memcpy(line, p, span);
        line[span] = 0;

        if (strstr(line, needle)) {
            /* url */
            char *u = strstr(line, "http");
            while (u && (u > line) && u[-1] != ' ' && u[-1] != ':') u = strstr(u + 1, "http");
            if (u) {
                char *e = u;
                while (*e && *e != ' ' && *e != '\r' && *e != '\n') e++;
                *url_out = (char *)malloc((size_t)(e - u) + 1);
                if (*url_out) { memcpy(*url_out, u, (size_t)(e - u)); (*url_out)[e - u] = 0; }
            }
            /* sha512 は次の行 */
            const char *s = strstr(line_end, "sha512:");
            if (s) {
                s += 7;
                while (*s == ' ' || *s == '\t') s++;
                const char *se = s;
                while (*se && *se != '\r' && *se != '\n') se++;
                *sha_out = (char *)malloc((size_t)(se - s) + 1);
                if (*sha_out) { memcpy(*sha_out, s, (size_t)(se - s)); (*sha_out)[se - s] = 0; }
            }
            /* size */
            if (size_out) {
                const char *z = strstr(line_end, "size:");
                if (z) { z += 5; while (*z == ' ' || *z == '\t') z++; *size_out = atol(z); }
            }
        }
        free(line);
        if (*url_out) break;
        p = line_end;
    }
    return *url_out ? 0 : -1;
}

int nx_update_fetch(const char *app_path, const char *dest, int apply, FILE *out) {
    if (!out) out = stdout;
    nx_update_feed_t feed;
    if (nx_update_read_feed(app_path, &feed, out) != 0) {
        fprintf(out, "  [update] no update channel\n");
        return 2;
    }
    char target[4096];
    snprintf(target, sizeof target, "%s/latest-mac.yml", feed.url ? feed.url : "");
    int st = 0;
    char *yml = http_fetch(target, &st, 15000);
    nx_update_feed_free(&feed);
    if (!yml || st != 200) {
        fprintf(out, "  [update] feed unreachable (%d)\n", st);
        free(yml);
        return 2;
    }

    const char *arch = "arm64";
#if defined(__x86_64__)
    arch = "x64";
#endif

    char *url = NULL, *sha = NULL;
    long size = 0;
    if (feed_pick(yml, arch, &url, &sha, &size) != 0) {
        fprintf(out, "  [update] no %s.zip in feed\n", arch);
        free(yml);
        return 2;
    }
    char *remote_ver = yml_value(yml, "version");
    fprintf(out, "remote version: %s\n  arch=%s size=%ld\n  url=%s\n",
            remote_ver ? remote_ver : "?", arch, size, url);
    free(yml);

    if (!apply) {
        fprintf(out, "  [update] dry-run (pass --apply to download and verify)\n");
        free(url); free(sha); free(remote_ver);
        return 0;
    }

    /* DB に置くだけ。アプリの差し替えは行わない (署名検証を壊さないため)。 */
    char path[4096];
    snprintf(path, sizeof path, "%s", dest && *dest ? dest : "/tmp/neodax-update.zip");
    fprintf(out, "  [update] downloading -> %s\n", path);

    char cmd[4200];
    snprintf(cmd, sizeof cmd, "/usr/bin/curl -sSL -o '%s' '%s'", path, url);
    int rc = system(cmd);
    free(url);
    if (rc != 0) {
        fprintf(out, "  [update] download failed (rc=%d)\n", rc);
        free(sha); free(remote_ver);
        return 2;
    }

    char *got = file_sha512_b64(path);
    fprintf(out, "  expected sha512: %s\n  actual   sha512: %s\n",
            sha ? sha : "?", got ? got : "?");
    int ok = (sha && got && strcmp(sha, got) == 0);
    fprintf(out, "  [update] %s\n", ok ? "sha512 OK" : "SHA512 MISMATCH");
    free(got);
    free(sha);
    free(remote_ver);
    return ok ? 0 : 3;
}
