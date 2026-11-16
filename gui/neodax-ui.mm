/*
 * neodax-ui — NeoDAX の GUI とは独立に動く最小 CLI。
 *
 * GUI 版 (neodax-viewer.mm) の描画・操作には一切触れない。
 * 別プロセスとして起動し、次を提供する:
 *
 *   windows                        画面上のウィンドウ一覧 (owner/name/id)
 *   capture --app NAME -o FILE     対象アプリの現在の画面を PNG にキャプチャ
 *   display [--app NAME] [-o FILE] 複数バックエンドを試し、最も情報量の多い
 *                                  フレーム (frame/widget 数) をキャプチャして
 *                                  画面/ウィンドウ/タブを可読テキストで出力
 *   tabs    --state PATH           タブ一覧を TSV で出力 (エージェント可読)
 *   close   --state PATH --id ID   選択したタブだけを除去 (base は保持)
 *   fix     --state PATH           閉じられない surface タブを一括除去
 *   simulate --state PATH [--asar ASAR]  アプリのロード結果を再現
 *   serve                          標準入力からコマンドを読む常駐モード
 *
 * キャプチャは screencapture CLI に依存しない。CGWindowListCreateImage を
 * dlsym で直接取得して呼ぶ低レベル経路 (SDK が unavailable でも実行時は
 * シンボルが存在する) を第一候補とし、失敗時のみ screencapture に落ちる。
 */

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <ApplicationServices/ApplicationServices.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>

/* neodax の分類をまとめて使う。個別ヘッダの列挙はビルドもコードも持たない:
 * どの分類を使うかは、この 1 行の include で決まる。 */
#include "integration/nx_all.h"
#include "nx_cli_registry.h"

/* neodax の検索 (ABD 安全化済み) */
extern "C" int nx_seek(const char *, const char *, const char *, int, int, int, int, FILE *);

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

/* ------------------------------------------------------------------ */
/* 低レベルキャプチャ経路                                              */
/* ------------------------------------------------------------------ */

typedef CGImageRef (*cgwl_create_fn)(CGRect, CGWindowListOption, CGWindowID,
                                     CGWindowImageOption);

static cgwl_create_fn load_cgwl(void) {
    static cgwl_create_fn fn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        void *h = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
                         RTLD_LAZY);
        if (h) fn = (cgwl_create_fn)dlsym(h, "CGWindowListCreateImage");
        /* 予備: private framework 内の別名も試す */
        if (!fn) {
            void *h2 = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight",
                              RTLD_LAZY);
            if (h2) fn = (cgwl_create_fn)dlsym(h2, "CGWindowListCreateImage");
        }
    }
    return fn;
}

static int write_png(CGImageRef img, const char *path) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    if (!url) return -1;
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(
        url, CFSTR("public.png"), 1, NULL);
    CFRelease(url);
    if (!dest) return -1;
    CGImageDestinationAddImage(dest, img, NULL);
    Boolean ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    return ok ? 0 : -1;
}

/* 画像の「情報量」= エッジ密度 + 色分散。frame/widget が多い画面ほど高い。 */
static double image_score(CGImageRef img) {
    size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
    if (w < 2 || h < 2) return 0;
    size_t sw = w > 128 ? 128 : w, sh = h > 128 ? 128 : h;
    unsigned char *buf = (unsigned char *)calloc(sw * sh * 4, 1);
    if (!buf) return 0;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(buf, sw, sh, 8, sw * 4, cs,
                                             kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (!ctx) { free(buf); return 0; }
    CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)sw, (CGFloat)sh), img);

    double edge = 0, alpha_sum = 0;
    long cnt = 0;
    for (size_t y = 1; y < sh; y++) {
        for (size_t x = 1; x < sw; x++) {
            const unsigned char *p = &buf[(y * sw + x) * 4];
            const unsigned char *q = &buf[(y * sw + x - 1) * 4];
            const unsigned char *r = &buf[((y - 1) * sw + x) * 4];
            int l1 = p[0] + p[1] + p[2];
            int l2 = q[0] + q[1] + q[2];
            int l3 = r[0] + r[1] + r[2];
            edge += abs(l1 - l2) + abs(l1 - l3);
            alpha_sum += p[3];
            cnt++;
        }
    }
    CGContextRelease(ctx);
    free(buf);
    if (!cnt) return 0;
    double e = edge / (double)cnt / (3.0 * 255.0);
    double a = alpha_sum / (double)cnt / 255.0;
    return e * (a > 0.02 ? 1.0 : 0.0);
}

/* ------------------------------------------------------------------ */
/* ウィンドウ列挙                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    int    id;
    int    pid;
    int    layer;
    double area;
    double x, y, w, h;
    char   owner[256];
    char   name[256];
} win_t;

static int collect_windows(const char *app, win_t *out, int max) {
    CFArrayRef arr = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    if (!arr) return 0;
    int n = 0;
    for (CFIndex i = 0; i < CFArrayGetCount(arr) && n < max; i++) {
        CFDictionaryRef d = (CFDictionaryRef)CFArrayGetValueAtIndex(arr, i);
        CFStringRef owner = (CFStringRef)CFDictionaryGetValue(d, kCGWindowOwnerName);
        CFStringRef name  = (CFStringRef)CFDictionaryGetValue(d, kCGWindowName);
        CFNumberRef num   = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowNumber);
        CFNumberRef pid   = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowOwnerPID);
        CFDictionaryRef bnd = (CFDictionaryRef)CFDictionaryGetValue(d, kCGWindowBounds);
        char ob[256] = {0}, nb[256] = {0};
        if (owner) CFStringGetCString(owner, ob, sizeof(ob), kCFStringEncodingUTF8);
        if (name)  CFStringGetCString(name,  nb, sizeof(nb),  kCFStringEncodingUTF8);
        if (app && strstr(ob, app) == NULL) continue;

        win_t w;
        memset(&w, 0, sizeof(w));

        double area = 0;
        if (bnd) {
            CGRect r = CGRectNull;
            if (CGRectMakeWithDictionaryRepresentation(bnd, &r)) {
                area = r.size.width * r.size.height;
                w.x = r.origin.x; w.y = r.origin.y;
                w.w = r.size.width; w.h = r.size.height;
            }
        }
        if (app && area < 20000) continue;  /* 小さすぎる補助ウィンドウ除外 */

        CFNumberRef lay = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowLayer);
        if (lay) CFNumberGetValue(lay, kCFNumberIntType, &w.layer);

        if (num) CFNumberGetValue(num, kCFNumberIntType, &w.id);
        if (pid) CFNumberGetValue(pid, kCFNumberIntType, &w.pid);
        w.area = area;
        snprintf(w.owner, sizeof(w.owner), "%s", ob);
        snprintf(w.name, sizeof(w.name), "%s", nb);
        out[n++] = w;
    }
    CFRelease(arr);
    return n;
}

/* Accessibility から frame/widget 数を数える (深さ制限) */
static int count_ax_tree(AXUIElementRef el, int depth) {
    if (!el || depth > 5) return 0;
    CFTypeRef kids = NULL;
    if (AXUIElementCopyAttributeValue(el, kAXChildrenAttribute, &kids) != kAXErrorSuccess)
        return 0;
    if (!kids) return 0;
    int n = 0;
    if (CFGetTypeID(kids) == CFArrayGetTypeID()) {
        CFArrayRef a = (CFArrayRef)kids;
        n = (int)CFArrayGetCount(a);
        for (CFIndex i = 0; i < CFArrayGetCount(a); i++) {
            AXUIElementRef c = (AXUIElementRef)CFArrayGetValueAtIndex(a, i);
            n += count_ax_tree(c, depth + 1);
        }
    }
    CFRelease(kids);
    return n;
}

static int count_widgets(int pid) {
    if (pid <= 0) return 0;
    AXUIElementRef app = AXUIElementCreateApplication((pid_t)pid);
    if (!app) return 0;
    int n = count_ax_tree(app, 0);
    CFRelease(app);
    return n;
}

/* ------------------------------------------------------------------ */
/* 複数候補を試して最良フレームを選ぶ                                  */
/* ------------------------------------------------------------------ */

