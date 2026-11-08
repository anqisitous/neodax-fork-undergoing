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
#include "integration/spec/nx_pe.h"
#include "integration/spec/nx_pe_disasm.h"
#include "integration/spec/nx_pipeline.h"

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
    if (nx_deepai_remove_tab_json(state, strlen(state), tab_id, &fixed, &nrem,
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

static void usage(void) {
    printf(
        "neodax-ui — NeoDAX minimal CLI (GUI とは独立)\n"
        "  windows\n"
        "  capture --app NAME -o FILE.png\n"
        "  display [--app NAME] [-o FILE] [--state PATH]\n"
        "  tabs     --state PATH\n"
        "  close    --state PATH --id ID\n"
        "  case     --path PATH [--kind app|state|dir] [--case NAME]\n"
        "           案件ごとの自発解析。対象に合う案件を逐次に試し、問題と計画を出す\n"
        "  repair   --state PATH\n"
        "           1 つの永続手続き: 観測→計画→段順に実行→再観測 を健全になるまで繰り返す\n"
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
        "  fix      --state PATH                      タブ状態を修復\n"
        "  check    --state PATH                    閉じられない surface タブを検出 (変更なし)\n"
        "  heal     --state PATH [--app NAME] [--store SUBSTR] [--key HINT] [--dry-run]\n"
        "           検出→永続層へ書き戻し→稼働中ストア再水和を自動実行\n"
        "  simulate --state PATH [--asar ASAR]\n"
        "  replay   --state PATH [-o FILE.png]   (capture 無しで画面を再現)\n"
        "  apply    --state PATH [--id ID] [--key HINT] [--live]  (キー自動判別で書き戻し; --live は起動中 WAL に追記)\n"
        "           --id 指定時はそのタブだけ除去 (未指定は stale surface を除去)\n"
        "           --attach [--app NAME] [--store SUBSTR] [--key HINT] [--id ID]  (SIGUSR1+inspector で稼働中 renderer を再水和)\n"
        "  stop     [--app NAME] [--store SUBSTR] [--key PREFIX]  起動時の状態復元を止める (JS 差し込み)\n"
        "  baseline [--app NAME] [--store SUBSTR] [--key KEY]    復元先を空の妥当な状態に置き換える\n"
        "  layout   [--app NAME]                                 復元有無の判定用レイアウト署名 (キャプチャ不要)\n"
        "  attach   [--state PATH] [--app NAME] [--store SUBSTR] [--key HINT]\n"
        "  spec     --hex \"0x81 0x05 ...\" | --file PATH [--ws]   (構造認識)\n"
        "  gen      --spec NAME|all [--class NAME] [-o FILE]        (C++ クラス生成)\n"
        "  pe       --file FILE.exe [--disasm] [--limit N]          (Windows PE 解析/逆アセンブル)\n"
        "  pipeline --file FILE.exe [--strategy auto|seq|sim] [-o out.cpp]  (decode+encode 戦略選択)\n"
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
        } else if (strcmp(cmd, "delete") == 0) {
            const char *st = flag(argc, argv, "--state");
            const char *id = flag(argc, argv, "--id");
            if (!st) { fprintf(stderr, "delete: --state required\n"); return 1; }
            if (!id) { fprintf(stderr, "delete: --id required\n"); return 1; }
            cmd_delete(st, id, flag(argc, argv, "--app"),
                       flag(argc, argv, "--store"), flag(argc, argv, "--key"),
                       has_flag(argc, argv, "--live"),
                       has_flag(argc, argv, "--no-store-sync"),
                       has_flag(argc, argv, "--force"));
        } else if (strcmp(cmd, "plan") == 0) {
            const char *st = flag(argc, argv, "--state");
            if (!st) { fprintf(stderr, "plan: --state required\n"); return 1; }
            return nx_plan_analyze_store(st, stdout) == 0 ? 0 : 1;
        } else if (strcmp(cmd, "case") == 0) {
            const char *path = flag(argc, argv, "--path");
            const char *kind = flag(argc, argv, "--kind");
            const char *nm = flag(argc, argv, "--case");
            if (!path) { fprintf(stderr, "case: --path required\n"); return 1; }
            return nx_plan_analyze_case(kind ? kind : "app", path, nm, stdout) == 0 ? 0 : 1;
        } else if (strcmp(cmd, "repair") == 0) {
            const char *st = flag(argc, argv, "--state");
            if (!st) { fprintf(stderr, "repair: --state required\n"); return 1; }
            return nx_plan_repair(st, stdout) == 0 ? 0 : 1;
        } else if (strcmp(cmd, "advise") == 0) {
            const char *st = flag(argc, argv, "--state");
            if (!st) { fprintf(stderr, "advise: --state required\n"); return 1; }
            char *js = nx_deepai_load_state(st);
            if (!js) { printf("ERROR: no state at %s\n", st); return 1; }
            int rc = nx_advise_tabs(st, js, flag(argc, argv, "--id"), stdout);
            free(js);
            return rc == 2 ? 1 : 0;
        } else if (strcmp(cmd, "fix") == 0) {
            const char *st = flag(argc, argv, "--state");
            if (!st) { fprintf(stderr, "fix: --state or --app required\n"); return 1; }
            return nx_deepai_fix_path(st, stdout);
        } else if (strcmp(cmd, "check") == 0) {
            const char *st = flag(argc, argv, "--state");
            if (!st) { fprintf(stderr, "check: --state required\n"); return 1; }
            cmd_check(st);
        } else if (strcmp(cmd, "heal") == 0) {
            const char *st = flag(argc, argv, "--state");
            if (!st) { fprintf(stderr, "heal: --state required\n"); return 1; }
            cmd_heal(st, flag(argc, argv, "--app"), flag(argc, argv, "--store"),
                     flag(argc, argv, "--key"), has_flag(argc, argv, "--dry-run"));
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
            if (!st) { fprintf(stderr, "simulate: --state required\n"); return 1; }
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
