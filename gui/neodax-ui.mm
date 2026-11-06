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

#include "integration/nx_deepai.h"
#include "integration/nx_sim.h"
#include "integration/nx_state.h"
#include "integration/nx_leveldb.h"
#include "integration/attach/nx_appfix.h"
#include "integration/spec/nx_recog.h"
#include "integration/spec/nx_specs_builtin.h"
#include "integration/spec/nx_gen.h"

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
    double area;
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

        double area = 0;
        if (bnd) {
            CGRect r = CGRectNull;
            if (CGRectMakeWithDictionaryRepresentation(bnd, &r))
                area = r.size.width * r.size.height;
        }
        if (app && area < 20000) continue;  /* 小さすぎる補助ウィンドウ除外 */

        win_t w;
        memset(&w, 0, sizeof(w));
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

static void dump_tabs_path(const char *path) {
    char *state = nx_deepai_load_state(path);
    if (!state) { printf("ERROR: no state at %s\n", path); return; }
    printf("STATE_SOURCE\t%s\n", path);
    nx_deepai_dump_state(state, stdout);
    free(state);
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

/* 修復した state を LevelDB へ書き戻す。キーは neodax 自身が自動判別する。 */
static void cmd_apply(const char *state_path, const char *key_hint, int live) {
    /* 1) 現在のタブ状態を読み、stale な surface を除いた修復 JSON を得る */
    char *state = nx_deepai_load_state(state_path);
    if (!state) { printf("ERROR: no state at %s\n", state_path); return; }
    char *fixed = NULL;
    int nrem = 0;
    if (nx_deepai_fix_json(state, strlen(state), &fixed, &nrem, stdout) != 0) {
        printf("ERROR: cannot build fixed state\n");
        free(state);
        return;
    }
    printf("  [apply] stale surface tabs removed: %d\n", nrem);
    if (nrem == 0) { free(state); free(fixed); printf("  [apply] nothing to write\n"); return; }

    /* 2) 書き込み先のレベルDBディレクトリを決める */
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

    /* 3) 正準キーを自動判別 (key_hint を含む最新 user key) */
    if (!key_hint) key_hint = "workspace-tabs";
    size_t klen = 0;
    char *key = nx_state_find_key(dir, key_hint, &klen);
    if (!key) {
        printf("  [apply] key containing '%s' not found in %s\n", key_hint, dir);
        free(state); free(fixed);
        return;
    }
    printf("  [apply] discovered key (%zu bytes): ", klen);
    for (size_t i = 0; i < klen; i++) {
        unsigned char c = (unsigned char)key[i];
        if (c >= 32 && c < 127) putchar(c); else printf("\\x%02x", c);
    }
    printf("\n");

    /* 4) 既存最大 seq より大きい seq を選ぶ */
    size_t vlen = strlen(fixed);
    unsigned long long seq = 1000000000ULL; /* 通常の seq より十分大きい */
    int rc = live
        ? nx_leveldb_append_kv(dir, key, klen, (const unsigned char *)fixed, vlen,
                               seq, NULL, stdout)
        : nx_leveldb_write_kv(dir, key, klen, (const unsigned char *)fixed, vlen,
                              seq, "000999_nxfix.log", stdout);
    if (rc == 0) {
        printf("  [apply] done.%s\n", live ? " (live WAL append; reload/restart to view)"
                                            : " Reload DeepAI Code to see the tabs removed.");
    } else {
        printf("  [apply] write failed%s\n", live ? " (WAL not writable?)" : " (leveldb locked?)");
    }
    free(key);
    free(fixed);
    free(state);
}

static const char *flag(int argc, char **argv, const char *name);
static int has_flag(int argc, char **argv, const char *name);

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
                       const char *store, const char *key) {
    char *fixed = NULL;
    if (state_path) {
        char *state = nx_deepai_load_state(state_path);
        if (!state) { printf("ERROR: no state at %s\n", state_path); return; }
        int nrem = 0;
        if (nx_deepai_fix_json(state, strlen(state), &fixed, &nrem, stdout) == 0)
            printf("  [attach] fixed state built (stale surface removed: %d)\n", nrem);
        free(state);
    }
    nx_appfix_profile_t prof = *nx_appfix_default_profile();
    if (app) prof.app_name = app;
    if (store) prof.store_substr = store;
    if (key) prof.key = key;
    nx_appfix_apply(&prof, fixed, fixed ? strlen(fixed) : 0, stdout);
    free(fixed);
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
        } else if (strcmp(cmd, "replay") == 0) {
            cmd_replay(rest1, rest2);
        } else if (strcmp(cmd, "apply") == 0) {
            if (rest1) cmd_apply(rest1, rest2, 0);
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
            cmd_attach(NULL, rest1, rest2, NULL);
        } else {
            printf("ERROR unknown command: %s\n", cmd);
        }
        printf("END\n");
        fflush(stdout);
    }
    free(line);
}