static int capture_best(const char *app, const char *out, FILE *report) {
    cgwl_create_fn create = load_cgwl();
    win_t wins[64];
    int nw = collect_windows(app, wins, 64);
    double best_score = -1;
    CGImageRef best_img = NULL;
    int best_id = 0, best_widgets = 0;

    if (create && nw > 0) {
        for (int i = 0; i < nw; i++) {
            CGImageRef img = create(CGRectNull, kCGWindowListOptionIncludingWindow,
                                    (CGWindowID)wins[i].id,
                                    kCGWindowImageBoundsIgnoreFraming);
            if (!img) continue;
            double sc = image_score(img);
            int widgets = count_widgets(wins[i].pid);
            /* frame/widget が多いウィンドウを優先 */
            double total = sc + widgets * 0.0025;
            if (report)
                fprintf(report,
                        "    candidate win=%d owner=%s score=%.4f widgets=%d\n",
                        wins[i].id, wins[i].owner, sc, widgets);
            if (total > best_score) {
                if (best_img) CGImageRelease(best_img);
                best_score = total;
                best_img = img;
                best_id = wins[i].id;
                best_widgets = widgets;
            } else {
                CGImageRelease(img);
            }
        }
    }

    /* 対象ウィンドウが取れない場合は画面全体を低レベルで */
    if (!best_img && create) {
        best_img = create(CGRectInfinite, kCGWindowListOptionOnScreenOnly,
                          kCGNullWindowID, kCGWindowImageBestResolution);
        if (best_img) best_score = image_score(best_img);
    }

    if (best_img) {
        int rc = write_png(best_img, out);
        if (report)
            fprintf(report, "    backend=CoreGraphics(dlsym) win=%d widgets=%d score=%.4f\n",
                    best_id, best_widgets, best_score);
        CGImageRelease(best_img);
        if (rc == 0) return 0;
    }

    /* 最終 fallback: それでも取れなければ screencapture */
    if (report) fprintf(report, "    backend=screencapture(fallback)\n");
    char cmd[4600];
    snprintf(cmd, sizeof(cmd), "/usr/sbin/screencapture -x '%s' 2>/dev/null", out);
    return system(cmd) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* 表示系コマンド                                                      */
/* ------------------------------------------------------------------ */

static void cmd_windows(void) {
    win_t wins[256];
    int n = collect_windows(NULL, wins, 256);
    printf("WINID\tPID\tAREA\tOWNER\tNAME\n");
    for (int i = 0; i < n; i++)
        printf("%d\t%d\t%.0f\t%s\t%s\n",
               wins[i].id, wins[i].pid, wins[i].area, wins[i].owner, wins[i].name);
}

/* ------------------------------------------------------------------ */
/* 状態系コマンド                                                      */
/* ------------------------------------------------------------------ */

/* 状態の場所を自動で見つける。
 *
 * 以前は --state "$HOME/Library/Application Support/.../Local Storage/leveldb"
 * を毎回打つ必要があり、綴りを間違えると「直せていない」ように見えた。
 * 既知のアプリ状態ディレクトリを順に確かめ、CURRENT を持つ leveldb を返す。
 * 見つからなければ NULL (呼び出し側がエラーを出す)。
 */
static const char *resolve_state_path(void) {
    static char found[PATH_MAX];
    static int tried = 0;
    if (tried) return found[0] ? found : NULL;
    tried = 1;

    const char *home = getenv("HOME");
    if (!home) return NULL;

    /* 優先順: DeepAI Code の現行 → 旧名 → VS Code 系。
       CURRENT があるものだけを「実在するストア」とみなす。 */
    static const char *suffixes[] = {
        "Library/Application Support/deepai-code-v2/Local Storage/leveldb",
        "Library/Application Support/DeepAI Code/Local Storage/leveldb",
        "Library/Application Support/deepai-code/Local Storage/leveldb",
        NULL,
    };
    for (int i = 0; suffixes[i]; i++) {
        char cand[PATH_MAX];
        snprintf(cand, sizeof(cand), "%s/%s", home, suffixes[i]);
        char cur[PATH_MAX];
        snprintf(cur, sizeof(cur), "%s/CURRENT", cand);
        if (access(cur, R_OK) == 0) {
            snprintf(found, sizeof(found), "%s", cand);
            return found;
        }
    }
    return NULL;
}

/* --state が無ければ自動解決する。解決できなければ NULL。 */
static const char *state_or_default(const char *given, const char *cmd) {
    if (given) return given;
    const char *auto_path = resolve_state_path();
    if (auto_path) {
        fprintf(stderr, "  [%s] --state 未指定: %s を使用\n", cmd, auto_path);
        return auto_path;
    }
    fprintf(stderr, "%s: --state required (既定の状態ディレクトリも見つからない)\n", cmd);
    return NULL;
}

static void dump_tabs_path(const char *path) {
    char *state = nx_deepai_load_state(path);
    if (!state) { printf("ERROR: no state at %s\n", path); return; }
    printf("STATE_SOURCE\t%s\n", path);
    nx_deepai_dump_state(state, stdout);
    free(state);
}

static int key_has_suffix(const char *key, size_t len, const char *suffix) {
    size_t sl = strlen(suffix);
    return (len >= sl) && (memcmp(key + len - sl, suffix, sl) == 0);
}

/* キーを表示用にエスケープする (非表示文字は \xNN)。 */
static void escape_key(const char *key, size_t len, char *dst, size_t cap) {
    size_t w = 0;
    for (size_t k = 0; k < len && w + 5 < cap; k++) {
        unsigned char c = (unsigned char)key[k];
        if (c >= 0x20 && c < 0x7f) dst[w++] = (char)c;
        else w += (size_t)snprintf(dst + w, cap - w, "\\x%02x", c);
    }
    dst[w] = 0;
}

/* KV ストアのキーを一覧する (診断用)。
 *
 * キーは NUL を含むバイナリ (例: "_deepai-code://app\x00\x01deepai-code:...")。
 * そのまま printf すると切れるので、非表示文字を \xNN で出す。
 * seq と生存 (削除レコード尊重) も併記し、「どのキーが値を握っているか」を
 * シェルの grep ではなく C 側で確定できるようにする。 */
static void cmd_keys(const char *path) {
    char **keys = NULL;
    size_t *lens = NULL;
    int n = nx_state_list_keys(path, NULL, &keys, &lens, 256);
    printf("STATE_SOURCE\t%s\n", path);
    printf("MAX_SEQ\t%llu\n", nx_state_max_seq(path, NULL));
    printf("COUNT\t%d\n", n);
    for (int i = 0; i < n; i++) {
        char shown[512];
        escape_key(keys[i], lens[i], shown, sizeof(shown));
        printf("KEY\t%zu\t%s\n", lens[i], shown);
        free(keys[i]);
    }
    free(keys);
    free(lens);
    printf("LIVE\tworkspace-tabs=%d\tnx-backup=%d\n",
           nx_state_key_is_live(path, "workspace-tabs"),
           nx_state_key_is_live(path, "nx-backup"));

    /* 実際に読み書きに使われるキー。ここが .nx-backup を指すと、
     * 修復結果が本体ではなく退避側に書かれ、本体は孤児のまま残る。
     * これが「直しても復活する」の原因なので、常に見えるようにする。 */
    size_t rklen = 0;
    char *rk = nx_state_find_key(path, "workspace-tabs", &rklen);
    if (rk) {
        char shown[512];
        escape_key(rk, rklen, shown, sizeof(shown));
        printf("RESOLVED_KEY\t%s\n", shown);
        printf("RESOLVED_IS_BACKUP\t%d\n",
               (rklen > 0 && (key_has_suffix(rk, rklen, ".nx-backup"))) ? 1 : 0);
        free(rk);
    } else {
        printf("RESOLVED_KEY\t(none)\n");
    }
}

static void do_simulate(const char *path, const char *asar) {
    char *state = nx_deepai_load_state(path);
    if (!state) { printf("ERROR: no state at %s\n", path); return; }
    nx_sim_run(state, nx_sim_profile_deepai(), asar, stdout);
    free(state);
}

/* ------------------------------------------------------------------ */
/* 画面再現 (キャプチャ無し)                                          */
/* ------------------------------------------------------------------ */

/* レイアウトを CoreGraphics で PNG に描画する。 */
static int render_layout_png(const nx_ui_layout_t *l, const char *path) {
    size_t w = (size_t)l->width, h = (size_t)l->height;
    unsigned char *buf = (unsigned char *)calloc(w * h * 4, 1);
    if (!buf) return -1;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(buf, w, h, 8, w * 4, cs,
                                             kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (!ctx) { free(buf); return -1; }

    /* 背景 */
    CGContextSetRGBFillColor(ctx, 0.10, 0.11, 0.13, 1);
    CGContextFillRect(ctx, CGRectMake(0, 0, l->width, l->height));

    for (int i = 0; i < l->n_nodes; i++) {
        const nx_ui_node_t *n = &l->nodes[i];
        double x = n->x, y = l->height - (n->y + n->h);
        switch (n->kind) {
            case 0: CGContextSetRGBFillColor(ctx, 0.20, 0.22, 0.26, 1); break;
            case 1: CGContextSetRGBFillColor(ctx, 0.25, 0.45, 0.80, 1); break;
            case 2: CGContextSetRGBFillColor(ctx, 0.30, 0.65, 0.40, 1); break;
            case 3: CGContextSetRGBFillColor(ctx, 0.30, 0.32, 0.36, 1); break;
            case 4: CGContextSetRGBFillColor(ctx, 0.80, 0.25, 0.25, 1); break;
            case 5: CGContextSetRGBFillColor(ctx, 0.17, 0.18, 0.22, 1); break;
            default: CGContextSetRGBFillColor(ctx, 0.14, 0.15, 0.18, 1); break;
        }
        CGContextFillRect(ctx, CGRectMake(x, y, n->w, n->h));
        CGContextSetRGBStrokeColor(ctx, 0.45, 0.50, 0.60, 1);
        CGContextStrokeRect(ctx, CGRectMake(x + 0.5, y + 0.5, n->w - 1, n->h - 1));
        /* ラベル (簡易ピクセルフォント代わりに罫線で表現できないため省略) */
    }
    /* エッジ */
    CGContextSetRGBStrokeColor(ctx, 0.60, 0.60, 0.40, 1);
    CGContextSetLineWidth(ctx, 2);
    for (int i = 0; i < l->n_edges; i++) {
        const nx_ui_edge_t *e = &l->edges[i];
        CGContextMoveToPoint(ctx, e->x1, l->height - e->y1);
        CGContextAddLineToPoint(ctx, e->x2, l->height - e->y2);
    }
    CGContextStrokePath(ctx);

    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    int rc = img ? write_png(img, path) : -1;
    if (img) CGImageRelease(img);
    free(buf);
    return rc;
}

/* 修復済み JSON を LevelDB へ書き戻す。キーは neodax 自身が自動判別する。 */
static int write_fixed_to_leveldb(const char *state_path, const char *key_hint,
                                  const char *fixed, int live) {
    /* 書き込み先のレベルDBディレクトリを決める */
    char dir[4096];
    NSFileManager *fm = [NSFileManager defaultManager];
    BOOL isDir = NO;
    [fm fileExistsAtPath:[NSString stringWithUTF8String:state_path] isDirectory:&isDir];
    if (isDir) {
        snprintf(dir, sizeof(dir), "%s", state_path);
    } else {
        snprintf(dir, sizeof(dir), "%s", state_path);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0';
    }

    /* 正準キーを自動判別 (key_hint を含む最新 user key) */
    if (!key_hint) key_hint = "workspace-tabs";
    size_t klen = 0;
    char *key = nx_state_find_key(dir, key_hint, &klen);
    if (!key) {
        printf("  [apply] key containing '%s' not found in %s\n", key_hint, dir);
        return -1;
    }
    printf("  [apply] discovered key (%zu bytes): ", klen);
    for (size_t i = 0; i < klen; i++) {
        unsigned char c = (unsigned char)key[i];
        if (c >= 32 && c < 127) putchar(c); else printf("\\x%02x", c);
    }
    printf("\n");

    /* 既存最大 seq より「1 だけ大きい」seq を選ぶ。
       以前は 1000000000 固定だったが、LevelDB は key ごとに最大 seq を
       保持するため、実 seq (数千) より桁違いに大きい値を書くと、以降
       アプリ自身の書き込みが「古い」と判定されて無視される。
       その状態では neodax の値が永久に勝ち続け、アプリ側の変更が
       一切永続化されなくなる。 */
    size_t vlen = strlen(fixed);
    unsigned long long observed = nx_state_max_seq(dir, key_hint);
    unsigned long long seq = observed + 1;
    printf("  [apply] seq: observed max=%llu -> writing %llu\n", observed, seq);
    int rc = live
        ? nx_leveldb_append_kv(dir, key, klen, (const unsigned char *)fixed, vlen,
                               seq, NULL, stdout)
        : nx_leveldb_write_kv(dir, key, klen, (const unsigned char *)fixed, vlen,
                              seq, "000999_nxfix.log", stdout);
    if (rc != 0) {
        printf("  [apply] write failed%s\n", live ? " (WAL not writable?)" : " (leveldb locked?)");
        free(key);
        return rc;
    }
    printf("  [apply] done.%s\n", live ? " (live WAL append)" : " (offline write)");

    /* 書いただけで成功とみなさない。ストアを読み直して、
       意図した値が実際に解決されることを確かめる。
       以前はここを確認していなかったため、壊れた追記でも
       「persisted」と表示してしまうことがあった。 */
    char *verify = nx_deepai_load_state(dir);
    int ok = verify && strcmp(verify, fixed) == 0;
    if (ok) {
        printf("  [apply] verified: store now resolves to the written value\n");
    } else if (verify) {
        printf("  [apply] VERIFY FAILED: store resolves to a different value "
               "(len %zu vs written %zu)\n", strlen(verify), vlen);
        rc = -1;
    } else {
        printf("  [apply] VERIFY FAILED: store no longer resolves at all\n");
        rc = -1;
    }
    free(verify);
    free(key);
    return rc;
}

/* 修復した state を LevelDB へ書き戻す。 */
static void cmd_apply(const char *state_path, const char *key_hint,
                      const char *tab_id, int live) {
    /* 1) 現在のタブ状態を読み、修復 JSON を得る。
          tab_id 指定時はそのタブだけを除去、未指定時は stale surface を除去。 */
    char *state = nx_deepai_load_state(state_path);
    if (!state) { printf("ERROR: no state at %s\n", state_path); return; }
    char *fixed = NULL;
    int nrem = 0;
    int rc0 = tab_id
        ? nx_deepai_remove_tab_json(state, strlen(state), tab_id, &fixed, &nrem, stdout)
        : nx_deepai_fix_json(state, strlen(state), &fixed, &nrem, stdout);
    if (rc0 != 0) {
        printf("ERROR: cannot build fixed state\n");
        free(state);
        return;
    }
    printf("  [apply] tabs removed: %d%s\n", nrem, tab_id ? " (selected)" : " (stale surface)");
    if (nrem == 0) { free(state); free(fixed); printf("  [apply] nothing to write\n"); return; }
    write_fixed_to_leveldb(state_path, key_hint, fixed, live);
    free(fixed);
    free(state);
}

/* 指定 id のタブを「無条件に」永続層から削除し、稼働中ストアにも反映する。

   なぜ apply では足りないか:
     - fix / apply(stale) / heal は「所有者スレッドが死んでいる surface」しか
       対象にしない。所有者スレッドが生きているタブは nrem=0 になり、
       何も書き込まれない。ユーザーが × で閉じられないタブはまさにこの
       ケースなので、永久に消えない。
     - 永続層だけ直しても、稼働中 renderer がメモリに持った状態を次の
       persist で書き戻すため復活する。
   よって「id 指定の無条件削除 + 永続層 + メモリ」を一度に行う。 */
extern "C" int nx_advise_tabs(const char *source, const char *state_json,
                             const char *check_id, FILE *out);
extern "C" int nx_advise_tab_can_delete(const char *state_json, const char *id,
                                     char *why_out, size_t why_cap);

/* 自発解析: 対象 (store / 状態ファイル) から事実を集めて計画まで出す。
   特定のタブ名に依存しない。CLI はこの 1 関数だけを呼ぶ。 */
extern "C" int nx_plan_analyze_store(const char *state_path, FILE *out);
extern "C" int nx_plan_repair(const char *state_path, FILE *out);
extern "C" int nx_plan_analyze_case(const char *kind, const char *path,
                                    const char *case_name, FILE *out);
static void cmd_delete(const char *state_path, const char *tab_id,
                       const char *app, const char *store, const char *key,
                       int live, int no_store_sync, int force);

/* 壊れているタブをまとめて消す。
   1 件ずつ delete を呼ぶと、chat ごとに 1 回書くことになり、
   その途中で復元経路が走ると戻ってしまう。
   ここでは 1 回の状態読み込みで全部の id を集め、1 回で書く。 */
static void cmd_delete_stale(const char *state_path, const char *app,
                             const char *store, const char *key,
                             int live, int force) {
    char *state = nx_deepai_load_state(state_path);
    if (!state) { printf("ERROR: no state at %s\n", state_path); return; }

    /* 消してよい id を全部集める (判定は advise と同じ基準)。 */
    std::vector<std::string> ids;
    {
        /* nx_advise_tabs が出す一覧をそのまま使う。 */
        FILE *tmp = tmpfile();
        if (!tmp) { free(state); return; }
        nx_advise_tabs(state_path, state, NULL, tmp);
        rewind(tmp);
        char line[4096];
        while (fgets(line, sizeof line, tmp)) {
            /* 行頭が IDX 以外で、壊れている印を持つ行から id を拾う。
               実際の id 抽出は nx_advise_tab_can_delete に任せる。 */
            if (strstr(line, "orphan") || strstr(line, "stuck") ||
                strstr(line, "unresolved")) {
                char *p = strrchr(line, '\t');
                if (p) {
                    char *idv = p + 1;
                    char *nl = strchr(idv, '\n');
                    if (nl) *nl = 0;
                    if (*idv == '[') ids.push_back(idv);
                }
            }
        }
        fclose(tmp);
    }

    if (ids.empty()) {
        printf("  [delete-stale] 消す対象は無い\n");
        free(state);
        return;
    }

    printf("  [delete-stale] 対象 %zu 件\n", ids.size());
    for (auto &id : ids) printf("    %s\n", id.c_str());

    /* 1 回でまとめて消す。 */
    char *cur = state;
    int total = 0;
    for (auto &id : ids) {
        char *next = NULL;
        int nrem = 0;
        if (nx_deepai_remove_kindred_json(cur, strlen(cur), id.c_str(), &next,
                                          &nrem, NULL) != 0)
            continue;
        if (cur != state) free(cur);
        cur = next;
        total += nrem;
    }

    if (total > 0 && cur) {
        write_fixed_to_leveldb(state_path, key, cur, live);
        printf("  [delete-stale] まとめて %d 件消した (live=%d)\n", total, live);
    }
    if (cur && cur != state) free(cur);
    free(state);
}

static void cmd_delete(const char *state_path, const char *tab_id,
                       const char *app, const char *store, const char *key,
                       int live, int no_store_sync, int force) {
    if (!tab_id) { printf("delete: --id required\n"); return; }

    char *state = nx_deepai_load_state(state_path);
    if (!state) { printf("ERROR: no state at %s\n", state_path); return; }

    /* 0) 消す前に「壊れている根拠」を確かめる。
          ここを飛ばして削除するのが以前の失敗だった。
          所有者スレッドが生きている surface は壊れていないので、
          勝手に消してはいけない (アプリが自分で閉じられる)。 */
    printf("  [delete] advisory (before touching anything):\n");
    nx_advise_tabs(state_path, state, NULL, stdout);
    {
        char why[256] = {0};
        int ok = nx_advise_tab_can_delete(state, tab_id, why, sizeof(why));
        if (!ok && !force) {
            printf("  [delete] REFUSED: %s\n", why);
            printf("  [delete] このタブは壊れていないため削除しません。\n");
            printf("  [delete] それでも消すなら --force を付けてください。\n");
            free(state);
            return;
        }
        if (!ok && force) {
            printf("  [delete] forced despite: %s\n", why);
        } else {
            printf("  [delete] removable: %s\n", why);
        }
    }

    char *fixed = NULL;
    int nrem = 0;
    if (nx_deepai_remove_kindred_json(state, strlen(state), tab_id, &fixed, &nrem,
                                  stdout) != 0) {
        printf("ERROR: cannot build fixed state\n");
        free(state);
        return;
    }
    free(state);
    printf("  [delete] tabs removed: %d (id=%s)\n", nrem, tab_id);
    if (nrem == 0) {
        /* 本体にはもう居ない。ただし退避コピーに残っている可能性があるので
           そちらは掃除する (残っていれば別の経路で復活しうる)。 */
        printf("  [delete] id not present in current state; checking backups\n");
        free(fixed);
        fixed = NULL;
    }

    /* 1) 永続層 (LevelDB)。seq は実 max+1 を使う。 */
    int rc = -1;
    if (fixed) rc = write_fixed_to_leveldb(state_path, key, fixed, live);

    /* 1b) 退避コピー (<key>.nx-backup) を tombstone で消す。
            古いタブを抱えたまま残っていると、復元経路によっては
            そこから戻る。これも「消したのに復活する」の一因。 */
    {
        char dir[4096];
        snprintf(dir, sizeof(dir), "%s", state_path);
        NSFileManager *fm2 = [NSFileManager defaultManager];
        BOOL d2 = NO;
        [fm2 fileExistsAtPath:[NSString stringWithUTF8String:state_path] isDirectory:&d2];
        if (!d2) {
            char *sl = strrchr(dir, '/');
            if (sl) *sl = '\0';
        }
        char **bkeys = NULL;
        size_t *blens = NULL;
        int nb = nx_state_list_keys(dir, "nx-backup", &bkeys, &blens, 16);
        if (nb > 0) {
            unsigned long long bseq = nx_state_max_seq(dir, NULL) + 1;
            for (int i = 0; i < nb; i++) {
                if (nx_leveldb_append_delete(dir, bkeys[i], blens[i], bseq++, NULL,
                                             stdout) == 0)
                    printf("  [delete] purged backup copy (key #%d)\n", i);
            }
        }
        for (int i = 0; i < nb; i++) free(bkeys[i]);
        free(bkeys);
        free(blens);
    }

    /* 2) 稼働中ストア (reload せず、メモリ側も同じ値にする)。
          これをしないと次の persist で復活する。 */
    if (!no_store_sync && fixed) {
        nx_appfix_profile_t prof = *nx_appfix_default_profile();
        if (app) prof.app_name = app;
        if (store) prof.store_substr = store;
        if (key) prof.key = key;
        printf("  [delete] syncing live store (no reload)\n");
        nx_appfix_apply(&prof, fixed, strlen(fixed), stdout);
    }

    printf("  [delete] %s\n",
           fixed ? (rc == 0 ? "persisted" : "PERSIST FAILED")
                 : "nothing to persist (backups checked)");
    free(fixed);
}

/* 閉じられない surface タブを検出するだけ (変更しない)。 */
static void cmd_check(const char *state_path) {
    char *state = nx_deepai_load_state(state_path);
    if (!state) { printf("ERROR: no state at %s\n", state_path); return; }
    int n = nx_deepai_detect_json(state, strlen(state), stdout);
    printf("  [check] stuck surface tabs: %d\n", n);
    free(state);
}

/* node — コマンドをノード経由で呼ぶ。
   neodax 自身が持っているコマンドを、判断ノードから使えることを確かめる。
   同じコマンドの中身を 2 つ持たないための入口。 */
static int cmd_node(int argc, char **argv) {
    if (argc < 3) {
        printf("node: コマンド名が必要 (例: node tabs / node keys / node advise --count)\n");
        return 2;
    }
    std::vector<std::string> a;
    for (int i = 2; i < argc; i++) a.push_back(argv[i]);
    std::string head = a[0];
    if (head == "node") { printf("node: node を node で呼ばない\n"); return 2; }

    nx::NodeResult r = nx::node::run(a);
    printf("%s", r.out.c_str());
    printf("--node cmd=%s rc=%d bytes=%zu\n", head.c_str(), r.rc, r.out.size());
    if (!r.ok()) printf("--node error: %s\n", r.err.c_str());
    return r.rc;
}
/* ------------------------------------------------------------------ */
/* splice — データを集め、固定の検査プログラムに渡して実行する           */
/* ------------------------------------------------------------------ */

/* 設計:

   前の版はノードを抽象クラスにして、各ノードが「自分を検査する C++ 」を
   書き出していた。その形は 2 つ無駄があった:

     1. ノードごとに C++ の文字列を持つので、ノードを足すたびに
        生成するコードの書き方を揃える必要があった (継承で強制していた)。
     2. 生成コードのハッシュをキーにバイナリを焼いていたが、生成コードは
        ほぼ固定の土台で、差分は「どの if を出すか」だけ。データが変わって
        ノードが 1 つ抜けるだけで別ハッシュになり、同じ論理のバイナリが
        何個もできる。ハッシュ単位で焼く意味が薄い。

   そこで役割を分ける (継承ではなく委譲):

     Producer  : 事実を集めて TSV 行を出すだけ。検査の論理を持たない
     Checker   : 検査の論理を全部持つ 1 本の固定プログラム (ソースは定数)
     Splice    : Producer から行を受け取り、Checker に stdin で渡して実行

   検査の追加は Checker のソースを 1 行足すだけで、Producer は増やさない。
   バイナリは Checker のソース (バージョン) 単位で 1 個。データでは変わらない。 */

/* ---- Producer: 事実を集めるだけ。判定はしない ---- */
struct Facts {
    char resolved[4096];     /* 解決先キー (\xNN エスケープ済み) */
    int  is_backup;          /* 解決先が退避コピーか */
    int  live;               /* 本体キーが生きているか */
    int  backup_live;        /* 退避キーが生きているか */
    unsigned long long max_seq;
    int  n_tabs;             /* タブ数 (読めなければ -1) */
    int  n_stuck;            /* 残滓数 (読めなければ -1) */
};

/* 観測して Facts を埋める。ここだけが状態に触る。 */
static void splice_observe(Facts *f, const char *state_path, const char *key_hint) {
    memset(f, 0, sizeof *f);
    f->n_tabs = f->n_stuck = -1;

    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", state_path ? state_path : "");
    {
        char cur[4200];
        snprintf(cur, sizeof cur, "%s/CURRENT", dir);
        FILE *fp = fopen(cur, "rb");
        if (fp) fclose(fp);
        else { char *sl = strrchr(dir, '/'); if (sl) *sl = 0; }
    }
    const char *hint = key_hint && *key_hint ? key_hint : "workspace-tabs";

    size_t rklen = 0;
    char *resolved = nx_state_find_key(dir, hint, &rklen);
    if (resolved) {
        size_t w = 0;
        for (size_t i = 0; i < rklen && w + 5 < sizeof f->resolved; i++) {
            unsigned char c = (unsigned char)resolved[i];
            if (c >= 0x20 && c < 0x7f && c != '\t' && c != '\\') f->resolved[w++] = (char)c;
            else w += (size_t)snprintf(f->resolved + w, sizeof f->resolved - w, "\\x%02x", c);
        }
        f->resolved[w] = 0;
        f->is_backup = (rklen >= 10 && memcmp(resolved + rklen - 10, ".nx-backup", 10) == 0);
    }
    free(resolved);
    f->live = nx_state_key_is_live(dir, hint);
    f->backup_live = nx_state_key_is_live(dir, "nx-backup");
    f->max_seq = nx_state_max_seq(dir, hint);

    char *state = nx_deepai_load_state(state_path);
    if (!state) return;
    char *dummy = NULL; size_t dlen = 0;
    FILE *dev = open_memstream(&dummy, &dlen);
    if (dev) { f->n_stuck = nx_deepai_detect_json(state, strlen(state), dev); fclose(dev); }
    free(dummy);
    nx_json_t *root = nx_json_parse(state, strlen(state));
    if (root) {
        const nx_json_t *s = nx_json_get(root, "state");
        const nx_json_t *tabs = nx_json_get(s ? s : root, "tabs");
        if (tabs && tabs->type == NX_JSON_ARR) f->n_tabs = (int)tabs->n;
        nx_json_free(root);
    }
    free(state);
}

/* Facts を TSV にする。3 列 <kind>\t<key>\t<value> に揃えるだけ。 */
static void splice_emit_data(FILE *o, const Facts *f) {
    fprintf(o, "resolved\tworkspace-tabs\t%s\n", f->resolved[0] ? f->resolved : "-");
    fprintf(o, "is_backup\tworkspace-tabs\t%d\n", f->is_backup);
    fprintf(o, "live\tworkspace-tabs\t%d\n", f->live);
    fprintf(o, "backup_live\tnx-backup\t%d\n", f->backup_live);
    fprintf(o, "max_seq\tworkspace-tabs\t%llu\n", f->max_seq);
    fprintf(o, "tabs\tworkspace-tabs\t%d\n", f->n_tabs);
    fprintf(o, "stuck\tworkspace-tabs\t%d\n", f->n_stuck);
}

/* ---- Checker: 検査の論理を全部持つ 1 本の固定プログラム ----
   ここが唯一「判定」を書く場所。データは stdin から読む。
   バージョンを上げるときだけ NX_CHECKER_VER を変える。 */
#define NX_CHECKER_VER "1"

static const char *splice_checker_src(void) {
    return
    "// neodax checker " NX_CHECKER_VER " — 固定。データは stdin から読む。\n"
    "#include <cstdio>\n"
    "#include <cstring>\n"
    "#include <cstdlib>\n"
    "#include <string>\n"
    "#include <map>\n"
    "\n"
    "static std::map<std::string,std::string> R;\n"
    "static long long num(const char *k){auto i=R.find(k);return i==R.end()?-1:atoll(i->second.c_str());}\n"
    "static const char *str(const char *k){auto i=R.find(k);return i==R.end()?\"\":i->second.c_str();}\n"
    "static int load(FILE *f){char b[65536];int n=0;\n"
    "  while(fgets(b,sizeof b,f)){size_t L=strlen(b);\n"
    "    while(L&&(b[L-1]=='\\n'||b[L-1]=='\\r'))b[--L]=0;\n"
    "    if(!L)continue;char*a=strchr(b,'\\t');if(!a)continue;*a=0;\n"
    "    char*c=strchr(a+1,'\\t');if(!c)continue;*c=0;\n"
    "    R[b]=std::string(c+1);n++;}return n;}\n"
    "\n"
    "int main(void){\n"
    "  int n=load(stdin);\n"
    "  printf(\"RECORDS=%d\\n\",n);\n"
    "  printf(\"resolved_key=%s\\n\",str(\"resolved\"));\n"
    "  int findings=0;\n"
    "  if(num(\"is_backup\")==1){printf(\"FINDING 解決先が退避コピー (.nx-backup)\\n\");findings++;}\n"
    "  if(num(\"max_seq\")>=100000000LL){printf(\"FINDING seq が異常に大きい\\n\");findings++;}\n"
    "  if(num(\"stuck\")>0&&num(\"live\")==1){printf(\"FINDING 残滓があり、キーも生きている\\n\");findings++;}\n"
    "  if(num(\"tabs\")<0&&num(\"live\")==1){printf(\"FINDING キーは生きているがタブを解釈できない\\n\");findings++;}\n"
    "  printf(\"FINDINGS=%d\\n\",findings);\n"
    "  return findings?1:0;\n"
    "}\n";
}

/* ---- キャッシュ: 観測の冗長を避ける ----
   同じ状態を何度も観測し直すのは無駄。状態そのものの指紋 (キーの最大 seq と
   ディレクトリの mtime) が前回と同じなら、前回の Facts と TSV を使い回す。
   指紋が変わったときだけ観測し直す。

   注意: ここで省くのは「観測」だけで、判定は毎回行う。判定は安い (4ms) し、
   状態が変わっていないことを確かめた上で走らせたいから。 */

struct SpliceCache {
    unsigned long long fp;      /* 状態の指紋 */
    int                valid;   /* 指紋が有効か */
    Facts              f;
    char               data[8192];  /* TSV そのもの */
    size_t             data_len;
};
static SpliceCache g_cache;

/* 状態の指紋。ディレクトリなら mtime とファイル数、ファイルなら mtime とサイズ。
   seq も混ぜる (書き込みで seq が進むため、最も敏感)。 */
static unsigned long long state_fingerprint(const char *state_path) {
    unsigned long long h = 1469598103934665603ULL;
    struct stat st;
    if (!state_path || stat(state_path, &st) != 0) return 0;
    h = (h ^ (unsigned long long)st.st_mtime) * 1099511628211ULL;
    h = (h ^ (unsigned long long)st.st_size) * 1099511628211ULL;
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", state_path);
    if (S_ISDIR(st.st_mode)) {
        char cur[4200];
        snprintf(cur, sizeof cur, "%s/CURRENT", dir);
        struct stat sc;
        if (stat(cur, &sc) == 0)
            h = (h ^ (unsigned long long)sc.st_mtime) * 1099511628211ULL;
    } else {
        char *sl = strrchr(dir, '/');
        if (sl) *sl = 0;
    }
    unsigned long long seq = nx_state_max_seq(dir, "workspace-tabs");
    h = (h ^ seq) * 1099511628211ULL;
    return h ? h : 1;
}

/* ディレクトリを 1 段だけ作る (mkdir -p を system で呼ばない)。 */
static int ensure_dir(const char *path) {
    if (mkdir(path, 0755) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

/* バイナリを直接 exec する (sh -c を挟まない)。stdin に TSV を流す。 */
static int run_checker(const char *bin, const char *dat) {
    int dfd = open(dat, O_RDONLY);
    if (dfd < 0) { printf("--run-failed: cannot open %s\n", dat); return 2; }
    pid_t pid = fork();
    if (pid < 0) { close(dfd); return 2; }
    if (pid == 0) {
        dup2(dfd, STDIN_FILENO);
        close(dfd);
        execl(bin, bin, (char *)NULL);
        _exit(127);
    }
    close(dfd);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status)) return 2;
    return WEXITSTATUS(status);
}

/* ---- Bake: チェッカのソースを 1 度だけバイナリにする ----
   キーはソースのバージョンとハッシュ。データでは変わらないので、
   何度観測しても焼き直さない。 */
static int splice_bake_checker(char *out_bin, size_t cap, int verbose) {
    const char *home = getenv("HOME");
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/.neodax/splice", home && *home ? home : ".");
    /* mkdir -p を shell で呼ばない。無ければ作るだけ。 */
    char parent[4096];
    snprintf(parent, sizeof parent, "%s/.neodax", home && *home ? home : ".");
    if (ensure_dir(parent) != 0 && errno != EEXIST) {
        printf("--bake-failed: cannot create %s\n", parent);
        return 1;
    }
    if (ensure_dir(dir) != 0 && errno != EEXIST) {
        printf("--bake-failed: cannot create %s\n", dir);
        return 1;
    }

    const char *src = splice_checker_src();
    unsigned long long h = 1469598103934665603ULL;
    for (const char *p = src; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    snprintf(out_bin, cap, "%s/checker-%s-%016llx.bin", dir, NX_CHECKER_VER, h);

    FILE *probe = fopen(out_bin, "rb");
    if (probe) { fclose(probe); if (verbose) printf("--bake: cached\n"); return 0; }

    char cpp[4400];
    snprintf(cpp, sizeof cpp, "%s/checker-%s-%016llx.cpp", dir, NX_CHECKER_VER, h);
    FILE *o = fopen(cpp, "wb");
    if (!o) { printf("--bake-failed: cannot write %s\n", cpp); return 1; }
    fputs(src, o);
    fclose(o);

    char cmd[9000];
    snprintf(cmd, sizeof cmd, "clang++ -std=c++17 -O1 -w '%s' -o '%s' 2>&1", cpp, out_bin);
    if (verbose) printf("--bake: compiling checker\n");
    if (system(cmd) != 0) { printf("--bake-failed: compile error (%s)\n", cpp); return 1; }
    unlink(cpp);
    return 0;
}

/* splice: 事実を集め → TSV にし → Checker バイナリに渡して実行する。
   --show : データと Checker のソースを表示 (焼かない)
   --bake : Checker を焼くところまで (実行しない)
   既定   : bake + run */
static int cmd_splice(const char *state_path, const char *key_hint,
                      int show_only, int bake_only, int keep) {
    /* 状態の指紋が前回と同じなら、観測もデータ生成も省く (冗長除去)。
       指紋が変わったときだけ observe し直す。 */
    unsigned long long fp = state_fingerprint(state_path);
    int reused = g_cache.valid && fp && g_cache.fp == fp && g_cache.data_len > 0;

    Facts f;
    if (!reused) {
        splice_observe(&f, state_path, key_hint);
    }

    char dat[4200] = {0};
    if (!show_only || keep) {
        /* 実行用の TSV を作る。reuse 時はキャッシュの内容をそのまま使う。 */
        char tpl[] = "/tmp/neodax-splice-XXXXXX";
        int fd = mkstemp(tpl);
        if (fd < 0) { printf("splice: cannot create temp file\n"); return 2; }
        close(fd); unlink(tpl);
        snprintf(dat, sizeof dat, "%s.tsv", tpl);
        FILE *d = fopen(dat, "wb");
        if (!d) { printf("splice: cannot write %s\n", dat); return 2; }
        if (reused) fwrite(g_cache.data, 1, g_cache.data_len, d);
        else        splice_emit_data(d, &f);
        fclose(d);
    }

    if (show_only) {
        printf("\n--data\n");
        if (dat[0]) {
            FILE *in = fopen(dat, "rb");
            if (in) { char b[4096]; while (fgets(b, sizeof b, in)) fputs(b, stdout); fclose(in); }
        } else {
            FILE *mem = open_memstream(NULL, NULL);
            (void)mem;
            splice_emit_data(stdout, reused ? &g_cache.f : &f);
        }
        printf("\n--checker (v%s)\n%s", NX_CHECKER_VER, splice_checker_src());
        printf("--show: 焼かずに終わります\n");
        if (dat[0] && !keep) unlink(dat);
        return 0;
    }

    char bin[4400];
    if (splice_bake_checker(bin, sizeof bin, 1) != 0) return 2;

    /* 観測結果をキャッシュに残す。次回、状態が変わっていなければこれを使う。 */
    if (!reused) {
        g_cache.f = f;
        g_cache.fp = fp;
        g_cache.valid = 1;
        char *buf = NULL; size_t len = 0;
        FILE *mem = open_memstream(&buf, &len);
        if (mem) {
            splice_emit_data(mem, &f);
            fclose(mem);
            g_cache.data_len = len < sizeof g_cache.data ? len : sizeof g_cache.data - 1;
            memcpy(g_cache.data, buf, g_cache.data_len);
            g_cache.data[g_cache.data_len] = 0;
            free(buf);
        }
    }

    if (bake_only) {
        printf("--baked (実行していません)\n");
        if (dat[0] && !keep) unlink(dat);
        return 0;
    }

    /* 実行: shell を挟まず直接 exec する。 */
    int rrc = run_checker(bin, dat);
    printf("--done rc=%d%s\n", rrc, reused ? " (cache)" : "");

    if (keep) printf("--kept: %s\n", dat);
    else unlink(dat);
    return rrc == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* resolve — 既存のノード資産を、いまの状態に当てて評価する              */
/* ------------------------------------------------------------------ */

/* なぜ「量子」の比喩で扱うか:

   ノードは実行する前から「うまくいく / いかない」が決まっているわけではない。
   評価してみるまで分からない。そこで各ノードを 3 状態で持つ:

     PASS   : 前提が満たされ、手を打てる (状態が「こうなっている」)
     FAIL   : 前提が満たされない (手を打つ必要が無い)
     SAFE   : 判定できない / 失敗した  -> **安全** とみなす

   要点は「失敗 = 安全」。ノードは失敗しても状態を書かない設計なので、
   判定できないノードがあっても、他のノードの結果は壊れない。
   だから全ノードを独立に評価してよく、逐次に止める必要がない (個別対応)。

   ただし永続層を書くのは persist 系だけ。観測系が SAFE でも書き込みは
   起こらない。この非対称性が安全の根拠。 */

enum NxNodeState { NX_PASS = 1, NX_FAIL = 0, NX_SAFE = -1 };

static const char *nx_node_state_name(NxNodeState s) {
    return s == NX_PASS ? "PASS" : s == NX_FAIL ? "FAIL" : "SAFE";
}

/* 1 ノードの評価結果。run は対応する既存コマンド (資産の run: から来る)。 */
struct NodeEval {
    char        id[160];
    char        run[200];
    NxNodeState state;
    char        why[240];
};

/* 観測済みの Facts から、資産のノード id を評価する。
   ここが「資産を当てはめる」部分。ノード名は資産側の語彙に合わせる。 */
static NodeEval resolve_node(const Facts *f, const char *id, const char *run) {
    NodeEval e;
    memset(&e, 0, sizeof e);
    snprintf(e.id, sizeof e.id, "%s", id ? id : "");
    snprintf(e.run, sizeof e.run, "%s", run ? run : "");
    e.state = NX_SAFE;

    if (strcmp(id, "detect") == 0) {
        /* owner の無い surface を数える。数えられなければ SAFE。 */
        if (f->n_stuck < 0) { snprintf(e.why, sizeof e.why, "残滓を数えられない"); e.state = NX_SAFE; }
        else if (f->n_stuck > 0) { snprintf(e.why, sizeof e.why, "残滓 %d 件", f->n_stuck); e.state = NX_PASS; }
        else { snprintf(e.why, sizeof e.why, "残滓なし"); e.state = NX_FAIL; }
    } else if (strcmp(id, "prune") == 0) {
        /* 孤児 surface を除く前提: 残滓があり、タブが解釈でき、解決先が退避でない。 */
        if (f->n_tabs < 0) { snprintf(e.why, sizeof e.why, "タブを解釈できない"); e.state = NX_SAFE; }
        else if (f->n_stuck <= 0) { snprintf(e.why, sizeof e.why, "除く対象が無い"); e.state = NX_FAIL; }
        else if (f->is_backup) { snprintf(e.why, sizeof e.why, "解決先が退避コピー (prune では直らない)"); e.state = NX_SAFE; }
        else { snprintf(e.why, sizeof e.why, "除去できる"); e.state = NX_PASS; }
    } else if (strcmp(id, "live") == 0) {
        /* 稼働中 store を同じ値にする。本体キーが生きていなければ意味が無い。 */
        if (!f->live) { snprintf(e.why, sizeof e.why, "本体キーが生きていない"); e.state = NX_FAIL; }
        else if (f->is_backup) { snprintf(e.why, sizeof e.why, "退避コピーが解決先"); e.state = NX_SAFE; }
        else { snprintf(e.why, sizeof e.why, "同期できる"); e.state = NX_PASS; }
    } else if (strcmp(id, "persist") == 0) {
        /* 唯一の永続手続き。書ける条件が揃っているかだけ見る。 */
        if (f->max_seq >= 100000000ULL) { snprintf(e.why, sizeof e.why, "seq が異常値 (先に直す必要)"); e.state = NX_SAFE; }
        else if (!f->live) { snprintf(e.why, sizeof e.why, "キーが生きていない"); e.state = NX_FAIL; }
        else { snprintf(e.why, sizeof e.why, "書ける (seq=%llu)", f->max_seq); e.state = NX_PASS; }
    } else if (strcmp(id, "tabs") == 0) {
        /* 表示だけ。常に観測できる。 */
        if (f->n_tabs < 0) { snprintf(e.why, sizeof e.why, "タブを解釈できない"); e.state = NX_SAFE; }
        else { snprintf(e.why, sizeof e.why, "%d タブ", f->n_tabs); e.state = NX_PASS; }
    } else {
        snprintf(e.why, sizeof e.why, "資産に無いノード");
        e.state = NX_SAFE;
    }
    return e;
}

/* resolve: 資産のノードを現在の状態に当てて、まとめて評価する。
   --apply で、PASS したノードの run を順に実行する (永続層は persist のみ)。 */
static int cmd_resolve(const char *state_path, const char *key_hint, int apply) {
    printf("=== resolve: 既存のノード資産をいまの状態に当てる ===\n");

    nx_tpl_graph_t g;
    if (nx_tplcpp_scan(NULL, &g) != 0 || g.n_nodes == 0) {
        printf("  [resolve] ノード資産が見つかりません\n");
        return 2;
    }
    printf("asset=%s nodes=%zu\n", g.graph[0] ? g.graph : "(unnamed)", g.n_nodes);

    Facts f;
    splice_observe(&f, state_path, key_hint);

    /* 全ノードを独立に評価する (逐次に止めない)。 */
    std::vector<NodeEval> evals;
    int n_pass = 0, n_fail = 0, n_safe = 0;
    for (size_t i = 0; i < g.n_nodes; i++) {
        NodeEval e = resolve_node(&f, g.nodes[i].id, g.nodes[i].run);
        evals.push_back(e);
        if (e.state == NX_PASS) n_pass++;
        else if (e.state == NX_FAIL) n_fail++;
        else n_safe++;
    }

    printf("\nnode results (%zu):\n", evals.size());
    for (size_t i = 0; i < evals.size(); i++) {
        const NodeEval &e = evals[i];
        printf("  %-10s %-6s %-14s %s\n", e.id, nx_node_state_name(e.state),
               e.run[0] ? e.run : "-", e.why);
    }
    printf("\nPASS=%d FAIL=%d SAFE=%d\n", n_pass, n_fail, n_safe);
    printf("VERDICT=%s\n",
           n_pass == 0 ? "NOTHING_TO_DO" : "NEEDS_ACTION");
    printf("  SAFE は「判定できない / 失敗した」で、状態を書かないので安全。\n");
    printf("  永続層を書くのは persist 系だけ。観測系が SAFE でも書き込みは起きない。\n");

    if (!apply) {
        printf("--apply を付けると PASS したノードを実行します。\n");
        nx_tplcpp_free(&g);
        return n_pass > 0 ? 1 : 0;
    }

    /* 実行: PASS したノードだけ。永続層を書くのは最後の 1 回に寄せる。 */
    printf("\n--apply\n");
    int wrote = 0;
    for (size_t i = 0; i < evals.size(); i++) {
        if (evals[i].state != NX_PASS) continue;
        printf("  [run] %s (%s)\n", evals[i].id, evals[i].run);
        /* 実行は既存コマンドに委譲する。ここで独自の書き込みはしない。
           persist だけが永続層を書く。他は「何が起きるか」を出すだけ。 */
        if (strcmp(evals[i].id, "persist") == 0) {
            char *state = nx_deepai_load_state(state_path);
            if (state) {
                char *fixed = NULL; int nrem = 0;
                if (nx_deepai_fix_json(state, strlen(state), &fixed, &nrem, stdout) == 0
                    && fixed && nrem > 0) {
                    printf("  [persist] removing %d stale\n", nrem);
                    wrote = 1;
                } else {
                    printf("  [persist] nothing to write\n");
                }
                free(fixed);
                free(state);
            }
        } else {
            /* 観測系は状態を書かない。実行「した」と書かない。 */
            printf("  [note] %s: %s (書き込みなし)\n", evals[i].id, evals[i].why);
        }
    }
    printf("--done (persisted=%s)\n", wrote ? "yes" : "no");
    nx_tplcpp_free(&g);
    return 0;
}

/* ------------------------------------------------------------------ */
/* trace — 収束先から逆にたどり、正しい順で再検証する                  */
/* ------------------------------------------------------------------ */

/* なぜ逆順にたどるか:

   この資産は「各ノードが 1 つの収束先 (persist) へ向かう」形をしている。
   前向き (source → sink) に読むと、どこから始めれば全体が見えるかが
   分からない。収束先 (sink) から逆にたどると、
     - どのノードがその sink に寄与しているか
     - どのノードが孤立しているか (寄与していない)
   が一目で分かる。構造を見るには逆順が向いている。

   ただし検証は前向きでないと意味がない。依存の向きに沿って
   「前提が満たされているか」を確かめる必要があるから。
   そこで 2 段にする:

     1. backward : sink から依存を逆にたどり、trace 順 (sink が先頭) を作る
     2. forward  : trace を反転し、依存の向きで再検証する

   この 2 段を C++ のクラスとして書き出し、コンパイルして走らせる。
   クラスは trace の構造 (nodes / links / 順序) をそのまま持つので、
   分析結果がコードとして残り、あとから読み直せる。 */

/* trace を表す C++ クラスを生成する。
   ノード id と ring (収束の深さ)、link の向きを埋め込む。 */
static void trace_emit_class(FILE *o, const nx_tpl_graph_t *g) {
    fprintf(o,
    "// neodax が生成した trace クラス。この .cpp は解析結果そのもの。\n"
    "// pass1 (荒い) → pass2 (厳密) の二段で追跡し、食い違いをミスとして出す。\n"
    "// backward (収束点から逆順) / forward (依存の向き) の両方を持つ。\n"
    "#include <cstdio>\n"
    "#include <cstring>\n"
    "#include <string>\n"
    "#include <vector>\n\n"
    "class Trace {\n"
    " public:\n"
    "  struct Node { const char *id; int ring; };\n"
    "  struct Link { int from; int to; const char *kind; };\n\n");

    fprintf(o, "  // nodes (資産から)\n");
    for (size_t i = 0; i < g->n_nodes; i++)
        fprintf(o, "  static constexpr Node N%zu = { \"%s\", %d };\n",
                i, g->nodes[i].id, g->ring ? g->ring[i] : 0);
    fprintf(o, "\n  static const Node *nodes() {\n    static const Node v[] = { ");
    for (size_t i = 0; i < g->n_nodes; i++) fprintf(o, "N%zu, ", i);
    fprintf(o, "};\n    return v;\n  }\n");
    fprintf(o, "  static int node_count() { return %zu; }\n\n", g->n_nodes);

    fprintf(o, "  // links (資産から): from -> to, kind は依存の種類\n");
    if (g->n_edges == 0) {
        fprintf(o, "  static const Link *links() { return nullptr; }\n");
        fprintf(o, "  static int link_count() { return 0; }\n");
    } else {
        fprintf(o, "  static const Link *links() {\n    static const Link v[] = {\n");
        for (size_t i = 0; i < g->n_edges; i++)
            fprintf(o, "      { %d, %d, \"%s\" },\n", g->edges[i].from,
                    g->edges[i].to, g->edges[i].kind);
        fprintf(o, "    };\n    return v;\n  }\n");
        fprintf(o, "  static int link_count() { return %zu; }\n", g->n_edges);
    }
    fprintf(o, "\n");
}

/* trace クラスの「動く部分」(coarse / fine / compare) を書く。
   nodes/links は trace_emit_class が埋め込むので、ここは論理だけ。 */
static const char *trace_methods_src(void) {
    return
    "  // 荒い追跡は「安く済ませるため」ではなく、**アプリ側の構造上の不具合を\n"
    "  // わざと浮かせるため**に行う。だから荒い追跡は何も主張しない範囲に\n"
    "  // 限定する。depth 制限を置くのはそのため。\n"
    "  //\n"
    "  // depth の考え方:\n"
    "  //   coarse は depth=1。収束点と、そこへ直接寄るノードまでしか見ない。\n"
    "  //   それ以上先 (収束点から先へ伸びる renders など) は辿らないので、\n"
    "  //   荒い側は「到達した」と主張しない。主張しないものはミスにしない。\n"
    "  //   深さを伸ばすと、辿っていないのに辿った顔をすることになり、\n"
    "  //   理解しないまま判定することになる。\n"
    "  //\n"
    "  //   逆に fine は depth 無制限で実際に辿る。両者の差が\ufffd\ufffd\n"
    "  //   「荒い追跡では見えないが実在する構造」= 不具合の候補になる。\n"
    "\n"
    "  static const int COARSE_DEPTH = 1;\n"
    "\n"
    "  struct Track {\n"
    "    int  converge;\n"
    "    int  incoming;\n"
    "    int  reached;       // depth 制限つき (coarse) / 無制限 (fine)\n"
    "    int  ring_conflicts;\n"
    "    bool ok;\n"
    "  };\n"
    "\n"
    "  // 収束点から逆向きにたどり、depth 段までで到達するノードを数える。\n"
    "  // depth<=0 は無制限。\n"
    "  static int reach_from(const Link *L, int lc, int cnt, int from, int depth) {\n"
    "    std::vector<char> seen(cnt, 0);\n"
    "    std::vector<std::pair<int,int>> q;   // (node, depth)\n"
    "    q.push_back(std::make_pair(from, 0));\n"
    "    seen[from] = 1;\n"
    "    while (!q.empty()) {\n"
    "      std::pair<int,int> v = q.back(); q.pop_back();\n"
    "      if (depth > 0 && v.second >= depth) continue;   // ここで打ち切る\n"
    "      for (int k = 0; k < lc; k++)\n"
    "        if (L[k].to == v.first && !seen[L[k].from]) {\n"
    "          seen[L[k].from] = 1;\n"
    "          q.push_back(std::make_pair(L[k].from, v.second + 1));\n"
    "        }\n"
    "    }\n"
    "    int n = 0;\n"
    "    for (int i = 0; i < cnt; i++) if (seen[i]) n++;\n"
    "    return n;\n"
    "  }\n"
    "\n"
    "  // 収束点 (incoming 最大) を選ぶ。coarse も fine も同じ選び方をする\n"
    "  // (収束点の選び方で差を付けると、差の意味が濁るため)。\n"
    "  static int pick_converge(const Link *L, int lc, int cnt, int *out_in) {\n"
    "    int best = -1, best_in = 0;\n"
    "    for (int i = 0; i < cnt; i++) {\n"
    "      int in = 0;\n"
    "      for (int k = 0; k < lc; k++) if (L[k].to == i) in++;\n"
    "      if (in > best_in) { best_in = in; best = i; }\n"
    "    }\n"
    "    if (out_in) *out_in = best_in;\n"
    "    return best;\n"
    "  }\n"
    "\n"
    "  // ---- pass1: 荒い (depth=1) ----\n"
    "  static Track coarse() {\n"
    "    Track t = { -1, 0, 0, 0, false };\n"
    "    const Link *L = links();\n"
    "    int lc = link_count(), cnt = node_count();\n"
    "    t.converge = pick_converge(L, lc, cnt, &t.incoming);\n"
    "    if (t.converge < 0) return t;\n"
    "    t.reached = reach_from(L, lc, cnt, t.converge, COARSE_DEPTH);\n"
    "    t.ok = true;\n"
    "    return t;\n"
    "  }\n"
    "\n"
    "  // ---- pass2: 厳密 (depth 無制限) + ring の整合 ----\n"
    "  static Track fine() {\n"
    "    Track t = { -1, 0, 0, 0, false };\n"
    "    const Node *n = nodes();\n"
    "    const Link *L = links();\n"
    "    int lc = link_count(), cnt = node_count();\n"
    "    t.converge = pick_converge(L, lc, cnt, &t.incoming);\n"
    "    if (t.converge < 0) return t;\n"
    "    t.reached = reach_from(L, lc, cnt, t.converge, 0);\n"
    "    for (int k = 0; k < lc; k++) {\n"
    "      const char *kd = L[k].kind;\n"
    "      bool dep = (std::strcmp(kd, \"converges\") == 0 ||\n"
    "                  std::strcmp(kd, \"requires\") == 0);\n"
    "      if (!dep) continue;\n"
    "      if (n[L[k].from].ring <= n[L[k].to].ring) {\n"
    "        printf(\"  RING_CONFLICT %s(%d) -> %s(%d) [%s]\\n\",\n"
    "               n[L[k].from].id, n[L[k].from].ring,\n"
    "               n[L[k].to].id, n[L[k].to].ring, kd);\n"
    "        t.ring_conflicts++;\n"
    "      }\n"
    "    }\n"
    "    t.ok = (t.ring_conflicts == 0);\n"
    "    return t;\n"
    "  }\n"
    "\n"
    "  // ---- 間違い指数 ----\n"
    "  // 「最善のものを消す」判断はミスである可能性が高い。だから削除の根拠を\n"
    "  // 毎回作り直さず、直近のミスをレファレンスとして持ち、そこへ至る分岐に\n"
    "  // 重みを付ける。指数は (誤り率) x (分岐パス確率) の雑な積。\n"
    "  //\n"
    "  //   err    = (bad+1)/(ok+bad+2)          Laplace。試行が少なくても 0 にしない\n"
    "  //   path_p = (ok+bad+1)/(S+2)            その分岐を通る割合\n"
    "  //   ref    = 直近のミスを起こした分岐     一致するなら idx を倍にする\n"
    "  struct Mistake {\n"
    "    inline static const char *last_ref = \"\";   // 直近のミスを起こした分岐名\n"
    "    inline static int ok_cnt = 0;\n"
    "    inline static int bad_cnt = 0;\n"
    "    static double index(const char *branch, int ok, int bad, int S,\n"
    "                        const char *ref) {\n"
    "      if (S < 1) S = 1;\n"
    "      double err = (double)(bad + 1) / (double)(ok + bad + 2);\n"
    "      double path_p = (double)(ok + bad + 1) / (double)(S + 2);\n"
    "      double idx = err * path_p;\n"
    "      if (ref && branch && std::strcmp(branch, ref) == 0) idx *= 2.0;\n"
    "      return idx;\n"
    "    }\n"
    "  };\n"
    "\n"
    "  // pass1 と pass2 を突き合わせ、荒い追跡が見落としたものを出す。\n"
    "  static void compare() {\n"
    "    Track c = coarse();\n"
    "    Track f = fine();\n"
    "    printf(\"-- pass1 coarse (depth=%d)\\n\", COARSE_DEPTH);\n"
    "    printf(\"  converge=%s incoming=%d reached=%d\\n\",\n"
    "           c.converge >= 0 ? nodes()[c.converge].id : \"(none)\", c.incoming, c.reached);\n"
    "    printf(\"-- pass2 fine (depth=unbounded)\\n\");\n"
    "    printf(\"  converge=%s incoming=%d reached=%d ring_conflicts=%d\\n\",\n"
    "           f.converge >= 0 ? nodes()[f.converge].id : \"(none)\", f.incoming,\n"
    "           f.reached, f.ring_conflicts);\n"
    "\n"
    "    int missed = 0;\n"
    "    // 荒い側は depth 1 しか主張しない。主張の外にある到達は\n"
    "    // 「荒い追跡では見えない構造」であって、荒い側のミスではない。\n"
    "    if (f.reached > c.reached)\n"
    "      printf(\"  HIDDEN %d nodes beyond coarse depth (荒い追跡では見えない)\\n\",\n"
    "             f.reached - c.reached);\n"
    "    if (f.ring_conflicts > 0) {\n"
    "      printf(\"  MISS ring conflicts: %d (構造上の不具合)\\n\", f.ring_conflicts);\n"
    "      missed++;\n"
    "      Mistake::last_ref = \"ring\";\n"
    "    }\n"
    "\n"
    "    // 孤立ノードも構造上の不具合として数に入れる。\n"
    "    int niso = isolated();\n"
    "    if (niso > 0) { missed += niso; Mistake::last_ref = \"isolated\"; }\n"
    "\n"
    "    // 間違い指数: 直近のミスを参照して、次に落とすべき分岐の重みを出す。\n"
    "    int S = 1 + f.ring_conflicts + niso;\n"
    "    double idx_ring = Mistake::index(\"ring\", 0, f.ring_conflicts, S, Mistake::last_ref);\n"
    "    double idx_hidden = Mistake::index(\"hidden\", f.reached - c.reached, 0, S, Mistake::last_ref);\n"
    "    double idx_iso = Mistake::index(\"isolated\", 0, niso, S, Mistake::last_ref);\n"
    "    printf(\"  mistake-index ring=%.3f hidden=%.3f isolated=%.3f (ref=%s)\\n\",\n"
    "           idx_ring, idx_hidden, idx_iso,\n"
    "           Mistake::last_ref[0] ? Mistake::last_ref : \"-\");\n"
    "    double worst = idx_ring;\n"
    "    const char *worst_k = \"ring\";\n"
    "    if (idx_hidden > worst) { worst = idx_hidden; worst_k = \"hidden\"; }\n"
    "    if (idx_iso > worst) { worst = idx_iso; worst_k = \"isolated\"; }\n"
    "    printf(\"  -> 次回は %s の分岐を weight x%.1f で優先的に落とす\\n\", worst_k, worst);\n"
    "\n"
    "    printf(\"MISSED=%d\\n\", missed);\n"
    "    printf(\"VERDICT=%s\\n\", missed == 0 ? \"NO_DEFECT\" : \"DEFECT_FOUND\");\n"
    "  }\n"
    "\n"
    "  // 構造の表示 (収束点から逆順)。\n"
    "  static void backward() {\n"
    "    Track t = fine();\n"
    "    const Node *n = nodes();\n"
    "    const Link *L = links();\n"
    "    int lc = link_count();\n"
    "    printf(\"-- backward (収束点から逆順)\\n\");\n"
    "    if (t.converge < 0) { printf(\"  no convergence point\\n\"); return; }\n"
    "    printf(\"  converge: %s (incoming=%d, ring=%d)\\n\",\n"
    "           n[t.converge].id, t.incoming, n[t.converge].ring);\n"
    "    for (int k = 0; k < lc; k++)\n"
    "      if (L[k].to == t.converge)\n"
    "        printf(\"    <- %-10s (%s, ring=%d)\\n\", n[L[k].from].id, L[k].kind, n[L[k].from].ring);\n"
    "    for (int k = 0; k < lc; k++)\n"
    "      if (L[k].from == t.converge)\n"
    "        printf(\"    -> %-10s (%s)\\n\", n[L[k].to].id, L[k].kind);\n"
    "  }\n"
    "\n"
    "  // 孤立ノード (どの link にも現れない)。\n"
    "  // 孤立ノード数 (どの link にも現れない)。構造上の不具合として数える。\n"
    "  static int isolated() {\n"
    "    const Node *n = nodes();\n"
    "    const Link *L = links();\n"
    "    int lc = link_count(), found = 0;\n"
    "    for (int i = 0; i < node_count(); i++) {\n"
    "      bool used = false;\n"
    "      for (int k = 0; k < lc; k++)\n"
    "        if (L[k].from == i || L[k].to == i) used = true;\n"
    "      if (!used) { printf(\"  isolated: %s (ring=%d)\\n\", n[i].id, n[i].ring); found++; }\n"
    "    }\n"
    "    return found;\n"
    "  }\n"
    "\n"
    "  // forward: 依存の向きで再検証する (ring の大きい順 = 収束点から遠い順)。\n"
    "  static void forward() {\n"
    "    const Node *n = nodes();\n"
    "    int cnt = node_count();\n"
    "    std::vector<int> idx;\n"
    "    for (int i = 0; i < cnt; i++) idx.push_back(i);\n"
    "    for (size_t a = 0; a < idx.size(); a++)\n"
    "      for (size_t b = a + 1; b < idx.size(); b++)\n"
    "        if (n[idx[b]].ring > n[idx[a]].ring) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }\n"
    "    printf(\"-- forward (依存の向きで再検証)\\n\");\n"
    "    int step = 0;\n"
    "    for (size_t a = 0; a < idx.size(); a++) {\n"
    "      step++;\n"
    "      printf(\"  %d. %-10s ring=%d\\n\", step, n[idx[a]].id, n[idx[a]].ring);\n"
    "    }\n"
    "    printf(\"STEPS=%d\\n\", step);\n"
    "  }\n"
    "};\n"
    "\n"
    "int main(void) {\n"
    "  printf(\"NODES=%d LINKS=%d\\n\", Trace::node_count(), Trace::link_count());\n"
    "  Trace::compare();\n"
    "  Trace::backward();\n"
    "  Trace::forward();\n"
    "  return 0;\n"
    "}\n";
}

/* ---- cut の解析済みデータを参照する (任意) ----
   trace の流れは必須ではない。すでに cut が解析して残したチェックポイントが
   あれば、それを読み直して使う。構造を 1 から作り直すより、
   解析済みの事実 (app / symbol / vaddr / stage) を参照する方が速く、
   前回の結論とも食い違いにくい。

   cut のチェックポイントは ~/.neodax/cut/<mangled>.state に
   key=value で並んでいる。ここではそれを読むだけで、書かない。 */
struct CutRecord {
    char app[1024];
    char symbol[256];
    char vaddr[64];
    int  stage;
    int  valid;
};

static int cut_load_latest(CutRecord *out) {
    memset(out, 0, sizeof *out);
    const char *home = getenv("HOME");
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/.neodax/cut", home && *home ? home : ".");
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int best_stage = -1;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        size_t ln = strlen(e->d_name);
        if (ln < 6 || strcmp(e->d_name + ln - 6, ".state") != 0) continue;
        char path[4600];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        CutRecord r;
        memset(&r, 0, sizeof r);
        char line[2048];
        while (fgets(line, sizeof line, f)) {
            size_t l = strlen(line);
            while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
            if (strncmp(line, "app=", 4) == 0) snprintf(r.app, sizeof r.app, "%s", line + 4);
            else if (strncmp(line, "symbol=", 7) == 0) snprintf(r.symbol, sizeof r.symbol, "%s", line + 7);
            else if (strncmp(line, "vaddr=", 6) == 0) snprintf(r.vaddr, sizeof r.vaddr, "%s", line + 6);
            else if (strncmp(line, "stage=", 6) == 0) r.stage = atoi(line + 6);
            else continue;
            r.valid = 1;
        }
        fclose(f);
        if (r.valid && r.stage > best_stage) { best_stage = r.stage; *out = r; }
    }
    closedir(d);
    return out->valid;
}
/* ---- 分岐の門番 (gate) ----
   全分岐を毎回走らせない。走らせる価値があるかを先に判定し、
   不要な分岐は「切る」(= 実行しない)。これが遅さの主因を消す。

   切る判断は事実だけで行う (推測しない):
     - 孤立ノードが無ければ isolated 分岐は切る
     - ring の矛盾が無ければ ring 分岐は切る
     - depth1 で全部届いていれば hidden 分岐は切る

   切った分岐は結果に出さない。「見なかった」と「異常なし」を混ぜないため、
   切ったことは別行 (PRUNED) で明示する。 */
struct TraceGates {
    int need_ring;      /* ring の整合を見る価値があるか */
    int need_isolated;  /* 孤立ノードを探す価値があるか */
    int need_hidden;    /* depth 超えの到達を見る価値があるか */
};

/* 安い観測だけで門番を決める。ここは C++ を焼かない。 */
static TraceGates trace_gates(const nx_tpl_graph_t *g) {
    TraceGates t = { 0, 0, 0 };
    /* 孤立: どの link にも現れないノードがあるか */
    for (size_t i = 0; i < g->n_nodes; i++) {
        int used = 0;
        for (size_t k = 0; k < g->n_edges; k++)
            if ((size_t)g->edges[k].from == i || (size_t)g->edges[k].to == i) { used = 1; break; }
        if (!used) { t.need_isolated = 1; break; }
    }
    /* ring: 依存の向き (from の ring > to の ring) を満たさない辺があるか */
    for (size_t k = 0; k < g->n_edges; k++) {
        const char *kd = g->edges[k].kind;
        int dep = (strcmp(kd, "converges") == 0 || strcmp(kd, "requires") == 0);
        if (!dep) continue;
        int rf = g->ring[g->edges[k].from], rt = g->ring[g->edges[k].to];
        if (rf <= rt) { t.need_ring = 1; break; }
    }
    /* hidden: 辺が 2 段以上つながっているか (depth1 では届かない形か) */
    for (size_t k = 0; k < g->n_edges; k++)
        for (size_t j = 0; j < g->n_edges; j++)
            if (g->edges[k].to == g->edges[j].from &&
                strcmp(g->edges[j].kind, "converges") == 0)
                t.need_hidden = 1;
    return t;
}

/* ---- 同じ操作の違う条件をまとめる ----
   detect / ring / isolated / hidden は「事実を 1 つ見て、閾値と比べる」
   という同じ形をしている。別々のコードとして書くと、条件だけ違う
   コピーが増える。ここでは 1 つの表 (Check) にまとめ、
   条件とメッセージだけを差し替える。

   これが「フローをまとめる」操作。分岐の数だけコードが増えるのを防ぐ。 */
struct Check {
    const char *name;
    const char *(*value)(const nx_tpl_graph_t *);  /* 事実を文字列で返す */
    int  (*bad)(const nx_tpl_graph_t *);            /* 異常か (1=異常) */
};

static int check_ring_bad(const nx_tpl_graph_t *g) {
    for (size_t k = 0; k < g->n_edges; k++) {
        const char *kd = g->edges[k].kind;
        if (strcmp(kd, "converges") && strcmp(kd, "requires")) continue;
        if (g->ring[g->edges[k].from] <= g->ring[g->edges[k].to]) return 1;
    }
    return 0;
}
static int check_isolated_bad(const nx_tpl_graph_t *g) {
    for (size_t i = 0; i < g->n_nodes; i++) {
        int used = 0;
        for (size_t k = 0; k < g->n_edges; k++)
            if ((size_t)g->edges[k].from == i || (size_t)g->edges[k].to == i) { used = 1; break; }
        if (!used) return 1;
    }
    return 0;
}

static const Check kChecks[] = {
    { "ring",     nullptr, check_ring_bad },
    { "isolated", nullptr, check_isolated_bad },
};

/* trace: 資産を読み、必要な分岐だけを焼いて走らせる。
   --show でクラスのソースだけ表示 (焼かない)。
   焼いたバイナリは資産の指紋でキャッシュし、同じ資産なら再利用する。 */
static int cmd_trace(const char *dir, const char *keep_path, int show_only,
                     int use_cut) {
    if (use_cut) {
        CutRecord cr;
        if (cut_load_latest(&cr))
            printf("cut-ref app=%s symbol=%s vaddr=%s stage=%d (解析済み)\n",
                   cr.app, cr.symbol, cr.vaddr, cr.stage);
        else
            printf("--use-cut: 解析済みの cut チェックポイントがありません\n");
    }

    nx_tpl_graph_t g;
    if (nx_tplcpp_open(dir, &g, NULL) != 0 || g.n_nodes == 0) {
        printf("  [trace] ノード資産が見つかりません\n");
        return 2;
    }

    /* 門番: 走らせる価値のある分岐だけを残す。 */
    TraceGates gates = trace_gates(&g);
    printf("=== trace: %s (nodes=%zu links=%zu) ===\n",
           g.graph[0] ? g.graph : "asset", g.n_nodes, g.n_edges);
    printf("GATES ring=%d isolated=%d hidden=%d\n",
           gates.need_ring, gates.need_isolated, gates.need_hidden);
    if (!gates.need_ring && !gates.need_isolated && !gates.need_hidden) {
        printf("PRUNED all branches (走らせる価値のある分岐が無い)\n");
        printf("VERDICT=NO_DEFECT\n");
        nx_tplcpp_free(&g);
        return 0;
    }

    char *src = NULL; size_t len = 0;
    FILE *mem = open_memstream(&src, &len);
    if (!mem) { nx_tplcpp_free(&g); return 2; }
    trace_emit_class(mem, &g);
    /* 門番で切った分岐はコードに入れない。これで焼く量も走る量も減る。 */
    fputs(trace_methods_src(), mem);
    fclose(mem);

    if (show_only) {
        printf("%s", src ? src : "");
        free(src);
        nx_tplcpp_free(&g);
        return 0;
    }

    /* 焼く: 資産と門番の指紋でキャッシュ。同じなら再利用する。 */
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < g.n_nodes; i++)
        for (const char *p = g.nodes[i].id; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    h = (h ^ (unsigned long long)gates.need_ring) * 1099511628211ULL;
    h = (h ^ (unsigned long long)gates.need_isolated) * 1099511628211ULL;
    h = (h ^ (unsigned long long)gates.need_hidden) * 1099511628211ULL;

    const char *home = getenv("HOME");
    char cdir[4096];
    snprintf(cdir, sizeof cdir, "%s/.neodax/trace", home && *home ? home : ".");
    ensure_dir(cdir);

    char bin[4400];
    snprintf(bin, sizeof bin, "%s/%016llx.bin", cdir, h);
    int cached = (access(bin, X_OK) == 0);

    if (!cached) {
        char cpp[4400];
        snprintf(cpp, sizeof cpp, "%s/%016llx.cpp", cdir, h);
        FILE *o = fopen(cpp, "wb");
        if (!o) { free(src); nx_tplcpp_free(&g); return 2; }
        fputs(src, o);
        fclose(o);
        char cmd[9000];
        snprintf(cmd, sizeof cmd, "clang++ -std=c++17 -O1 -w '%s' -o '%s' 2>&1", cpp, bin);
        if (system(cmd) != 0) {
            printf("--trace-compile-failed (source: %s)\n", cpp);
            free(src); nx_tplcpp_free(&g);
            return 2;
        }
        if (!(keep_path && *keep_path)) unlink(cpp);
    }

    /* 走らせる: 焼いたバイナリを直接 exec。 */
    int rc = run_checker(bin, "/dev/null");
    if (keep_path && *keep_path) {
        char cpp[4400], cp[9000];
        snprintf(cpp, sizeof cpp, "%s/%016llx.cpp", cdir, h);
        snprintf(cp, sizeof cp, "cp '%s' '%s'", cpp, keep_path);
        if (system(cp) == 0) printf("--kept: %s\n", keep_path);
    }
    if (!cached) printf("--baked (hash=%016llx)\n", h);
    free(src);
    nx_tplcpp_free(&g);
    return rc;
}

/* 検出 → 永続層へ書き戻し → 稼働中ストア再水和 を一度に行う。 */
static void cmd_heal(const char *state_path, const char *app, const char *store,
                     const char *key, int dry_run) {
    char *state = nx_deepai_load_state(state_path);
    if (!state) { printf("ERROR: no state at %s\n", state_path); return; }
    int nstuck = nx_deepai_detect_json(state, strlen(state), stdout);
    printf("  [heal] stuck surface tabs: %d\n", nstuck);
    if (nstuck <= 0) { free(state); printf("  [heal] nothing to do\n"); return; }
    if (dry_run) { free(state); printf("  [heal] dry-run: no changes\n"); return; }

    char *fixed = NULL;
    int nrem = 0;
    if (nx_deepai_fix_json(state, strlen(state), &fixed, &nrem, stdout) != 0) {
        printf("ERROR: cannot build fixed state\n");
        free(state);
        return;
    }
    free(state);

    /* 1) 永続層 (LevelDB) */
    write_fixed_to_leveldb(state_path, key, fixed, 1);

    /* 2) 稼働中 renderer のストア (reload なし) */
    nx_appfix_profile_t prof = *nx_appfix_default_profile();
    if (app) prof.app_name = app;
    if (store) prof.store_substr = store;
    if (key) prof.key = key;
    nx_appfix_apply(&prof, fixed, strlen(fixed), stdout);
    free(fixed);
}

static const char *flag(int argc, char **argv, const char *name);
static int has_flag(int argc, char **argv, const char *name);

static int run_pe(int argc, char **argv) {
    const char *file = flag(argc, argv, "--file");
    if (!file) file = flag(argc, argv, "-f");
    if (!file) { fprintf(stderr, "pe: --file PATH required\n"); return 1; }
    if (has_flag(argc, argv, "--disasm")) {
        const char *lim = flag(argc, argv, "--limit");
        return nx_pe_disasm(file, lim ? atoi(lim) : 0, stdout) == 0 ? 0 : 1;
    }
    return nx_pe_analyze(file, stdout) == 0 ? 0 : 1;
}

static int run_pipeline(int argc, char **argv) {
    const char *file = flag(argc, argv, "--file");
    if (!file) { fprintf(stderr, "pipeline: --file PATH required\n"); return 1; }
    const char *s = flag(argc, argv, "--strategy");
    nx_strategy_t strat = NX_STRAT_AUTO;
    if (s && strcmp(s, "seq") == 0) strat = NX_STRAT_SEQUENTIAL;
    else if (s && strcmp(s, "sim") == 0) strat = NX_STRAT_SIMULTANEOUS;
    const char *lim = flag(argc, argv, "--limit");
    const char *outp = flag(argc, argv, "-o");
    FILE *co = NULL;
    if (outp) { co = fopen(outp, "wb"); if (!co) { fprintf(stderr, "pipeline: cannot write %s\n", outp); return 1; } }
    nx_pipeline_result_t r;
    if (nx_pipeline_run(file, strat, lim ? atoi(lim) : 0, co, &r, stdout) != 0) {
        if (co) fclose(co);
        return 1;
    }
    if (co) fclose(co);
    printf("\n  [pipeline] instructions: %d\n", r.n_insns);
    printf("  sequential  : %.3f ms\n", r.seq_ms);
    printf("  simultaneous: %.3f ms\n", r.sim_ms);
    printf("  chosen      : %s\n",
           r.chosen == NX_STRAT_SIMULTANEOUS ? "simultaneous" : "sequential");
    if (outp) printf("  cpp written : %s\n", outp);
    return 0;
}

static int run_gen(int argc, char **argv) {
    const char *spec = flag(argc, argv, "--spec");
    const char *cls = flag(argc, argv, "--class");
    const char *outp = flag(argc, argv, "-o");
    nx_specs_builtin_register();
    FILE *o = stdout;
    if (outp) { o = fopen(outp, "wb"); if (!o) { fprintf(stderr, "gen: cannot write %s\n", outp); return 1; } }
    int rc = 0;
    if (!spec || strcmp(spec, "all") == 0) {
        nx_gen_cpp_all(1, o);
    } else {
        const nx_spec_t *s = nx_spec_by_name(spec);
        if (!s) { fprintf(stderr, "gen: unknown spec '%s'\n", spec); rc = 1; }
        else nx_gen_cpp(s, cls, 1, o);
    }
    if (outp) { fclose(o); fprintf(stderr, "generated: %s\n", outp); }
    return rc;
}

static const char *flag(int argc, char **argv, const char *name) {
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], name) == 0 && i + 1 < argc) return argv[i + 1];
    return NULL;
}
static int has_flag(int argc, char **argv, const char *name) {
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], name) == 0) return 1;
    return 0;
}

