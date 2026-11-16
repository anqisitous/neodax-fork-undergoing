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

/* neodax の分類をまとめて使う。個別ヘッダの列挙はビルドもコードも持たない:
 * どの分類を使うかは、この 1 行の include で決まる。 */
#include "integration/nx_all.h"

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
}

/* CLI の実体。GUI からも CLI からも同じこれを呼ぶ (二重実装をしない)。
 * 自作の @autoreleasepool は持たない — 呼び出し側が用意する。 */
static int nx_cli_dispatch(int argc, char **argv) {
    {
        if (argc < 2) { usage(); return 1; }
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
               ここで app の表示を置き換えない (解析用の一覧/SVG のみ)。 */
            nx_tpl_graph_t g;
            const char *dir = flag(argc, argv, "--dir");
            if (nx_tplcpp_scan(dir, &g) != 0) {
                printf("no templates (dir: %s)\n", dir ? dir : nx_tplcpp_default_dir());
                return 1;
            }
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
            if (!id) { fprintf(stderr, "delete: --id required\n"); return 1; }
            st = state_or_default(st, "delete");
            if (!st) return 1;
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
        } else if (strcmp(cmd, "advise") == 0) {
            const char *st = flag(argc, argv, "--state");
            st = state_or_default(st, "advise");
            if (!st) return 1;
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
        } else if (strcmp(cmd, "check") == 0) {
            const char *st = flag(argc, argv, "--state");
            st = state_or_default(st, "check");
            if (!st) return 1;
            cmd_check(st);
        } else if (strcmp(cmd, "heal") == 0) {
            const char *st = flag(argc, argv, "--state");
            st = state_or_default(st, "heal");
            if (!st) return 1;
            cmd_heal(st, flag(argc, argv, "--app"), flag(argc, argv, "--store"),
                     flag(argc, argv, "--key"), has_flag(argc, argv, "--dry-run"));
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