static void usage(void) {
    printf(
        "neodax-ui — NeoDAX minimal CLI (GUI とは独立)\n"
        "  windows\n"
        "  capture --app NAME -o FILE.png\n"
        "  display [--app NAME] [-o FILE] [--state PATH]\n"
        "  tabs     --state PATH\n"
        "  close    --state PATH --id ID\n"
        "  fix      --state PATH\n"
        "  simulate --state PATH [--asar ASAR]\n"
        "  replay   --state PATH [-o FILE.png]   (capture 無しで画面を再現)\n"
        "  apply    --state PATH [--key HINT] [--live]  (キー自動判別で書き戻し; --live は起動中 WAL に追記)\n"
        "           --attach [--app NAME] [--store SUBSTR] [--key HINT]  (SIGUSR1+inspector で稼働中 renderer を再水和)\n"
        "  attach   [--state PATH] [--app NAME] [--store SUBSTR] [--key HINT]\n"
        "  spec     --hex \"0x81 0x05 ...\" | --file PATH [--ws]   (構造認識)\n"
        "  gen      --spec NAME|all [--class NAME] [-o FILE]        (C++ クラス生成)\n"
        "  serve\n");
}

int main(int argc, char **argv) {
    @autoreleasepool {
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
            const char *st = flag(argc, argv, "--state");
            if (!st) { fprintf(stderr, "tabs: --state required\n"); return 1; }
            dump_tabs_path(st);
        } else if (strcmp(cmd, "close") == 0) {
            const char *st = flag(argc, argv, "--state");
            const char *id = flag(argc, argv, "--id");
            if (!st || !id) { fprintf(stderr, "close: --state --id required\n"); return 1; }
            return nx_deepai_remove_tab_path(st, id, stdout);
        } else if (strcmp(cmd, "fix") == 0) {
            const char *st = flag(argc, argv, "--state");
            if (!st) { fprintf(stderr, "fix: --state required\n"); return 1; }
            return nx_deepai_fix_path(st, stdout);
        } else if (strcmp(cmd, "replay") == 0) {
            const char *st = flag(argc, argv, "--state");
            if (!st) { fprintf(stderr, "replay: --state required\n"); return 1; }
            cmd_replay(st, flag(argc, argv, "-o"));
        } else if (strcmp(cmd, "apply") == 0) {
            const char *st = flag(argc, argv, "--state");
            if (!st) { fprintf(stderr, "apply: --state required\n"); return 1; }
            if (has_flag(argc, argv, "--attach")) {
                cmd_attach(st, flag(argc, argv, "--app"),
                           flag(argc, argv, "--store"),
                           flag(argc, argv, "--key"));
            } else {
                cmd_apply(st, flag(argc, argv, "--key"),
                          has_flag(argc, argv, "--live"));
            }
        } else if (strcmp(cmd, "simulate") == 0) {
            const char *st = flag(argc, argv, "--state");
            if (!st) { fprintf(stderr, "simulate: --state required\n"); return 1; }
            do_simulate(st, flag(argc, argv, "--asar"));
        } else if (strcmp(cmd, "spec") == 0) {
            return run_spec(argc, argv);
        } else if (strcmp(cmd, "gen") == 0) {
            return run_gen(argc, argv);
        } else if (strcmp(cmd, "serve") == 0) {
            cmd_serve();
        } else {
            usage();
            return 1;
        }
    }
    return 0;
}