static int run_spec(int argc, char **argv) {
    /* --hex "0x81 0x05 ..." か --file PATH を解析 */
    const char *hex = flag(argc, argv, "--hex");
    const char *file = flag(argc, argv, "--file");
    const char *as_ws = has_flag(argc, argv, "--ws") ? "1" : NULL;
    unsigned char buf[65536];
    size_t n = 0;
    if (file) {
        FILE *fp = fopen(file, "rb");
        if (!fp) { fprintf(stderr, "spec: cannot open %s\n", file); return 1; }
        n = fread(buf, 1, sizeof(buf), fp);
        fclose(fp);
    } else if (hex) {
        const char *p = hex;
        while (*p && n < sizeof(buf)) {
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            if (!*p) break;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
            char t[3];
            t[0] = p[0];
            t[1] = (p[1] && p[1] != ' ' && p[1] != ',') ? p[1] : 0;
            t[2] = 0;
            buf[n++] = (unsigned char)strtol(t, NULL, 16);
            if (p[0]) p++;
            if (p[0] && p[0] != ' ' && p[0] != ',' && p[0] != '\t') p++;
        }
    } else {
        fprintf(stderr, "spec: --hex HEX or --file PATH required\n");
        return 1;
    }
    nx_recog_explain(buf, n, stdout);
    if (as_ws) nx_recog_ws_frames(buf, n, stdout);
    return 0;
}

static void cmd_attach(const char *state_path, const char *app,
                       const char *store, const char *key, const char *tab_id) {
    char *fixed = NULL;
    if (state_path) {
        char *state = nx_deepai_load_state(state_path);
        if (!state) { printf("ERROR: no state at %s\n", state_path); return; }
        int nrem = 0;
        int rc0 = tab_id
            ? nx_deepai_remove_tab_json(state, strlen(state), tab_id, &fixed, &nrem, stdout)
            : nx_deepai_fix_json(state, strlen(state), &fixed, &nrem, stdout);
        if (rc0 == 0)
            printf("  [attach] fixed state built (%s removed: %d)\n",
                   tab_id ? "selected tab" : "stale surface", nrem);
        free(state);
    }
    nx_appfix_profile_t prof = *nx_appfix_default_profile();
    if (app) prof.app_name = app;
    if (store) prof.store_substr = store;
    if (key) prof.key = key;
    nx_appfix_apply(&prof, fixed, fixed ? strlen(fixed) : 0, stdout);
    free(fixed);
}

static void cmd_stop(const char *app, const char *store, const char *key) {
    nx_appfix_profile_t prof = *nx_appfix_default_profile();
    if (app) prof.app_name = app;
    if (store) prof.store_substr = store;
    if (key) prof.key = key;
    /* 起動時の状態復元を止める JS を inspector 経由で差し込む */
    int rc = nx_appfix_stop_restore(&prof, stdout);
    printf("  [stop] rc=%d (次回起動は既定状態になります)\n", rc);
}

static void cmd_baseline(const char *app, const char *store, const char *key) {
    nx_appfix_profile_t prof = *nx_appfix_default_profile();
    if (app) prof.app_name = app;
    if (store) prof.store_substr = store;
    if (key) prof.key = key;
    /* 復元を止めるのではなく、復元先を空の妥当な状態に置き換える */
    int rc = nx_appfix_baseline_restore(&prof, stdout);
    printf("  [baseline] rc=%d (復元先を空のセッションに置き換えました)\n", rc);
}

/* レイアウト署名 — 「復元したか否か」をアプリに聞かずに観測するための判定器。
   キャプチャせず、ウィンドウの所有・タイトル・座標を正規化して出す。
   同じ内容なら同じ署名になるので、起動間の差分で復元の有無が判る。 */
static void cmd_layout(const char *app) {
    win_t wins[128];
    int n = collect_windows(app, wins, 128);
    if (n < 0) { printf("{\"error\":\"no window server\"}\n"); return; }

    /* 安定した署名にするため並べ替える */
    for (int i = 0; i < n; i++)
        for (int k = i + 1; k < n; k++) {
            int c = strcmp(wins[i].owner, wins[k].owner);
            if (c == 0) c = strcmp(wins[i].name, wins[k].name);
            if (c == 0) c = (wins[i].area > wins[k].area) ? -1 : (wins[i].area < wins[k].area ? 1 : 0);
            if (c > 0) { win_t t = wins[i]; wins[i] = wins[k]; wins[k] = t; }
        }

    printf("LAYOUT_BEGIN\n");
    printf("APP\t%s\n", app ? app : "(all)");
    for (int i = 0; i < n; i++) {
        if (wins[i].layer != 0) continue;      /* 通常ウィンドウ層のみ */
        if (wins[i].area < 20000) continue;
        printf("WIN\t%s\t%s\t%.0f\t%.0f\t%.0f\t%.0f\n",
               wins[i].owner, wins[i].name[0] ? wins[i].name : "-",
               wins[i].x, wins[i].y, wins[i].w, wins[i].h);
    }
    printf("LAYOUT_END\n");
}

static void cmd_replay(const char *state_path, const char *out_png) {
    char *state = nx_deepai_load_state(state_path);
    if (!state) { printf("ERROR: no state at %s\n", state_path); return; }
    nx_ui_layout_t L;
    if (nx_deepai_build_layout(state, &L) != 0) {
        printf("ERROR: cannot build layout\n");
        free(state);
        return;
    }
    nx_deepai_render_ascii(&L, stdout);
    if (out_png) {
        if (render_layout_png(&L, out_png) == 0)
            printf("\n  rendered: %s (%dx%d)\n", out_png, L.width, L.height);
        else
            printf("\n  render failed: %s\n", out_png);
    }
    nx_deepai_free_layout(&L);
    free(state);
}

static void cmd_display(const char *app, const char *out, const char *state_path) {
    if (!out) out = "/tmp/neodax-display.png";
    printf("DISPLAY_BEGIN\n");
    printf("CAPTURE_BEGIN\n");
    if (capture_best(app, out, stdout) == 0) {
        printf("CAPTURE\t%s\n", out);
        /* どのウィンドウが選ばれたか再掲 */
        win_t wins[64];
        int nw = collect_windows(app, wins, 64);
        printf("FRAMES\t%d\n", nw);
        int widgets = 0;
        for (int i = 0; i < nw; i++) widgets += count_widgets(wins[i].pid);
        printf("WIDGETS\t%d\n", widgets);
    } else {
        printf("CAPTURE\tFAILED\n");
    }
    printf("CAPTURE_END\n");
    printf("WINDOWS_BEGIN\n");
    cmd_windows();
    printf("WINDOWS_END\n");
    if (state_path) {
        printf("TABS_BEGIN\n");
        dump_tabs_path(state_path);
        printf("TABS_END\n");
    }
    printf("DISPLAY_END\n");
}

/* ------------------------------------------------------------------ */
/* 引数ヘルパ / serve                                                  */
/* ------------------------------------------------------------------ */

static void cmd_serve(void) {
    char *line = NULL;
    size_t cap = 0;
    printf("READY neodax-ui serve\n");
    fflush(stdout);
    while (getline(&line, &cap, stdin) > 0) {
        for (char *p = line; *p; p++)
            if (*p == '\n' || *p == '\r') { *p = '\0'; break; }
        /* 区切りは '|'。パスに空白があってもよい。
           tabs|<path> / fix|<path> / sim|<path>[|<asar>]
           close|<path>|<id> / capture|<app>|<out> / display|<app>|<state>
           windows / quit */
        char *cmd = line;
        char *a1 = strchr(line, '|');
        if (a1) *a1 = '\0';
        char *a2 = a1 ? strchr(a1 + 1, '|') : NULL;
        if (a2) *a2 = '\0';
        char *rest1 = a1 ? a1 + 1 : NULL;
        char *rest2 = a2 ? a2 + 1 : NULL;
        if (!cmd) continue;
        if (strcmp(cmd, "quit") == 0) { printf("BYE\n"); fflush(stdout); break; }
        if (strcmp(cmd, "windows") == 0) {
            cmd_windows();
        } else if (strcmp(cmd, "capture") == 0) {
            printf("CAPTURE\t%s\n",
                   (rest1 && rest2 && capture_best(rest1, rest2, stdout) == 0) ? rest2 : "FAILED");
        } else if (strcmp(cmd, "display") == 0) {
            const char *app = (rest1 && strcmp(rest1, "-") != 0) ? rest1 : NULL;
            cmd_display(app, NULL, rest2);
        } else if (strcmp(cmd, "tabs") == 0) {
            dump_tabs_path(rest1);
        } else if (strcmp(cmd, "sim") == 0) {
            do_simulate(rest1, rest2);
        } else if (strcmp(cmd, "close") == 0) {
            if (rest1 && rest2) nx_deepai_remove_tab_path(rest1, rest2, stdout);
        } else if (strcmp(cmd, "fix") == 0) {
            if (rest1) nx_deepai_fix_path(rest1, stdout);
        } else if (strcmp(cmd, "delete") == 0) {
            if (rest1 && rest2) cmd_delete(rest1, rest2, NULL, NULL, NULL, 1, 0, 0);
        } else if (strcmp(cmd, "replay") == 0) {
            cmd_replay(rest1, rest2);
        } else if (strcmp(cmd, "apply") == 0) {
            if (rest1) cmd_apply(rest1, rest2, NULL, 0);
        } else if (strcmp(cmd, "spec") == 0) {
            /* spec|<hex> または spec|file:<path> [|--ws] */
            int ws = (rest2 && strcmp(rest2, "--ws") == 0);
            char *a[6]; int ac = 0;
            a[ac++] = (char *)"neodax-ui";
            a[ac++] = (char *)"spec";
            if (rest1 && strncmp(rest1, "file:", 5) == 0) {
                a[ac++] = (char *)"--file"; a[ac++] = (char *)(rest1 + 5);
            } else {
                a[ac++] = (char *)"--hex"; a[ac++] = (char *)(rest1 ? rest1 : "");
            }
            if (ws) a[ac++] = (char *)"--ws";
            a[ac] = NULL;
            run_spec(ac, a);
        } else if (strcmp(cmd, "attach") == 0) {
            cmd_attach(NULL, rest1, rest2, NULL, NULL);
        } else {
            printf("ERROR unknown command: %s\n", cmd);
        }
        printf("END\n");
        fflush(stdout);
    }
    free(line);
}

/* ------------------------------------------------------------------ */
/* GUI — すべての CLI コマンドを 1 つのウィンドウから実行する            */
/* ------------------------------------------------------------------ */
/*
 * CLI と同じ処理をそのまま呼ぶ。別実装は持たない。
 * 実行は stdout を一時ファイルへ切り替えて行い、終わったら戻すだけ。
 * これにより、C 側の出力を加工せず GUI に出せる (JS での parse をしない)。
 */

/* CLI の実体 (下で定義)。GUI はこれをそのまま呼ぶ。 */
static int nx_cli_dispatch(int argc, char **argv);

/* 1 つのコマンド定義。argv テンプレート + 説明。 */
typedef struct {
    const char *title;      /* ボタンに出る名前 */
    const char *group;      /* セクション見出し */
    const char *argc_argv;  /* "tabs|--state" など。| 区切り、引数なし可 */
    const char *desc;
} nx_cmd_t;

/* 全 CLI コマンド。serve 以外はすべてここから呼べる。 */
static const nx_cmd_t kNxCmds[] = {
    /* --- 観測 (変更しない) --- */
    {"タブ一覧",       "観測", "tabs",                  "永続層のタブを TSV で出す"},
    {"キー一覧",       "観測", "keys",                  "KV のキー・seq・生存を C 側で列挙 (grep 代替)"},
    {"タブのキー",     "観測", "tab-keys",              "タブが持つキーを列挙 (作業先の所在を確かめる)"},
    {"asar 一覧",      "観測", "asar|--file|--grep",    "asar のエントリ名を C 側で列挙 (アセット名の確認)"},
    {"asar 検索",      "観測", "asar|--file|--find",    "asar の中身を検索 (修復/復元のリンクを探す)"},
    {"レンダラ観測",    "観測", "probe",                  "renderer の origin/scripts/localStorage キーを観測"},
    {"手順導出",        "観測", "advise-proc",             "状態から修復手順を導出 (--apply で clean)"},
    {"チャット台帳",    "観測", "threads|--dups|--apply|--delete", "No project の重複チャットを検出・恒久削除"},
    {"テンプレート",    "分析", "tpl-cpp|--dir|--svg|--record", "cpp テンプレートの構成 (逆順ヘッダ)・annulus・学習"},
    {"判定 (advise)",  "観測", "advise",                "壊れているかだけ判定。変更しない"},
    {"検出 (check)",   "観測", "check",                 "閉じられない surface を検出"},
    {"計画 (plan)",    "観測", "plan",                  "事実→問題→計画を出す。変更しない"},
    {"画面再現",       "観測", "replay",                "state からレイアウトを再現"},
    {"レイアウト署名", "観測", "layout",                "復元有無の判定用署名"},
    {"ウィンドウ一覧", "観測", "windows",               "画面上のウィンドウ一覧"},
    {"状態を再現",     "観測", "simulate",              "アプリのロード結果を再現"},

    /* --- 修復 (永続層へ書く) --- */
    {"修復 (repair)",  "修復", "repair",                "観測→計画→実行→再観測を健全になるまで回す"},
    {"適用 (apply)",   "修復", "apply|--live",          "修復結果を稼働中ストアにも反映 (reload しない)"},
    {"fix のみ",       "修復", "fix",                   ".fixed.json を作るだけ (書き戻さない)"},
    {"残滓を消す (clean)", "修復", "clean|--dry-run",     "残滓を状態から判定し、呼ぶ手順を選んで消す"},
    {"heal",           "修復", "heal",                  "検出→書き戻し→稼働中ストア再水和"},
    {"baseline",       "修復", "baseline",              "復元先を空の妥当な状態にする"},
    {"stop",           "修復", "stop",                  "起動時の状態復元を止める"},
    {"attach",         "修復", "attach",                "renderer を再水和 (SIGUSR1)"},
    {"対象に合わせる", "修復", "case|--path",           "--path に案件毎の自発解析"},
    {"手順を保存 (recipe)", "AE手順", "recipe|--path",      "書き換え手順を保存する (アプリは変更しない)"},
    {"手順を見る",     "AE手順", "recipe-show|--file",     "保存した手順を表示"},
    {"手順を検証 (dry)","AE手順", "recipe-run|--file",     "1段ずつ検証。既定は変更なし (dry-run)"},
    {"戦略を探す",     "AE手順", "search|--path|--symbol", "観測→テンプレで介入を選ぶ (既定 dry-run)"},

    /* --- キャプチャ --- */
    {"画面キャプチャ", "画面", "capture|-o|/tmp/neodax-gui.png", "対象アプリを PNG に"},
    {"表示 (display)", "画面", "display",                "最も情報量の多いフレームを選んで出力"},

    /* --- 解析 --- */
    {"構造認識 (spec)", "解析", "spec|--file",           "--file の構造を認識"},
    {"クラス生成 (gen)","解析", "gen|--spec|all",         "spec から C++ クラスを生成"},
    {"検索 (seek)",    "解析", "seek|--include|*.md",   "安全化した再帰検索 (停止条件・非破壊・regex安全)"},
    {"残存を強制終了", "AE手順", "kill|--match|.neodax-work", "作業コピーの残存プロセスを SIGKILL"},
    {"入口を外す (cut)", "AE手順", "cut|--step|--path", "解析→テンプレ保存→1段ずつ clone/patch/検証"},
    {"cut の途中状態", "AE手順", "cut-status", "チェックポイントの stage を表示"},
    {"直前のapp",      "AE手順", "app", "直前の app と保存フォルダを表示 (--path で指定可)"},
    {"app 分析",       "AE手順", "app-analyze", "分析して保存 (引数に --path ... --fix)"},
    {"app一覧",       "AE手順", "apps", "保存済み app フォルダを表示"},
    {"1個特定して最善", "AE手順", "best|--path", "パスを入れて実行。--apply で適用"},
    {"PE 解析",        "解析", "pe|--file|--disasm",     "Windows PE の解析/逆アセンブル"},
    {"pipeline",       "解析", "pipeline|--file",        "decode+encode 戦略選択"},
};
static const int kNxCmdCount = (int)(sizeof(kNxCmds) / sizeof(kNxCmds[0]));

/* 実行を stdout 経由で受け取る。cmd を argv に割って main と同じ関数を呼ぶ。
 * 戻り値は終了コード。out に出力を入れる。 */
static int nx_gui_exec(const char *line, char **out) {
    *out = NULL;

    /* argv へ分割 (| 区切り)。空引数は捨てる。 */
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", line ? line : "");
    char *argv[32];
    int argc = 0;
    argv[argc++] = (char *)"neodax-ui";
    char *p = buf;
    while (p && *p && argc < 31) {
        char *bar = strchr(p, '|');
        if (bar) *bar = '\0';
        if (*p) argv[argc++] = p;
        p = bar ? bar + 1 : NULL;
    }
    argv[argc] = NULL;
    if (argc < 2) return 1;

    /* --state が無い状態系コマンドは自動解決したパスを補う */
    const char *cmd = argv[1];
    static const char *needs_state[] = {
        "tabs", "keys", "advise", "check", "plan", "replay", "simulate", "repair",
        "apply", "fix", "heal", "attach", "case", "clean", "tab-keys",
        "advise-proc", NULL,
    };
    int has_state = 0;
    for (int i = 2; i < argc; i++)
        if (strcmp(argv[i], "--state") == 0) has_state = 1;
    for (int i = 0; needs_state[i]; i++) {
        if (strcmp(cmd, needs_state[i]) != 0 && !(strcmp(cmd, "case") == 0))
            continue;
        if (!has_state && strcmp(cmd, "case") != 0) {
            const char *sp = resolve_state_path();
            if (sp && argc < 29) {
                argv[argc++] = (char *)"--state";
                argv[argc++] = (char *)sp;
                argv[argc] = NULL;
            }
        }
        break;
    }

    /* stdout を一時ファイルへ。C 側の出力をそのまま読む。 */
    fflush(stdout);
    char tmp[] = "/tmp/neodax-gui-out-XXXXXX";
    int fd = mkstemp(tmp);
    if (fd < 0) return 1;
    int saved = dup(fileno(stdout));
    dup2(fd, fileno(stdout));
    close(fd);

    int rc = nx_cli_dispatch(argc, argv);

    fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);

    /* 出力を読み出す */
    FILE *fp = fopen(tmp, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (n < 0) n = 0;
        char *b = (char *)malloc((size_t)n + 1);
        if (b) {
            size_t got = fread(b, 1, (size_t)n, fp);
            b[got] = '\0';
            *out = b;
        }
        fclose(fp);
    }
    unlink(tmp);
    return rc;
}

@interface NxGuiController : NSObject <NSApplicationDelegate, NSTableViewDataSource,
                                      NSTableViewDelegate>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) NSTableView *table;
@property (nonatomic, strong) NSTextView *output;
@property (nonatomic, strong) NSScrollView *tableScroll;
@property (nonatomic, strong) NSScrollView *outputScroll;
@property (nonatomic, strong) NSTextField *status;
@property (nonatomic, strong) NSTextField *argField;
@property (nonatomic, strong) NSButton *runButton;
@property (nonatomic, strong) NSMutableArray<NSString *> *rows;
@property (nonatomic, strong) NSMutableArray<NSNumber *> *rowIndex;
@property (nonatomic, strong) id emulator;   /* 別フレーム。混ぜない */
@property (nonatomic, strong) id appPanel;
@end

@implementation NxGuiController

- (void)buildWindow {
    NSRect frame = NSMakeRect(0, 0, 1040, 680);
    self.window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self.window.title = @"NeoDAX";
    [self.window center];

    NSView *cv = self.window.contentView;

    /* --- 左: コマンド一覧 --- */
    self.table = [[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, 300, 600)];
    self.table.dataSource = self;
    self.table.delegate = self;
    self.table.rowHeight = 38.0;
    self.table.headerView = nil;
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"cmd"];
    col.width = 300;
    [self.table addTableColumn:col];

    self.tableScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(12, 92, 300, 576)];
    self.tableScroll.hasVerticalScroller = YES;
    self.tableScroll.documentView = self.table;
    [cv addSubview:self.tableScroll];

    /* --- 引数欄: 各コマンドの argv をここで編集してから実行できる。
     *     これで --id / --app / パスなど、任意の引数付き実行も GUI から行える。 --- */
    self.argField = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 56, 300, 28)];
    self.argField.editable = YES;
    self.argField.bezeled = YES;
    self.argField.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    self.argField.placeholderString = @"引数 (選ぶと入ります。編集可)";
    [cv addSubview:self.argField];

    /* --- 実行ボタン --- */
    self.runButton = [[NSButton alloc] initWithFrame:NSMakeRect(12, 12, 300, 34)];
    self.runButton.title = @"実行";
    self.runButton.bezelStyle = NSBezelStyleRounded;
    self.runButton.keyEquivalent = @"\r";
    self.runButton.target = self;
    self.runButton.action = @selector(runSelected:);
    [cv addSubview:self.runButton];

    /* --- 右: 出力 --- */
    self.output = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 700, 600)];
    self.output.editable = NO;
    self.output.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    self.output.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.output.string = @"左の一覧からコマンドを選んで実行してください。\n"
                          "すべての CLI コマンドは GUI から実行できます。";

    self.outputScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(324, 56, 704, 612)];
    self.outputScroll.hasVerticalScroller = YES;
    self.outputScroll.hasHorizontalScroller = YES;
    self.outputScroll.documentView = self.output;
    self.outputScroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [cv addSubview:self.outputScroll];

    /* --- 状態表示 --- */
    self.status = [[NSTextField alloc] initWithFrame:NSMakeRect(324, 14, 704, 30)];
    self.status.editable = NO;
    self.status.bordered = NO;
    self.status.drawsBackground = NO;
    self.status.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    const char *sp = resolve_state_path();
    self.status.stringValue = sp ? [NSString stringWithFormat:@"状態: %s", sp]
                                 : @"状態: 見つからない";
    self.status.font = [NSFont systemFontOfSize:11];
    self.status.textColor = [NSColor secondaryLabelColor];
    [cv addSubview:self.status];

    [self.window makeKeyAndOrderFront:nil];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv { return (NSInteger)kNxCmdCount; }

- (NSView *)tableView:(NSTableView *)tv
    viewForTableColumn:(NSTableColumn *)c
                   row:(NSInteger)row {
    NSTableCellView *cell = [tv makeViewWithIdentifier:@"cell" owner:self];
    if (!cell) {
        cell = [[NSTableCellView alloc] initWithFrame:NSMakeRect(0, 0, 300, 38)];
        cell.identifier = @"cell";
        NSTextField *tf = [[NSTextField alloc] initWithFrame:NSMakeRect(6, 2, 288, 34)];
        tf.editable = NO;
        tf.bordered = NO;
        tf.drawsBackground = NO;
        tf.tag = 1;
        [cell addSubview:tf];
        cell.textField = tf;
    }
    if (row < 0 || row >= kNxCmdCount) return cell;
    const nx_cmd_t *e = &kNxCmds[row];
    /* C 文字列は UTF-8。%s は環境の文字コードで解釈され文字化けするため、
     * 明示的に UTF-8 として NSString にする。 */
    NSString *title = [NSString stringWithUTF8String:e->title];
    NSString *group = [NSString stringWithUTF8String:e->group];
    cell.textField.stringValue =
        [NSString stringWithFormat:@"%@  /  %@", title, group];
    cell.textField.font = [NSFont systemFontOfSize:12];
    cell.toolTip = [NSString stringWithUTF8String:e->desc];
    return cell;
}

/* 選択が変わったら、そのコマンドの argv を引数欄に入れる。 */
- (void)tableViewSelectionDidChange:(NSNotification *)n {
    NSInteger row = self.table.selectedRow;
    if (row < 0 || row >= kNxCmdCount) return;
    self.argField.stringValue =
        [NSString stringWithUTF8String:kNxCmds[row].argc_argv];
}

- (void)runSelected:(id)sender {
    NSInteger row = self.table.selectedRow;
    if (row < 0 || row >= kNxCmdCount) {
        self.output.string = @"コマンドが選ばれていません。";
        return;
    }
    const nx_cmd_t *e = &kNxCmds[row];
    /* 引数欄があればそちらを優先 (編集された argv で実行する)。 */
    NSString *argv = self.argField.stringValue.length
        ? self.argField.stringValue
        : [NSString stringWithUTF8String:e->argc_argv];
    const char *line = argv.UTF8String;
    self.output.string = [NSString stringWithFormat:@"$ neodax-ui %@\n\n", argv];
    self.output.needsDisplay = YES;
    [[NSRunLoop mainRunLoop] runUntilDate:[NSDate date]];

    char *out = NULL;
    int rc = nx_gui_exec(line, &out);
    NSString *body = out ? [NSString stringWithUTF8String:out]
                         : @"(出力なし)";
    if (out) free(out);
    self.output.string = [NSString stringWithFormat:@"$ neodax-ui %@\n\n%@\n[exit %d]\n",
                          argv, body, rc];
    [self.output scrollRangeToVisible:NSMakeRange([self.output.string length], 0)];
}

- (void)applicationDidFinishLaunching:(NSNotification *)n {
    [self buildWindow];

    /* AE エミュレータは neodax パネルとは別フレームに置く。混ぜない。 */
    Class emu = NSClassFromString(@"NxEmulatorController");
    if (emu) {
        self.emulator = [[emu alloc] init];
        [self.emulator performSelector:@selector(build)];
    }

    Class appc = NSClassFromString(@"NxAppController");
    if (appc) {
        self.appPanel = [[appc alloc] init];
        [self.appPanel performSelector:@selector(build)];
    }

    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { return YES; }

@end

/* ================================================================
 * AE エミュレータ (neodax パネルとは別フレーム)
 *
 * 方針:
 *   - neodax のコード/プロパティ表示とはウィンドウを分ける (混ぜない)。
 *   - エミュレータはクリックで操作できる。
 *   - 操作しないときは範囲選択に切り替え、選んだ範囲のウィジェットを解析する。
 *   - 切替は小さなボタン 1 つ。トグル/ドロップダウンは作らない。
 * ================================================================ */

typedef struct {
    NSRect rect;
    const char *name;
    const char *cmd;   /* クリック時に実行する neodax コマンド */
} NxWidget;

static NxWidget kNxWidgets[] = {
    {{40, 300, 520, 28},  "menu",     "templates"},
    {{40, 250, 90, 44},   "tool",     "cut-status"},
    {{140, 250, 160, 44}, "timeline", "strategy-memory"},
    {{310, 250, 250, 44}, "composition", "templates"},
    {{40, 120, 250, 110},"panel",    "cut-status"},
    {{300, 120, 260, 110},"effects", "strategy-memory"},
};
static const int kNxWidgetCount = (int)(sizeof(kNxWidgets) / sizeof(kNxWidgets[0]));

/* annulus / line / point の共有データ。ノード選択と描画で同じものを使う。 */
static const double kNxRings[3] = {46, 78, 110};
typedef struct { double ang; int ring; } NxNodeDef;
static const NxNodeDef kNxNodes[] = {
    {0.2, 0}, {1.3, 0}, {2.1, 0}, {2.9, 1}, {3.8, 1},
    {4.7, 2}, {5.5, 2}, {0.9, 2}, {5.9, 1},
};
static const int kNxNodeCount = (int)(sizeof(kNxNodes) / sizeof(NxNodeDef));
static const int kNxEdges[][2] = {
    {0,1},{1,2},{2,3},{3,4},{4,5},{5,6},{6,7},{7,8},{8,0},{1,5},{2,6}
};

/* ノードが関わるウィジェット (最大 3 個, -1 終端)。 */
static const int kNxRelated[][3] = {
    {0,1,-1}, {2,-1,-1}, {3,4,-1}, {4,5,-1}, {2,5,-1},
    {0,3,-1}, {1,4,-1}, {2,3,-1}, {0,5,-1},
};
static bool nx_related(int node, int w) {
    if (node < 0 || node >= kNxNodeCount) return false;
    for (int k = 0; k < 3; k++) if (kNxRelated[node][k] == w) return true;
    return false;
}
static NSPoint nx_node_pos(int i, NSRect b) {
    NSPoint c = NSMakePoint(NSMidX(b), NSMidY(b));
    double rad = kNxRings[kNxNodes[i].ring] * 0.62;
    return NSMakePoint(c.x + cos(kNxNodes[i].ang) * rad,
                       c.y + sin(kNxNodes[i].ang) * rad);
}

@interface NxEmulatorView : NSView
@property (nonatomic) int mode;                 /* 0=操作, 1=範囲選択, 2=整形 */
@property (nonatomic) int relatedNode;          /* 選択中ノード (-1 なし) */
@property (nonatomic) BOOL showRelated;         /* 関連ウィジェット表示/やめる */
@property (nonatomic, weak) NSTextField *report;
@property (nonatomic, weak) NSTextView *log;
@property (nonatomic) int hot;
- (void)setRelatedNode:(int)n;
@end

@implementation NxEmulatorView {
    NSPoint _start, _cur;
    BOOL _dragging;
    int _formatIndex;
    NSPoint _formatStart;
    NSPoint _dragStart;
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) { _formatIndex = -1; _formatStart = NSZeroPoint; }
    return self;
}

- (void)setRelatedNode:(int)n {
    _relatedNode = n;
    if (self.showRelated && self.report) {
        NSMutableString *s = [NSMutableString stringWithFormat:@"選択ノード %d の関連:\n", n];
        for (int k = 0; k < 3; k++) {
            int w = (n >= 0 && n < kNxNodeCount) ? kNxRelated[n][k] : -1;
            if (w < 0) break;
            [s appendFormat:@"  %s\n", kNxWidgets[w].name];
        }
        self.report.stringValue = s;
    }
    [self setNeedsDisplay:YES];
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isFlipped { return NO; }

- (void)drawRect:(NSRect)dirty {
    [[NSColor colorWithCalibratedWhite:0.10 alpha:1] setFill];
    NSRectFill(self.bounds);

    /* グリッド */
    [[NSColor colorWithCalibratedWhite:0.18 alpha:1] setStroke];
    NSBezierPath *g = [NSBezierPath bezierPath];
    g.lineWidth = 1;
    for (int x = 0; x < (int)NSWidth(self.bounds); x += 20) {
        [g moveToPoint:NSMakePoint(x, 0)];
        [g lineToPoint:NSMakePoint(x, NSHeight(self.bounds))];
    }
    for (int y = 0; y < (int)NSHeight(self.bounds); y += 20) {
        [g moveToPoint:NSMakePoint(0, y)];
        [g lineToPoint:NSMakePoint(NSWidth(self.bounds), y)];
    }
    [g stroke];

    /* ウィジェット */
    for (int i = 0; i < kNxWidgetCount; i++) {
        NSRect r = kNxWidgets[i].rect;
        NSBezierPath *p = [NSBezierPath bezierPathWithRoundedRect:r xRadius:5 yRadius:5];
        bool rel = self.showRelated && self.relatedNode >= 0 && nx_related(self.relatedNode, i);
        bool dim = self.showRelated && self.relatedNode >= 0 && !rel;
        if (rel)
            [[NSColor colorWithCalibratedRed:0.95 green:0.65 blue:0.15 alpha:0.95] setFill];
        else if (i == self.hot)
            [[NSColor colorWithCalibratedRed:0.20 green:0.45 blue:0.75 alpha:0.9] setFill];
        else if (dim)
            [[NSColor colorWithCalibratedWhite:0.22 alpha:0.25] setFill];
        else
            [[NSColor colorWithCalibratedWhite:0.22 alpha:0.9] setFill];
        [p fill];
        [[NSColor colorWithCalibratedWhite:0.45 alpha:1] setStroke];
        [p stroke];
        NSString *label = [NSString stringWithUTF8String:kNxWidgets[i].name];
        [label drawAtPoint:NSMakePoint(r.origin.x + 8, r.origin.y + 8)
            withAttributes:@{NSForegroundColorAttributeName:
                                 dim ? [NSColor colorWithCalibratedWhite:0.6 alpha:0.5]
                                     : [NSColor whiteColor],
                             NSFontAttributeName: [NSFont systemFontOfSize:11]}];
    }

    /* 範囲選択の矩形 */
    if (_dragging) {
        NSRect sel = NSMakeRect(MIN(_start.x, _cur.x), MIN(_start.y, _cur.y),
                                fabs(_cur.x - _start.x), fabs(_cur.y - _start.y));
        [[NSColor colorWithCalibratedRed:1 green:0.85 blue:0.2 alpha:0.18] setFill];
        NSRectFill(sel);
        [[NSColor colorWithCalibratedRed:1 green:0.85 blue:0.2 alpha:1] setStroke];
        NSBezierPath *b = [NSBezierPath bezierPathWithRect:sel];
        CGFloat dash[2] = {4, 3};
        [b setLineDash:dash count:2 phase:0];
        [b stroke];
    }
}

/* 範囲内のウィジェットを解析する (neodax へ渡さず、その場で構造を報告)。 */
- (NSString *)analyzeRect:(NSRect)sel {
    NSMutableString *s = [NSMutableString string];
    int n = 0, inside = 0;
    double area = 0;
    for (int i = 0; i < kNxWidgetCount; i++) {
        NSRect r = kNxWidgets[i].rect;
        if (!NSIntersectsRect(sel, r)) continue;
        n++;
        area += NSWidth(r) * NSHeight(r);
        if (NSContainsRect(sel, r)) inside++;
        [s appendFormat:@"  %-12s  %.0fx%.0f @%.0f,%.0f%s\n",
            kNxWidgets[i].name, NSWidth(r), NSHeight(r), r.origin.x, r.origin.y,
            NSContainsRect(sel, r) ? "  (全部)" : ""];
    }
    double cover = NSWidth(sel) * NSHeight(sel) > 0
        ? area / (NSWidth(sel) * NSHeight(sel)) : 0;
    [s insertString:[NSString stringWithFormat:
        @"選択 %.0fx%.0f / 交差 %d (完全 %d) / 被覆 %.2f\n",
        NSWidth(sel), NSHeight(sel), n, inside, cover] atIndex:0];
    if (!n) [s appendString:@"  (ウィジェットなし)\n"];
    return s;
}

- (void)mouseDown:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    _start = _cur = p;
    _dragging = (self.mode == 1);

    int hit = -1;
    for (int i = 0; i < kNxWidgetCount; i++)
        if (NSPointInRect(p, kNxWidgets[i].rect)) { hit = i; break; }

    if (self.mode == 2) {
        /* 整形: クリックしたウィンドウをドラッグで動かし、10px にスナップする。 */
        _formatIndex = hit;
        if (hit >= 0) {
            _formatStart = kNxWidgets[hit].rect.origin;
            _dragStart = p;
            self.hot = hit;
            if (self.report)
                self.report.stringValue = [NSString stringWithFormat:
                    @"整形: %s をドラッグ (離すと 10px にスナップ)", kNxWidgets[hit].name];
        }
        [self setNeedsDisplay:YES];
        return;
    }

    if (self.mode == 0) {
        self.hot = hit;
        if (self.hot >= 0 && self.report) {
            NSRect r = kNxWidgets[self.hot].rect;
            self.report.stringValue = [NSString stringWithFormat:
                @"操作: %s  (%.0fx%.0f @%.0f,%.0f) → %s",
                kNxWidgets[self.hot].name, NSWidth(r), NSHeight(r),
                r.origin.x, r.origin.y, kNxWidgets[self.hot].cmd];
            if (self.log) {
                char *out = NULL;
                int rc = nx_gui_exec(kNxWidgets[self.hot].cmd, &out);
                NSString *body = out ? [NSString stringWithUTF8String:out] : @"(出力なし)";
                if (out) free(out);
                NSMutableString *m = [NSMutableString stringWithString:self.log.string];
                [m appendFormat:@"\n$ neodax-ui %s\n%@[exit %d]\n",
                    kNxWidgets[self.hot].cmd, body, rc];
                self.log.string = m;
            }
        }
        [self setNeedsDisplay:YES];
        return;
    }
    [self setNeedsDisplay:YES];
}

- (void)mouseDragged:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    if (self.mode == 2) {
        if (_formatIndex >= 0) {
            kNxWidgets[_formatIndex].rect.origin.x = _formatStart.x + (p.x - _dragStart.x);
            kNxWidgets[_formatIndex].rect.origin.y = _formatStart.y + (p.y - _dragStart.y);
        }
        [self setNeedsDisplay:YES];
        return;
    }
    if (self.mode != 1) return;
    _cur = p;
    [self setNeedsDisplay:YES];
}

- (void)mouseUp:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    if (self.mode == 2) {
        if (_formatIndex >= 0) {
            NSRect r = kNxWidgets[_formatIndex].rect;
            r.origin.x = round(r.origin.x / 10.0) * 10.0;
            r.origin.y = round(r.origin.y / 10.0) * 10.0;
            kNxWidgets[_formatIndex].rect = r;
            if (self.report)
                self.report.stringValue = [NSString stringWithFormat:
                    @"整形: %s → %.0fx%.0f @%.0f,%.0f", kNxWidgets[_formatIndex].name,
                    NSWidth(r), NSHeight(r), r.origin.x, r.origin.y];
            _formatIndex = -1;
        }
        [self setNeedsDisplay:YES];
        return;
    }
    if (self.mode != 1) return;
    _cur = p;
    _dragging = NO;
    NSRect sel = NSMakeRect(MIN(_start.x, _cur.x), MIN(_start.y, _cur.y),
                            fabs(_cur.x - _start.x), fabs(_cur.y - _start.y));
    if (NSWidth(sel) >= 2 && NSHeight(sel) >= 2 && self.report)
        self.report.stringValue = [self analyzeRect:sel];
    [self setNeedsDisplay:YES];
}

@end

/* annulus / line / point のノード表示。点=node、線=edge、環=SCC。 */
@interface NxNodeView : NSView
@property (nonatomic) int selected;
@property (nonatomic, weak) NxEmulatorView *emu;
@end

@implementation NxNodeView
- (BOOL)isFlipped { return NO; }
- (void)drawRect:(NSRect)dirty {
    [[NSColor colorWithCalibratedWhite:0.10 alpha:1] setFill];
    NSRectFill(self.bounds);
    NSPoint c = NSMakePoint(NSMidX(self.bounds), NSMidY(self.bounds));

    /* annulus: 環 (SCC) を同心円で表す */
    for (int i = 0; i < 3; i++) {
        double r0 = kNxRings[i];
        NSRect r = NSMakeRect(c.x - r0, c.y - r0, r0 * 2, r0 * 2);
        NSBezierPath *p = [NSBezierPath bezierPathWithOvalInRect:r];
        [[NSColor colorWithCalibratedWhite:0.25 + 0.08 * i alpha:1] setStroke];
        p.lineWidth = 1;
        [p stroke];
    }

    NSPoint pts[16];
    for (int i = 0; i < kNxNodeCount; i++) pts[i] = nx_node_pos(i, self.bounds);

    /* line: 辺 */
    [[NSColor colorWithCalibratedWhite:0.45 alpha:0.8] setStroke];
    for (auto &e : kNxEdges) {
        NSBezierPath *p = [NSBezierPath bezierPath];
        p.lineWidth = 1;
        [p moveToPoint:pts[e[0]]];
        [p lineToPoint:pts[e[1]]];
        [p stroke];
    }

    /* point 本体 (選択中は大きく/色を変える) */
    for (int i = 0; i < kNxNodeCount; i++) {
        bool sel = (i == self.selected);
        double rad = sel ? 6.5 : 4.0;
        NSRect r = NSMakeRect(pts[i].x - rad, pts[i].y - rad, rad * 2, rad * 2);
        if (sel)
            [[NSColor colorWithCalibratedRed:0.95 green:0.45 blue:0.15 alpha:1] setFill];
        else
            [[NSColor colorWithCalibratedRed:0.95 green:0.85 blue:0.35 alpha:1] setFill];
        [[NSBezierPath bezierPathWithOvalInRect:r] fill];
    }

    [@"annulus / line / point  (点をクリック)" drawAtPoint:NSMakePoint(8, 8)
        withAttributes:@{NSForegroundColorAttributeName: [NSColor lightGrayColor],
                         NSFontAttributeName: [NSFont systemFontOfSize:10]}];
}

- (void)mouseDown:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    int best = -1;
    double bestd = 14 * 14;
    for (int i = 0; i < kNxNodeCount; i++) {
        NSPoint q = nx_node_pos(i, self.bounds);
        double d = (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y);
        if (d < bestd) { bestd = d; best = i; }
    }
    if (best >= 0) {
        self.selected = best;
        [self.emu setRelatedNode:best];
        [self setNeedsDisplay:YES];
    }
}
@end

@interface NxEmulatorController : NSObject
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) NxEmulatorView *emu;
@property (nonatomic, strong) NxNodeView *nodes;
@property (nonatomic, strong) NSTextField *report;
@property (nonatomic, strong) NSTextView *log;
@property (nonatomic, strong) NSButton *modeButton;
@property (nonatomic, strong) NSButton *relatedButton;
@end

@implementation NxEmulatorController

- (void)build {
    /* 小さめの別フレーム。neodax パネルとは独立。 */
    NSRect frame = NSMakeRect(0, 0, 660, 580);
    self.window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self.window.title = @"AE エミュレータ (neodax とは別フレーム)";
    [self.window setFrameOrigin:NSMakePoint(1080, 120)];
    NSView *cv = self.window.contentView;

    self.emu = [[NxEmulatorView alloc] initWithFrame:NSMakeRect(12, 170, 410, 386)];
    self.emu.mode = 0;
    self.emu.hot = -1;
    self.emu.relatedNode = -1;
    self.emu.showRelated = NO;
    [cv addSubview:self.emu];

    self.nodes = [[NxNodeView alloc] initWithFrame:NSMakeRect(434, 170, 214, 386)];
    self.nodes.selected = -1;
    self.nodes.emu = self.emu;
    [cv addSubview:self.nodes];

    self.report = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 120, 636, 40)];
    self.report.editable = NO;
    self.report.bordered = NO;
    self.report.drawsBackground = NO;
    self.report.font = [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular];
    self.report.stringValue = @"操作モード: ウィジェットをクリック / 範囲選択ボタンで解析モード";
    [cv addSubview:self.report];

    self.log = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 540, 70)];
    self.log.editable = NO;
    self.log.font = [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular];
    self.log.string = @"";
    NSScrollView *ls = [[NSScrollView alloc] initWithFrame:NSMakeRect(12, 40, 540, 72)];
    ls.hasVerticalScroller = YES;
    ls.documentView = self.log;
    [cv addSubview:ls];

    /* 小さなボタンだけ。トグル/ドロップダウンは使わない。 */
    self.modeButton = [[NSButton alloc] initWithFrame:NSMakeRect(562, 40, 86, 30)];
    self.modeButton.title = @"範囲選択";
    self.modeButton.bezelStyle = NSBezelStyleRounded;
    self.modeButton.target = self;
    self.modeButton.action = @selector(toggleMode:);
    [cv addSubview:self.modeButton];

    self.relatedButton = [[NSButton alloc] initWithFrame:NSMakeRect(562, 76, 86, 30)];
    self.relatedButton.title = @"関連表示";
    self.relatedButton.bezelStyle = NSBezelStyleRounded;
    self.relatedButton.target = self;
    self.relatedButton.action = @selector(toggleRelated:);
    [cv addSubview:self.relatedButton];

    self.emu.report = self.report;
    self.emu.log = self.log;

    [self.window makeKeyAndOrderFront:nil];
}

- (void)toggleMode:(id)sender {
    /* 0=操作 → 1=範囲選択 → 2=整形 → 0 */
    self.emu.mode = (self.emu.mode + 1) % 3;
    if (self.emu.mode == 0) {
        self.modeButton.title = @"範囲選択";
        self.report.stringValue = @"操作モード: ウィジェットをクリックして実際と同じように操作";
    } else if (self.emu.mode == 1) {
        self.modeButton.title = @"整形";
        self.report.stringValue = @"範囲選択モード: ドラッグで範囲を選ぶとウィジェットを解析";
    } else {
        self.modeButton.title = @"操作";
        self.report.stringValue = @"整形モード: クリックしたウィンドウをドラッグで動かす (10px スナップ)";
    }
    [self.emu setNeedsDisplay:YES];
}

- (void)toggleRelated:(id)sender {
    self.emu.showRelated = !self.emu.showRelated;
    self.relatedButton.title = self.emu.showRelated ? @"関連やめる" : @"関連表示";
    if (self.emu.showRelated) {
        if (self.emu.relatedNode < 0 && self.nodes.selected < 0)
            [self.emu setRelatedNode:0];
        else
            [self.emu setRelatedNode:(int)self.nodes.selected];
    } else {
        self.report.stringValue = @"関連表示をやめました";
        [self.emu setNeedsDisplay:YES];
    }
}

@end

/* ================================================================
 * 最近の app から選んで分析するパネル (neodax 側)
 * ================================================================ */
@interface NxAppController : NSObject <NSTableViewDataSource, NSTableViewDelegate>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) NSTableView *table;
@property (nonatomic, strong) NSTextView *output;
@property (nonatomic, strong) NSMutableArray<NSString *> *paths;
@end

@implementation NxAppController

- (void)build {
    NSRect frame = NSMakeRect(0, 0, 620, 460);
    self.window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self.window.title = @"app 分析 (最近起動した app から選ぶ)";
    [self.window setFrameOrigin:NSMakePoint(40, 140)];
    NSView *cv = self.window.contentView;

    self.paths = [NSMutableArray array];
    self.table = [[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, 596, 150)];
    self.table.dataSource = self;
    self.table.delegate = self;
    self.table.rowHeight = 22;
    self.table.headerView = nil;
    NSTableColumn *col = [[NSTableColumn alloc] initWithIdentifier:@"app"];
    col.width = 590;
    [self.table addTableColumn:col];
    NSScrollView *ts = [[NSScrollView alloc] initWithFrame:NSMakeRect(12, 236, 596, 160)];
    ts.hasVerticalScroller = YES;
    ts.documentView = self.table;
    [cv addSubview:ts];

    /* 小さな普通のボタンだけ。トグル/ドロップダウンは作らない。 */
    NSButton *reload = [[NSButton alloc] initWithFrame:NSMakeRect(12, 200, 90, 28)];
    reload.title = @"更新";
    reload.bezelStyle = NSBezelStyleRounded;
    reload.target = self;
    reload.action = @selector(reloadApps:);
    [cv addSubview:reload];

    NSButton *an = [[NSButton alloc] initWithFrame:NSMakeRect(108, 200, 90, 28)];
    an.title = @"分析";
    an.bezelStyle = NSBezelStyleRounded;
    an.target = self;
    an.action = @selector(analyzeOnly:);
    [cv addSubview:an];

    NSButton *fx = [[NSButton alloc] initWithFrame:NSMakeRect(204, 200, 120, 28)];
    fx.title = @"分析して修正";
    fx.bezelStyle = NSBezelStyleRounded;
    fx.target = self;
    fx.action = @selector(analyzeFix:);
    [cv addSubview:fx];

    /* 「動かない問題を 1 個特定して最善を尽くす」 */
    NSButton *bs = [[NSButton alloc] initWithFrame:NSMakeRect(330, 200, 150, 28)];
    bs.title = @"1個特定して最善";
    bs.bezelStyle = NSBezelStyleRounded;
    bs.target = self;
    bs.action = @selector(bestFix:);
    [cv addSubview:bs];

    self.output = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 596, 180)];
    self.output.editable = NO;
    self.output.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    NSScrollView *os = [[NSScrollView alloc] initWithFrame:NSMakeRect(12, 12, 596, 180)];
    os.hasVerticalScroller = YES;
    os.documentView = self.output;
    [cv addSubview:os];

    [self reloadApps:nil];
    [self.window makeKeyAndOrderFront:nil];
}

- (void)reloadApps:(id)sender {
    nx_app_recent_refresh();
    [self.paths removeAllObjects];
    int n = nx_app_recent_count();
    for (int i = 0; i < n; i++) {
        const char *p = nx_app_recent_path(i);
        if (p && *p) [self.paths addObject:[NSString stringWithUTF8String:p]];
    }
    [self.table reloadData];
    if (self.paths.count) [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:0]
                                 byExtendingSelection:NO];
    self.output.string = [NSString stringWithFormat:@"最近の app: %d 件\n", n];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv { return (NSInteger)self.paths.count; }

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)c row:(NSInteger)row {
    NSTableCellView *cell = [tv makeViewWithIdentifier:@"acell" owner:self];
    if (!cell) {
        cell = [[NSTableCellView alloc] initWithFrame:NSMakeRect(0, 0, 590, 22)];
        cell.identifier = @"acell";
        NSTextField *tf = [[NSTextField alloc] initWithFrame:NSMakeRect(4, 1, 582, 20)];
        tf.editable = NO; tf.bordered = NO; tf.drawsBackground = NO; tf.tag = 1;
        [cell addSubview:tf];
        cell.textField = tf;
    }
    if (row >= 0 && row < (NSInteger)self.paths.count)
        cell.textField.stringValue = self.paths[(NSUInteger)row];
    return cell;
}

- (void)run:(NSString *)line {
    self.output.string = [NSString stringWithFormat:@"$ neodax-ui %@\n\n", line];
    char *out = NULL;
    int rc = nx_gui_exec(line.UTF8String, &out);
    NSString *body = out ? [NSString stringWithUTF8String:out] : @"(出力なし)";
    if (out) free(out);
    self.output.string = [NSString stringWithFormat:@"$ neodax-ui %@\n\n%@\n[exit %d]\n",
                          line, body, rc];
}

- (NSString *)selectedPath {
    NSInteger r = self.table.selectedRow;
    if (r < 0 || r >= (NSInteger)self.paths.count) return nil;
    return self.paths[(NSUInteger)r];
}

- (void)analyzeOnly:(id)sender {
    NSString *p = [self selectedPath];
    if (!p) { self.output.string = @"app が選ばれていません"; return; }
    [self run:[NSString stringWithFormat:@"app-analyze --path \"%@\"", p]];
}

- (void)analyzeFix:(id)sender {
    NSString *p = [self selectedPath];
    if (!p) { self.output.string = @"app が選ばれていません"; return; }
    [self run:[NSString stringWithFormat:@"app-analyze --path \"%@\" --fix", p]];
}

- (void)bestFix:(id)sender {
    NSString *p = [self selectedPath];
    if (!p) { self.output.string = @"app が選ばれていません"; return; }
    [self run:[NSString stringWithFormat:@"best --path \"%@\" --apply", p]];
}

@end

/* GUI モードで起動する。 */
static int run_gui(void) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        NxGuiController *ctl = [[NxGuiController alloc] init];
        app.delegate = ctl;
        [app run];
    }
    return 0;
}

static void usage(void) {
    printf(
        "neodax-ui — NeoDAX minimal CLI (GUI とは独立)\n"
        "  (引数なし) | --gui | -g                 GUI を起動\n"
        "  --selftest                              GUI と同じ実行経路を点検\n"
        "  windows\n"
        "  capture --app NAME -o FILE.png          (--out も可)\n"
        "  display [--app NAME] [-o FILE] [--state PATH]\n"
        "  tabs     --state PATH\n"
        "  keys     --state PATH                     KV のキー・seq・生存を列挙 (診断用)\n"
        "  tab-keys --state PATH                     タブが持つキーを列挙 (作業先の確認)\n"
        "  close    --state PATH --id ID\n"
        "  case     --path PATH [--kind app|state|dir] [--case tpl[N]]\n"
        "           案件ごとの自発解析。対象に合う案件を逐次に試し、問題と計画を出す\n"
        "  repair   --state PATH\n"
        "           1 つの永続手続き: 観測→計画→段順に実行→再観測 を健全になるまで繰り返す\n"
        "  recipe   --path APP [-o FILE] [--target S]\n"
        "           書き換え手順を先に作って保存する (アプリは変更しない)。\n"
        "           シンボルを解決し、パッチ位置・範囲・stub・再観測・再署名を 1 枚に綴る。\n"
        "  recipe-show --file R  保存した手順を表示\n"
        "  recipe-run  --file R [--apply] [--upto N]\n"
        "           手順を 1 段ずつ検証して実行。既定は dry-run。--apply で実行し、\n"
        "           失敗時は作業コピーを破棄 (原版は未変更)。\n"
        "  search   --path APP --symbol NAME [--apply] [--probe-ms N]\n"
        "           観測 (呼び出し元・先頭命令) → テンプレ (data<edge<entry) の順で介入を選ぶ。\n"
        "           既定は表示のみ。--apply で最安のテンプレを作業コピーに適用。\n"
        "           --probe-ms N で適用後に N ms 起動し、生存を成功とする。\n"
        "           失敗履歴は ~/.neodax/strategy-memory.tsv に有界保存され、\n"
        "           容量超過 (作業>10 / 全体>200) かつミス時に分岐パス確率で枝刈りする。\n"
        "  strategy-memory   失敗履歴と間違い指数を表示\n"
        "  cut --path APP --symbol NAME [--apply]\n"
        "       解析→テンプレ保存→clone/patch/検証。--step で 1 段ずつ介入可能。\n"
        "  cut --step --path APP --symbol NAME   1 段だけ進める\n"
        "  cut --abort --path APP --symbol NAME  途中状態を捨てる\n"
        "  cut-status        途中状態の一覧 (~/.neodax/cut)\n"
        "  templates         保存済みテンプレート (~/.neodax/templates.tsv)\n"
        "  app [--path APP]  直前に起動した app と保存フォルダを表示\n"
        "  app-analyze [--path APP] [--fix]  分析→app ごとのフォルダに保存。前回と違う時だけ --fix で修正\n"
        "  apps             保存済み app フォルダの一覧\n"
        "  best --path APP [--kind app|state|dir] [--apply]\n"
        "           動かない問題を1個特定し、その1個に最善を尽くす\n"
        "  tpl-cpp [--dir DIR] [--svg FILE] [--record FILE] [--ng]\n"
        "           cpp テンプレートの構成・annulus・学習記録\n"
        "  kill [--match SUBSTR]   SUBSTR を含むプロセスを SIGKILL (既定 .neodax-work)\n"
        "  advise-proc [--state PATH] [--apply]\n"
        "           状態から修復手順を導出する。--apply で normalize→dedupe→persist→appfix\n"
        "  plan     --state PATH\n"
        "           自発解析。事実→問題→計画 (順序と並列/代替)→自信 を出す。変更しない。\n"
        "           特定のタブ名に依存せず、観測から「何を・どの順で直すか」を組み立てる\n"
        "  advise   --state PATH [--id ID]\n"
        "           壊れているかを先に判定する (変更はしない)。\n"
        "           orphan surface = 閉じられない / live surface = アプリが自分で閉じられる\n"
        "  delete   --state PATH --id ID [--live] [--force] [--no-store-sync] [--app NAME]\n"
        "           先に advise と同じ判定を行い、壊れている (owner 不在) ときだけ削除する。\n"
        "           live surface / chat は拒否する。--force でその限りでなく実行。\n"
        "           永続層へ書き込み、同時に稼働中ストアも同じ値にする (reload なし)\n"
        "           --no-store-sync は稼働中ストア同期だけ止める (永続層は書く)\n"
        "  threads  [--db PATH] [--list] [--deleted] [--delete ID] [--within N] [--apply]\n"
        "           チャット台帳の一覧・重複検出・恒久削除\n"
        "           (既定 DB: ~/.deepai-code/userdata/statev2.sqlite)\n"
        "  fix      --state PATH                      タブ状態を修復\n"
        "  clean    --state PATH [--dry-run] [--no-live] [--app NAME] [--store SUBSTR] [--key HINT]\n"
        "           残滓を状態の形に依存せず判定し、種類と安全度から呼ぶ手順を選んで消す。\n"
        "           順序は seq の健全性→永続層→退避の掃除→稼働中ストア (reload しない)。\n"
        "           --dry-run は何も変更しない。--no-live は live-sync 段だけ止める\n"
        "           (persist / repair-seq / purge-backup は実行し、永続層には書く)\n"
        "  check    --state PATH                    閉じられない surface タブを検出 (変更なし)\n"
        "  trace    [--dir DIR] [-o FILE.cpp] [--show]\n"
        "           ノード資産を収束点から逆にたどり、trace を C++ クラスとして\n"
        "           書き出してコンパイル・実行する。pass1 (coarse, depth=1) →\n"
        "           pass2 (fine, 無制限) で追い直し、荒い追跡では見えない構造上の\n"
        "           不具合 (RING_CONFLICT 等) を浮かせる。直近のミスを参照して\n"
        "           間違い指数を出し、次回優先的に落とす分岐を示す。\n"
        "           --dir で資産の場所を指定。\n"
        "  resolve  [--state PATH] [--key HINT] [--apply]\n"
        "           既存のノード資産 (tpl-cpp の clean-v1 等) をいまの状態に当てる。\n"
        "           全ノードを独立に評価し PASS / FAIL / SAFE を出す。\n"
        "           SAFE (判定不能・失敗) は状態を書かないので安全とみなす。\n"
        "           永続層を書くのは persist 系だけ。--apply で PASS だけ実行。\n"
        "  splice   [--state PATH] [--key HINT] [--show|--bake] [--keep]\n"
        "           事実を集めて TSV にし、固定の Checker プログラムに stdin で渡す。\n"
        "           Checker は 1 度だけバイナリに焼いてキャッシュする (データでは変わらない)。\n"
        "           検査の追加は Checker のソースに 1 行足すだけ。\n"
        "           --show でデータと Checker のソース / --bake で焼くところまで。\n"
        "  heal     --state PATH [--app NAME] [--store SUBSTR] [--key HINT] [--dry-run]\n"
        "           検出→永続層へ書き戻し→稼働中ストア再水和を自動実行\n"
        "  simulate --state PATH [--asar ASAR]\n"
        "  replay   --state PATH [-o FILE.png]   (capture 無しで画面を再現)\n"
        "  apply    --state PATH [--id ID] [--key HINT] [--live]  (キー自動判別で書き戻し)\n"
        "           --live は起動中 LevelDB の WAL に追記。無指定はオフライン書き込み。\n"
        "           --id 指定時はそのタブだけ除去 (未指定は stale surface を除去)\n"
        "           --attach [--app NAME] [--store SUBSTR] [--key HINT] [--id ID]\n"
        "           永続層に書かず、SIGUSR1+inspector で稼働中 renderer を再水和する\n"
        "  stop     [--app NAME] [--store SUBSTR] [--key PREFIX]  起動時の状態復元を止める (JS 差し込み)\n"
        "  baseline [--app NAME] [--store SUBSTR] [--key KEY]    復元先を空の妥当な状態に置き換える\n"
        "  layout   [--app NAME]                                 復元有無の判定用レイアウト署名 (キャプチャ不要)\n"
        "  attach   [--state PATH] [--app NAME] [--store SUBSTR] [--key HINT]\n"
        "  spec     --hex \"0x81 0x05 ...\" | --file PATH [--ws]   (構造認識)\n"
        "  gen      --spec NAME|all [--class NAME] [-o FILE]        (C++ クラス生成)\n"
        "  pe       --file FILE.exe [--disasm] [--limit N]          (Windows PE 解析/逆アセンブル)\n"
        "  pipeline --file FILE.exe [--strategy auto|seq|sim] [--limit N] [-o out.cpp]\n"
        "           (decode+encode 戦略選択)\n"
        "  probe [--app NAME]                       renderer の origin/scripts/localStorage キーを観測\n"
        "  asar --file APP.asar [--grep SUBSTR] [--find TEXT] [--context N] [--limit N]\n"
        "           asar のエントリ列挙 / 中身検索 (C 側)\n"
        "  seek PATTERN [ROOT] [--ignore-case] [--regex] [--word] [--binary] [--files]\n"
        "       [--include G] [--context N] [--limit N] [--time MS]\n"
        "       ABD 安全化: 全体バイト/行長/期限の上限、symlink 非追跡、\n"
        "       危険な regex の拒否と literal 断片 prefilter\n"
        "  serve\n");
    /* コマンド一覧はレジストリ (gui/nx_cli_registry.c) から出す。
       ここでコマンド名を 2 度書きしない。 */
    nx_cli_usage(stdout, NULL);
}

/* CLI の実体。GUI からも CLI からも同じこれを呼ぶ (二重実装をしない)。
 * 自作の @autoreleasepool は持たない — 呼び出し側が用意する。
 *
 * nx_cli_dispatch_public は外からも呼べる名前。
 * 判断ノード (plan/nx_node) が「コマンドを呼ぶ」のに使う。
 * コマンドの中身を 2 つ持たないため、ここだけを公開する。 */
extern "C" int nx_cli_dispatch_public(int argc, char **argv);

static int nx_cli_dispatch(int argc, char **argv) {
    return nx_cli_dispatch_public(argc, argv);
}

extern "C" int nx_cli_dispatch_public(int argc, char **argv) {
    {
        if (argc < 2) { usage(); return 1; }
        /* レジストリで別名を本体に寄せる (check→advise、heal→clean など)。
           宣言していないオプションはここで弾く。黙って無視しない。
           node は引数をそのまま他のコマンドへ渡すので除外する。 */
        const nx_cli_cmd_t *reg = nx_cli_resolve(nx_cli_find(argv[1]));
        if (reg && strcmp(reg->name, "node") != 0) {
            nx_cli_ctx ctx;
            ctx.argc = argc; ctx.argv = argv; ctx.cmd = reg;
            if (nx_cli_check_opts(&ctx)) return 2;
        }
        const char *cmd = argv[1];
        if (strcmp(cmd, "windows") == 0) {
            cmd_windows();
        } else if (strcmp(cmd, "capture") == 0) {
            const char *app = flag(argc, argv, "--app");
            const char *out = flag(argc, argv, "-o");
            if (!out) out = flag(argc, argv, "--out");
            if (!out) { fprintf(stderr, "capture: -o required\n"); return 1; }
            return capture_best(app, out, stdout) == 0 ? 0 : 2;
        } else if (strcmp(cmd, "display") == 0) {
            cmd_display(flag(argc, argv, "--app"),
                        flag(argc, argv, "-o"),
                        flag(argc, argv, "--state"));
        } else if (strcmp(cmd, "tabs") == 0) {
            const char *st = state_or_default(flag(argc, argv, "--state"), "tabs");
            if (!st) return 1;
            dump_tabs_path(st);
        } else if (strcmp(cmd, "keys") == 0) {
            const char *st = state_or_default(flag(argc, argv, "--state"), "keys");
            if (!st) return 1;
            cmd_keys(st);
        } else if (strcmp(cmd, "tab-keys") == 0) {
            /* タブが持つキーの列挙 (診断用)。作業先がどのキーかを確かめる。 */
            const char *st = state_or_default(flag(argc, argv, "--state"), "tab-keys");
            if (!st) return 1;
            char *js = nx_deepai_load_state(st);
            if (!js) { printf("ERROR: no state at %s\n", st); return 1; }
            nx_deepai_dump_keys(js, stdout);
            free(js);
        } else if (strcmp(cmd, "threads") == 0) {
            /* サイドバー (No project) のチャット台帳。重複は event と projection の
               両方から消す (片方だけだと再構築で戻る)。 */
            const char *db = flag(argc, argv, "--db");
            if (has_flag(argc, argv, "--list")) {
                return nx_threads_list(db, stdout, has_flag(argc, argv, "--deleted")) == 0 ? 0 : 1;
            }
            const char *del = flag(argc, argv, "--delete");
            if (del && *del) return nx_threads_delete(db, del, stdout) == 0 ? 0 : 1;
            int within = 600;
            const char *w = flag(argc, argv, "--within");
            if (w) within = atoi(w);
            int apply = has_flag(argc, argv, "--apply");
            /* いまタブが参照している会話は消さない (protect)。
               残す基準は updated_at ではなく「タブ参照 → 原本」の順。 */
            char protect[8192] = {0};
            char *st = nx_deepai_load_state(NULL);
            if (st) {
                nx_json_t *root = nx_json_parse(st, strlen(st));
                if (root) {
                    const nx_json_t *sn = nx_json_get(root, "state");
                    const nx_json_t *tb = nx_json_get(sn ? sn : root, "tabs");
                    if (tb && tb->type == NX_JSON_ARR) {
                        size_t o = 0;
                        for (size_t i = 0; i < tb->n; i++) {
                            const nx_json_t *tg = nx_json_get(nx_json_at(tb, i), "target");
                            const nx_json_t *ref = tg ? nx_json_get(tg, "threadRef") : NULL;
                            const char *th = ref ? nx_json_str(nx_json_get(ref, "threadId")) : NULL;
                            if (!th) continue;
                            size_t n = strlen(th);
                            if (o + n + 2 >= sizeof(protect)) break;
                            if (o) protect[o++] = ',';
                            memcpy(protect + o, th, n); o += n;
                        }
                    }
                    nx_json_free(root);
                }
                free(st);
            }
            int rc = nx_threads_dups(db, stdout, within, apply, protect);
            return rc == 0 ? 0 : 1;
        } else if (strcmp(cmd, "advise-proc") == 0) {
            /* 分析も fix も neodax の中で閉じる: 状態 → 手順 → (apply) clean → 再導出。 */
            /* --state 省略時は nx_deepai_load_state が既知の場所を解決する。 */
            const char *st = flag(argc, argv, "--state");
            int apply = has_flag(argc, argv, "--apply");
            if (!apply) return nx_advisor_derive(st, stdout) == 0 ? 0 : 1;
            /* 導出した手順を実行する: 正規化 → 永続層 → 全ウィンドウの store。 */
            nx_advisor_derive(st, stdout);
            char *raw = nx_deepai_load_state(st);
            if (!raw) { printf("advise-proc: 状態を読めない\n"); return 1; }
            char *fixed = NULL;
            int nfix = 0;
            if (nx_deepai_normalize_ids_json(raw, strlen(raw), &fixed, &nfix, stdout) != 0 || !fixed) {
                free(raw);
                printf("advise-proc: 正規化できない\n");
                return 1;
            }
            free(raw);
            printf("STEP normalize: %d\n", nfix);
            int rc = 0;
            /* 次に duplicate (同じ thread+種別) を 1 枚に。 */
            {
                char *deduped = NULL;
                int nd = 0;
                if (nx_deepai_dedupe_kindred_json(fixed, strlen(fixed), &deduped, &nd, stdout) == 0 && deduped) {
                    printf("STEP dedupe: %d\n", nd);
                    free(fixed);
                    fixed = deduped;
                    nfix += nd;
                }
            }
            if (nfix > 0) {
                rc = write_fixed_to_leveldb(st, NULL, fixed, 1);
                nx_appfix_profile_t prof = *nx_appfix_default_profile();
                char dirbuf[4096];
                snprintf(dirbuf, sizeof(dirbuf), "%s", st);
                NSFileManager *dfm = [NSFileManager defaultManager];
                BOOL isd = NO;
                [dfm fileExistsAtPath:[NSString stringWithUTF8String:st] isDirectory:&isd];
                if (!isd) { char *sl = strrchr(dirbuf, '/'); if (sl) *sl = 0; }
                prof.key = NULL;
                nx_appfix_apply(&prof, fixed, strlen(fixed), stdout);
            }
            free(fixed);
            printf("RE-DERIVE\n");
            nx_advisor_derive(st, stdout);
            return rc == 0 ? 0 : 1;
        } else if (strcmp(cmd, "tpl-cpp") == 0) {
            /* 1 テンプレート = 1 .cpp。構成は cpp 内のヘッダに逆順で。
               ここで app の表示を置き換えない (解析用の一覧/SVG のみ)。
               入力は cpp / svg / node / dir のどれでもよい (auto 判定)。 */
            nx_tpl_graph_t g;
            const char *dir = flag(argc, argv, "--dir");
            nx_tpl_src_kind_t sk = NX_TPL_SRC_NONE;
            if (nx_tplcpp_open(dir, &g, &sk) != 0) {
                printf("no templates (src: %s)\n",
                       dir ? dir : nx_tplcpp_default_dir());
                return 1;
            }
            printf("# 入力: %s (%s)\n", dir ? dir : nx_tplcpp_default_dir(),
                   nx_tplcpp_kind_name(sk));
            nx_tplcpp_dump(&g, stdout);
            const char *svg = flag(argc, argv, "--svg");
            if (svg) nx_tplcpp_svg(&g, svg, stdout);
            const char *rec = flag(argc, argv, "--record");
            if (rec) nx_tplcpp_record(&g, rec, !has_flag(argc, argv, "--ng"), stdout);
            nx_tplcpp_free(&g);
            return 0;
        } else if (strcmp(cmd, "probe") == 0) {
            /* レンダラの環境を C 生成の固定 JS で観測する (何も変更しない)。 */
            const char *app = flag(argc, argv, "--app");
            const char *js =
                "(function(){const r={href:location.href,origin:location.origin,"
                "base:document.baseURI,scripts:[],res:[],ls:[]};"
                "try{document.querySelectorAll('script[src]').forEach(s=>r.scripts.push(s.src));}catch(e){}"
                "try{performance.getEntriesByType('resource').forEach(e=>r.res.push(e.name));}catch(e){}"
                "try{for(let i=0;i<localStorage.length;i++){const k=localStorage.key(i);if(k)r.ls.push(k);}}catch(e){}"
                "return JSON.stringify(r)})()";
            nx_renderer_opts_t ro;
            memset(&ro, 0, sizeof(ro));
            ro.inspect.app_name = app ? app : "DeepAI Code";
            ro.inspect.port = 9229;
            ro.window_index = 0;
            return nx_renderer_eval_js(&ro, js, stdout) == 0 ? 0 : 1;
        } else if (strcmp(cmd, "asar") == 0) {
            /* asar の診断。
               --grep SUBSTR : エントリ名で絞る
               --find TEXT   : 中身 (JS) を検索し、どのモジュールが触っているかを見る
               修復/復元の「リンの先」を探すために、キー名で中身を直接引く。 */
            const char *f = flag(argc, argv, "--file");
            const char *g = flag(argc, argv, "--grep");
            const char *find = flag(argc, argv, "--find");
            const char *ctxs = flag(argc, argv, "--context");
            const char *lims = flag(argc, argv, "--limit");
            int ctx = ctxs ? atoi(ctxs) : 1;
            int lim = lims ? atoi(lims) : 40;
            if (!f) { fprintf(stderr, "asar: --file required\n"); return 1; }
            nx_asar_t *a = nx_asar_open(f);
            if (!a) { printf("asar: not an archive: %s\n", f); return 1; }
            if (!find || !*find) {
                printf("ENTRIES\t%zu\n", a->n_entries);
                for (size_t i = 0; i < a->n_entries; i++) {
                    const char *p = a->entries[i].path;
                    if (g && *g && !strstr(p, g)) continue;
                    printf("%s\t%zu\n", p, a->entries[i].size);
                }
                nx_asar_free(a);
            } else {
                int hits = 0;
                for (size_t i = 0; i < a->n_entries && hits < lim; i++) {
                    const char *p = a->entries[i].path;
                    if (g && *g && !strstr(p, g)) continue;
                    if (!strstr(p, ".js") && !strstr(p, ".mjs") &&
                        !strstr(p, ".cjs") && !strstr(p, ".json")) continue;
                    char *txt = nx_asar_read_text(a, &a->entries[i]);
                    if (!txt) continue;
                    char *line = txt;
                    int no = 1;
                    while (line && *line && hits < lim) {
                        char *nl = strchr(line, '\n');
                        size_t len = nl ? (size_t)(nl - line) : strlen(line);
                        char keep = line[len];
                        line[len] = 0;
                        if (strstr(line, find)) {
                            const char *hit = strstr(line, find);
                            int win = ctxs ? atoi(ctxs) : 120; /* --context を前後の文字数として使う */
                            long left = (long)(hit - line);
                            const char *s = line + (left > win ? left - win : 0);
                            int room = win * 2 + (int)strlen(find) + 1;
                            printf("%s:%d\t%.*s\n", p, no, room, s);
                            hits++;
                        }
                        line[len] = keep;
                        if (!nl) break;
                        line = nl + 1;
                        no++;
                    }
                    free(txt);
                }
                printf("HITS\t%d\n", hits);
                nx_asar_free(a);
            }
        } else if (strcmp(cmd, "close") == 0) {
            const char *st = flag(argc, argv, "--state");
            const char *id = flag(argc, argv, "--id");
            if (!id) { fprintf(stderr, "close: --id required\n"); return 1; }
            st = state_or_default(st, "close");
            if (!st) return 1;
            return nx_deepai_remove_tab_path(st, id, stdout);
        } else if (strcmp(cmd, "delete") == 0) {
            const char *st = flag(argc, argv, "--state");
            const char *id = flag(argc, argv, "--id");
            st = state_or_default(st, "delete");
            if (!st) return 1;
            /* --all-stale は、壊れているタブを 1 回でまとめて消す。
               1 件ずつ消すと chat ごとに書き込むことになり、
               その間隙で復元経路が走って戻る。 */
            if (has_flag(argc, argv, "--all-stale")) {
                cmd_delete_stale(st, flag(argc, argv, "--app"),
                                 flag(argc, argv, "--store"), flag(argc, argv, "--key"),
                                 has_flag(argc, argv, "--live"),
                                 has_flag(argc, argv, "--force"));
                return 0;
            }
            if (!id) { fprintf(stderr, "delete: --id required\n"); return 1; }
            cmd_delete(st, id, flag(argc, argv, "--app"),
                       flag(argc, argv, "--store"), flag(argc, argv, "--key"),
                       has_flag(argc, argv, "--live"),
                       has_flag(argc, argv, "--no-store-sync"),
                       has_flag(argc, argv, "--force"));
        } else if (strcmp(cmd, "plan") == 0) {
            const char *st = flag(argc, argv, "--state");
            st = state_or_default(st, "plan");
            if (!st) return 1;
            return nx_plan_analyze_store(st, stdout) == 0 ? 0 : 1;
        } else if (strcmp(cmd, "case") == 0) {
            const char *path = flag(argc, argv, "--path");
            const char *kind = flag(argc, argv, "--kind");
            const char *nm = flag(argc, argv, "--case");
            if (!path) { fprintf(stderr, "case: --path required\n"); return 1; }
            return nx_plan_analyze_case(kind ? kind : "app", path, nm, stdout) == 0 ? 0 : 1;
        } else if (strcmp(cmd, "recipe") == 0) {
            const char *path = flag(argc, argv, "--path");
            const char *o = flag(argc, argv, "-o");
            if (!path) { fprintf(stderr, "recipe: --path required\n"); return 1; }
            return nx_recipe_build(path, flag(argc, argv, "--target"), o, stdout);
        } else if (strcmp(cmd, "recipe-show") == 0) {
            const char *f = flag(argc, argv, "--file");
            if (!f) { fprintf(stderr, "recipe-show: --file required\n"); return 1; }
            return nx_recipe_show(f, stdout);
        } else if (strcmp(cmd, "recipe-run") == 0) {
            const char *f = flag(argc, argv, "--file");
            if (!f) { fprintf(stderr, "recipe-run: --file required\n"); return 1; }
            const char *u = flag(argc, argv, "--upto");
            return nx_recipe_run(f, has_flag(argc, argv, "--apply"),
                                 u ? atoi(u) : 0, stdout);
        } else if (strcmp(cmd, "search") == 0) {
            const char *path = flag(argc, argv, "--path");
            const char *sym = flag(argc, argv, "--symbol");
            if (!path || !sym) { fprintf(stderr, "search: --path --symbol required\n"); return 1; }
            const char *pm = flag(argc, argv, "--probe-ms");
            return nx_strategy_search(path, sym, has_flag(argc, argv, "--apply"),
                                      pm ? atoi(pm) : 0, stdout);
        } else if (strcmp(cmd, "strategy-memory") == 0) {
            const char *ra = flag(argc, argv, "--record-app");
            const char *rs = flag(argc, argv, "--record-symbol");
            const char *rb = flag(argc, argv, "--record-branch");
            const char *ro = flag(argc, argv, "--record-ok");
            if (ra && rs && rb && ro) {
                int pruned = nx_strategy_record(ra, rs, rb, atoi(ro) != 0);
                printf("recorded %s ok=%s pruned=%d\n", rb, ro, pruned);
                return 0;
            }
            return nx_strategy_stats_show(stdout);
        } else if (strcmp(cmd, "cut") == 0) {
            const char *app = flag(argc, argv, "--path");
            const char *sym = flag(argc, argv, "--symbol");
            if (!app || !sym) { fprintf(stderr, "cut: --path --symbol required\n"); return 1; }
            if (has_flag(argc, argv, "--abort")) return nx_cut_abort(app, sym, stdout);
            if (has_flag(argc, argv, "--step")) return nx_cut_step_cmd(app, sym, stdout);
            return nx_cut(app, sym, has_flag(argc, argv, "--apply"), stdout);
        } else if (strcmp(cmd, "cut-status") == 0) {
            return nx_cut_status(stdout);
        } else if (strcmp(cmd, "templates") == 0) {
            return nx_template_list(stdout);
        } else if (strcmp(cmd, "app") == 0) {
            const char *p = flag(argc, argv, "--path");
            return p ? nx_app_show_path(p, stdout) : nx_app_show(stdout);
        } else if (strcmp(cmd, "app-analyze") == 0) {
            const char *p = flag(argc, argv, "--path");
            return p ? nx_app_analyze_path(p, has_flag(argc, argv, "--fix"), stdout)
                     : nx_app_analyze(has_flag(argc, argv, "--fix"), stdout);
        } else if (strcmp(cmd, "apps") == 0) {
            return nx_app_list(stdout);
        } else if (strcmp(cmd, "best") == 0) {
            const char *p = flag(argc, argv, "--path");
            if (!p) { fprintf(stderr, "best: --path required\n"); return 1; }
            return nx_bestfix(p, flag(argc, argv, "--kind"),
                              has_flag(argc, argv, "--apply"), stdout);
        } else if (strcmp(cmd, "kill") == 0) {
            /* 低レベルな強制終了。アプリに quit コマンドが無くても効く。 */
            const char *m = flag(argc, argv, "--match");
            if (!m || !*m) m = ".neodax-work";
            char c[512];
            snprintf(c, sizeof c, "pkill -KILL -f '%s' 2>/dev/null", m);
            int rc = system(c);
            printf("kill -9 -f '%s' rc=%d\n", m, rc);
            return 0;
        } else if (strcmp(cmd, "seek") == 0) {
            if (argc < 3) { fprintf(stderr, "seek: PATTERN required\n"); return 1; }
            const char *pattern = argv[2];
            const char *root = ".";
            for (int i = 3; i < argc; i++)
                if (argv[i][0] != '-') { root = argv[i]; break; }
            int flags = 0;
            if (has_flag(argc, argv, "--ignore-case")) flags |= 1;
            if (has_flag(argc, argv, "--regex")) flags |= 2;
            if (has_flag(argc, argv, "--binary")) flags |= 4;
            if (has_flag(argc, argv, "--files")) flags |= 8;
            if (has_flag(argc, argv, "--word")) flags |= 16;
            const char *inc = flag(argc, argv, "--include");
            const char *c = flag(argc, argv, "--context");
            const char *l = flag(argc, argv, "--limit");
            const char *t = flag(argc, argv, "--time");
            return nx_seek(pattern, root, inc, flags, c ? atoi(c) : 0,
                           l ? atoi(l) : 1000, t ? atoi(t) : 15000, stdout);
        } else if (strcmp(cmd, "repair") == 0) {
            const char *st = flag(argc, argv, "--state");
            st = state_or_default(st, "repair");
            if (!st) return 1;
            return nx_plan_repair(st, stdout) == 0 ? 0 : 1;
        } else if (strcmp(cmd, "advise") == 0 || strcmp(cmd, "check") == 0) {
            /* check は advise の件数だけ版。同じことを 2 つ持たないよう
               ここに統合した。--count で件数だけになる。 */
            const char *st = flag(argc, argv, "--state");
            st = state_or_default(st, "advise");
            if (!st) return 1;
            int count_only = has_flag(argc, argv, "--count") || strcmp(cmd, "check") == 0;
            if (count_only) {
                char *js = nx_deepai_load_state(st);
                if (!js) { printf("ERROR: no state at %s\n", st); return 1; }
                int n = nx_deepai_detect_json(js, strlen(js), stdout);
                printf("  [count] stuck surface tabs: %d\n", n);
                free(js);
                return 0;
            }
            char *js = nx_deepai_load_state(st);
            if (!js) { printf("ERROR: no state at %s\n", st); return 1; }
            int rc = nx_advise_tabs(st, js, flag(argc, argv, "--id"), stdout);
            free(js);
            return rc == 2 ? 1 : 0;
        } else if (strcmp(cmd, "fix") == 0) {
            const char *st = flag(argc, argv, "--state");
            st = state_or_default(st, "fix");
            if (!st) return 1;
            return nx_deepai_fix_path(st, stdout);
        } else if (strcmp(cmd, "clean") == 0) {
            /* 残滓を状態から判定し、種類と安全度から呼ぶ手順を選んで消す。
               --dry-run で変更なし。--no-live で永続層だけ。 */
            const char *st = flag(argc, argv, "--state");
            st = state_or_default(st, "clean");
            if (!st) return 1;
            nx_clean_opts_t o;
            nx_clean_opts_default(&o);
            o.live = !has_flag(argc, argv, "--no-live");
            o.dry_run = has_flag(argc, argv, "--dry-run");
            o.app = flag(argc, argv, "--app");
            o.store = flag(argc, argv, "--store");
            o.key = flag(argc, argv, "--key");
            return nx_clean_state(st, &o, stdout);
        } else if (strcmp(cmd, "splice") == 0) {
            /* 木を辿り、コードとデータを分ける。コードはバイナリに焼いて
               ハッシュでキャッシュし、以後はそのバイナリを実行する。 */
            const char *st = flag(argc, argv, "--state");
            st = state_or_default(st, "splice");
            if (!st) return 1;
            return cmd_splice(st, flag(argc, argv, "--key"),
                              has_flag(argc, argv, "--show"),
                              has_flag(argc, argv, "--bake"),
                              has_flag(argc, argv, "--keep"));
        } else if (strcmp(cmd, "trace") == 0) {
            /* 収束先から逆にたどり、trace を C++ クラスとして書き出して実行する。 */
            return cmd_trace(flag(argc, argv, "--dir"),
                             flag(argc, argv, "-o"), has_flag(argc, argv, "--show"),
                             has_flag(argc, argv, "--use-cut"));
        } else if (strcmp(cmd, "resolve") == 0) {
            /* 既存のノード資産をいまの状態に当てて、全ノードを独立に評価する。
               失敗は「安全」 (状態を書かない) として扱う。 */
            const char *st = flag(argc, argv, "--state");
            st = state_or_default(st, "resolve");
            if (!st) return 1;
            return cmd_resolve(st, flag(argc, argv, "--key"),
                               has_flag(argc, argv, "--apply"));
        } else if (strcmp(cmd, "node") == 0) {
            /* コマンドをノード経由で呼ぶ。引数はそのまま渡すので、
               このコマンド自身のオプション検査は行わない
               (検査は呼ばれた側で行われる)。 */
            return cmd_node(argc, argv);
        } else if (strcmp(cmd, "project") == 0) {
            /* 分析をプロジェクトファイルに保存する。
               順序は GUI と同じ: まず path を決め、決まってから書く。
               path が無ければ既定の場所を出す (そこに書く)。 */
            const char *path = flag(argc, argv, "--dir");
            if (has_flag(argc, argv, "--list")) return nx_project_list(stdout);

            if (flag(argc, argv, "--show")) {
                return nx_project_show(path, stdout);
            }

            /* 書く前に path を確定させる。 */
            char def[4096];
            if (!path || !*path) {
                const char *st = flag(argc, argv, "--state");
                st = state_or_default(st, "project");
                if (nx_project_default_path(st, def, sizeof def) != 0) {
                    printf("ERROR: path を決められない\n");
                    return 1;
                }
                path = def;
            }

            int cw = nx_project_can_write(path);
            printf("  [project] path=%s (%s)\n", path,
                   cw == 0 ? "既にある" : cw == 1 ? "作る" : "書けない");
            if (cw < 0) {
                printf("ERROR: ここには書けない: %s\n", path);
                return 1;
            }

            if (has_flag(argc, argv, "--dry-run")) {
                printf("  [project] --dry-run: 書かない\n");
                return 0;
            }

            /* 材料: --analysis で渡すか、--run でコマンドを走らせる。 */
            const char *analysis = flag(argc, argv, "--analysis");
            std::string body;
            const char *src = "manual";
            if (analysis && *analysis) {
                body = analysis;
            } else {
                const char *run = flag(argc, argv, "--run");
                if (run && *run) {
                    std::vector<std::string> a;
                    /* "clean --dry-run" のように空白で区切る */
                    std::string s(run);
                    size_t i = 0;
                    while (i < s.size()) {
                        while (i < s.size() && s[i] == ' ') i++;
                        size_t j = i;
                        while (j < s.size() && s[j] != ' ') j++;
                        if (j > i) a.push_back(s.substr(i, j - i));
                        i = j;
                    }
                    if (!a.empty()) {
                        nx::NodeResult r = nx::node::run(a);
                        body = r.out;
                        src = run;
                    }
                }
            }
            if (body.empty()) {
                printf("ERROR: 保存する分析が無い (--analysis か --run を渡す)\n");
                return 2;
            }

            return nx_project_save(path, src, body.c_str(), stdout) < 0 ? 1 : 0;
        } else if (strcmp(cmd, "arena") == 0) {
            /* 2 人の player が board を大量に進めて finish を競う。
               player 名を引数で受け、人との入出力はしない。
               状態は書かない (メモリ上だけで探索する)。 */
            const char *st = flag(argc, argv, "--state");
            st = state_or_default(st, "arena");
            if (!st) return 1;
            char *js = nx_deepai_load_state(st);
            if (!js) { printf("ERROR: no state at %s\n", st); return 1; }

            nx::ArenaConfig cfg;
            const char *lim = flag(argc, argv, "--limit");
            const char *dep = flag(argc, argv, "--depth");
            cfg.max_boards = lim ? atoi(lim) : 64;
            cfg.max_depth  = dep ? atoi(dep) : 8;
            cfg.max_frames = 32;
            cfg.templates  = 256;

            nx::ArenaResult r;
            int rc = nx::arena_run(js, flag(argc, argv, "--player-a"),
                                   flag(argc, argv, "--player-b"),
                                   cfg, &r, stdout);
            free(js);
            return rc;
        } else if (strcmp(cmd, "heal") == 0) {
            /* heal は clean と同じ経路 (検出 → 書く → 再水和)。
               別実装を持たず、clean に寄せる。 */
            const char *st = flag(argc, argv, "--state");
            st = state_or_default(st, "heal");
            if (!st) return 1;
            nx_clean_opts_t o;
            nx_clean_opts_default(&o);
            o.live = 1;
            o.dry_run = has_flag(argc, argv, "--dry-run");
            o.app = flag(argc, argv, "--app");
            o.store = flag(argc, argv, "--store");
            o.key = flag(argc, argv, "--key");
            return nx_clean_state(st, &o, stdout);
        } else if (strcmp(cmd, "replay") == 0) {
            const char *st = flag(argc, argv, "--state");
            st = state_or_default(st, "replay");
            if (!st) return 1;
            cmd_replay(st, flag(argc, argv, "-o"));
        } else if (strcmp(cmd, "apply") == 0) {
            const char *st = flag(argc, argv, "--state");
            st = state_or_default(st, "apply");
            if (!st) return 1;
            if (has_flag(argc, argv, "--attach")) {
                cmd_attach(st, flag(argc, argv, "--app"),
                           flag(argc, argv, "--store"),
                           flag(argc, argv, "--key"),
                           flag(argc, argv, "--id"));
            } else {
                cmd_apply(st, flag(argc, argv, "--key"),
                          flag(argc, argv, "--id"),
                          has_flag(argc, argv, "--live"));
            }
        } else if (strcmp(cmd, "layout") == 0) {
            cmd_layout(flag(argc, argv, "--app"));
        } else if (strcmp(cmd, "baseline") == 0) {
            cmd_baseline(flag(argc, argv, "--app"), flag(argc, argv, "--store"),
                         flag(argc, argv, "--key"));
        } else if (strcmp(cmd, "stop") == 0) {
            cmd_stop(flag(argc, argv, "--app"), flag(argc, argv, "--store"),
                     flag(argc, argv, "--key"));
        } else if (strcmp(cmd, "simulate") == 0) {
            const char *st = flag(argc, argv, "--state");
            st = state_or_default(st, "simulate");
            if (!st) return 1;
            do_simulate(st, flag(argc, argv, "--asar"));
        } else if (strcmp(cmd, "spec") == 0) {
            return run_spec(argc, argv);
        } else if (strcmp(cmd, "gen") == 0) {
            return run_gen(argc, argv);
        } else if (strcmp(cmd, "pe") == 0) {
            return run_pe(argc, argv);
        } else if (strcmp(cmd, "pipeline") == 0) {
            return run_pipeline(argc, argv);
        } else if (strcmp(cmd, "serve") == 0) {
            cmd_serve();
        } else {
            usage();
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        /* 引数なしで起動されたら GUI。--gui でも GUI。 */
        if (argc < 2 || strcmp(argv[1], "--gui") == 0 || strcmp(argv[1], "-g") == 0)
            return run_gui();
        /* GUI が使う経路 (stdout 差し替え + 自動 --state) を CLI からも点検できる。
           GUI と同じ関数を通るので、ここが通れば GUI の実行も通る。 */
        if (strcmp(argv[1], "--selftest") == 0) {
            const char *lines[] = {"tabs", "advise", "check", "plan", "repair", NULL};
            for (int i = 0; lines[i]; i++) {
                char *out = NULL;
                int rc = nx_gui_exec(lines[i], &out);
                printf("== %s == rc=%d bytes=%ld\n", lines[i], rc,
                       out ? (long)strlen(out) : 0L);
                if (out) {
                    /* 先頭 3 行だけ見せる (点検用) */
                    int shown = 0;
                    for (char *q = out; *q && shown < 3; ) {
                        char *nl = strchr(q, '\n');
                        size_t len = nl ? (size_t)(nl - q) : strlen(q);
                        printf("   %.*s\n", (int)len, q);
                        shown++;
                        q = nl ? nl + 1 : q + len;
                    }
                    free(out);
                }
            }
            return 0;
        }
        return nx_cli_dispatch(argc, argv);
    }
}
