/*
 * neodax-detect.c — 汎用キャプチャ可否プローブ (純 C)
 *
 * 旧 gui/neodax-detect.mm を .mm なしで書き直したもの。
 * CoreGraphics / ApplicationServices は C API なので Objective-C は不要。
 *
 * 検出する仕組み (3 層 + 権限):
 *   L0 権限      CGPreflightScreenCaptureAccess / AXIsProcessTrusted / TCC.db
 *   L1 シンボル  CGWindowListCreateImage が dlsym で引けるか
 *   L2 メタデータ CGWindowListCopyWindowInfo がウィンドウを返すか
 *   L3 ピクセル  画像が「実データ」を含むか (不透明率 + エッジ密度)
 *
 *   未承認でも L1/L2 は成功し、L3 だけが空になる。これが誤診の元。
 *
 * 使い方:
 *   neodax-detect [--json] [--app NAME] [--request]
 *
 * ビルド:
 *   cc -std=c11 -O2 neodax-detect.c -framework CoreGraphics \
 *      -framework ApplicationServices -o neodax-detect
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>

#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ApplicationServices/ApplicationServices.h>

typedef CGImageRef (*cgwl_create_fn)(CGRect, CGWindowListOption, CGWindowID,
                                     CGWindowImageOption);

static cgwl_create_fn load_cgwl(const char **where) {
    static cgwl_create_fn fn = NULL;
    static const char *src = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        void *h = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
                         RTLD_LAZY);
        if (h) { *(void **)(&fn) = dlsym(h, "CGWindowListCreateImage");
                 if (fn) src = "CoreGraphics"; }
        if (!fn) {
            void *h2 = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight",
                              RTLD_LAZY);
            if (h2) { *(void **)(&fn) = dlsym(h2, "CGWindowListCreateImage");
                      if (fn) src = "SkyLight"; }
        }
    }
    if (where) *where = src;
    return fn;
}

typedef struct { double opaque, edge, mean; int ok; } pixinfo_t;

static pixinfo_t pixel_stats(CGImageRef img) {
    pixinfo_t r = {0, 0, 0, 0};
    if (!img) return r;
    size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
    if (w < 2 || h < 2) return r;
    size_t sw = w > 96 ? 96 : w, sh = h > 96 ? 96 : h;
    unsigned char *buf = (unsigned char *)calloc(sw * sh * 4, 1);
    if (!buf) return r;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(buf, sw, sh, 8, sw * 4, cs,
                                             kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (!ctx) { free(buf); return r; }
    CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)sw, (CGFloat)sh), img);

    long opaque = 0, tot = 0;
    double edge = 0, lum = 0;
    for (size_t y = 1; y < sh; y++) {
        for (size_t x = 1; x < sw; x++) {
            const unsigned char *p = &buf[(y * sw + x) * 4];
            const unsigned char *q = &buf[(y * sw + x - 1) * 4];
            const unsigned char *u = &buf[((y - 1) * sw + x) * 4];
            int l1 = p[0] + p[1] + p[2], l2 = q[0] + q[1] + q[2], l3 = u[0] + u[1] + u[2];
            if (p[3] > 8) opaque++;
            edge += abs(l1 - l2) + abs(l1 - l3);
            lum += l1;
            tot++;
        }
    }
    CGContextRelease(ctx);
    free(buf);
    if (!tot) return r;
    r.opaque = (double)opaque / (double)tot;
    r.edge = edge / (double)tot / (3.0 * 255.0);
    r.mean = lum / (double)tot / (3.0 * 255.0);
    r.ok = (r.opaque > 0.35 && (r.edge > 0.002 || r.mean > 0.01)) ? 1 : 0;
    return r;
}

typedef struct {
    int id, pid, layer;
    double area;
    char owner[256], name[256];
} win_t;

static int collect(const char *app, win_t *out, int max) {
    CFArrayRef arr = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    if (!arr) return -1;
    int n = 0;
    for (CFIndex i = 0; i < CFArrayGetCount(arr) && n < max; i++) {
        CFDictionaryRef d = (CFDictionaryRef)CFArrayGetValueAtIndex(arr, i);
        CFStringRef owner = (CFStringRef)CFDictionaryGetValue(d, kCGWindowOwnerName);
        CFStringRef name = (CFStringRef)CFDictionaryGetValue(d, kCGWindowName);
        CFNumberRef num = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowNumber);
        CFNumberRef pid = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowOwnerPID);
        CFNumberRef lay = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowLayer);
        CFDictionaryRef b = (CFDictionaryRef)CFDictionaryGetValue(d, kCGWindowBounds);
        char ob[256] = {0}, nb[256] = {0};
        if (owner) CFStringGetCString(owner, ob, sizeof ob, kCFStringEncodingUTF8);
        if (name) CFStringGetCString(name, nb, sizeof nb, kCFStringEncodingUTF8);
        if (app && strstr(ob, app) == NULL) continue;
        win_t w;
        memset(&w, 0, sizeof w);
        if (num) CFNumberGetValue(num, kCFNumberIntType, &w.id);
        if (pid) CFNumberGetValue(pid, kCFNumberIntType, &w.pid);
        if (lay) CFNumberGetValue(lay, kCFNumberIntType, &w.layer);
        CGRect r = CGRectNull;
        if (b && CGRectMakeWithDictionaryRepresentation(b, &r))
            w.area = r.size.width * r.size.height;
        snprintf(w.owner, sizeof w.owner, "%s", ob);
        snprintf(w.name, sizeof w.name, "%s", nb);
        out[n++] = w;
    }
    CFRelease(arr);
    return n;
}

int main(int argc, char **argv) {
    const char *app = NULL;
    int json = 0, request = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--request")) request = 1;
        else if (!strcmp(argv[i], "--app") && i + 1 < argc) app = argv[++i];
    }

    /* L0: 権限 */
    int trusted = AXIsProcessTrusted() ? 1 : 0;
    void *cg = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY);
    typedef bool (*pf_fn)(void);
    pf_fn preflight = NULL, request_fn = NULL;
    if (cg) {
        *(void **)(&preflight) = dlsym(cg, "CGPreflightScreenCaptureAccess");
        *(void **)(&request_fn) = dlsym(cg, "CGRequestScreenCaptureAccess");
    }
    int screen_ok = preflight ? (preflight() ? 1 : 0) : -1;
    if (request && screen_ok == 0 && request_fn) {
        request_fn();
        screen_ok = preflight() ? 1 : 0;
    }

    /* L1: シンボル */
    const char *src = NULL;
    cgwl_create_fn create = load_cgwl(&src);

    /* L2: メタデータ */
    win_t wins[256];
    int nw = collect(app, wins, 256);

    /* L3: ピクセル */
    int tested = 0, good = 0;
    pixinfo_t best = {0, 0, 0, 0};
    if (create && nw > 0) {
        for (int i = 0; i < nw; i++) {
            if (wins[i].layer != 0 || wins[i].area < 20000) continue;
            CGImageRef img = create(CGRectNull, kCGWindowListOptionIncludingWindow,
                                    (CGWindowID)wins[i].id, kCGWindowImageBoundsIgnoreFraming);
            if (!img) continue;
            pixinfo_t p = pixel_stats(img);
            CGImageRelease(img);
            tested++;
            if (p.ok) good++;
            if (p.opaque > best.opaque) best = p;
        }
    }
    pixinfo_t full = {0, 0, 0, 0};
    int full_null = 1;
    if (create) {
        CGImageRef img = create(CGRectInfinite, kCGWindowListOptionOnScreenOnly,
                                kCGNullWindowID, kCGWindowImageBestResolution);
        if (img) { full_null = 0; full = pixel_stats(img); CGImageRelease(img); }
    }
    (void)tested;

    const char *verdict, *cause, *fix;
    if (screen_ok == 0) {
        verdict = "BLOCKED";
        cause = "kTCCServiceScreenCapture 未承認 - 画像は返るが中身が空/黒になる";
        fix = "システム設定 > プライバシーとセキュリティ > 画面収録 で許可 (または --request)";
    } else if (!create) {
        verdict = "BLOCKED";
        cause = "CGWindowListCreateImage のシンボルが解決できない (macOS 15+ で obsoleted)";
        fix = "SDK ヘッダ直呼びは不可。dlsym 迂回が必要";
    } else if (nw < 0) {
        verdict = "BLOCKED";
        cause = "CGWindowListCopyWindowInfo が NULL - ウィンドウサーバに接続できない";
        fix = "GUI セッション外 (SSH/launchd) で実行していないか確認";
    } else if (nw == 0) {
        verdict = "NO_TARGET";
        cause = "対象ウィンドウが0件 (アプリ未起動 / layer!=0)";
        fix = "対象アプリを起動する。--app の指定文字列を確認";
    } else if (tested == 0 && good == 1) {
        verdict = "BLOCKED";
        cause = "CGWindowListCreateImage のシンボルが解決できない (macOS 15+ で obsoleted)";
        fix = "SDK ヘッダ直呼びは不可。dlsym 迂回が必要";
    } else if (good > 0) {
        verdict = "OK";
        cause = "キャプチャ可 - 不透明な実データを含む画像が取れている";
        fix = NULL;
    } else if (!full_null && full.opaque > 0.35 &&
               (full.edge > 0.002 || full.mean > 0.01)) {
        verdict = "PARTIAL";
        cause = "個別ウィンドウは空だが画面全体は取れる (別 space / 最小化 / 保護サーフェス)";
        fix = "対象を前面に出すか、画面全体キャプチャにフォールバックする";
    } else {
        verdict = "BLOCKED";
        cause = "画像は返るが全て空 - TCC 承認はあるが実体が無い";
        fix = "アプリを前面化。それでも不可なら当該ウィンドウは保護されている";
    }

    if (json) {
        printf("{\n");
        printf("  \"verdict\": \"%s\",\n", verdict);
        printf("  \"cause\": \"%s\",\n", cause);
        printf("  \"l0_permissions\": { \"accessibility\": %s, \"screencapture\": %s },\n",
               trusted ? "true" : "false",
               screen_ok == 1 ? "true" : (screen_ok == 0 ? "false" : "null"));
        printf("  \"l1_symbol\": { \"resolved\": %s, \"source\": %s },\n",
               create ? "true" : "false", src ? src : "null");
        printf("  \"l2_metadata\": { \"windows\": %d, \"app_filter\": %s },\n",
               nw < 0 ? 0 : nw, app ? app : "null");
        printf("  \"l3_pixels\": { \"tested\": %d, \"with_data\": %d, \"best_opaque\": %.4f, \"full_opaque\": %.4f },\n",
               tested, good, best.opaque, full.opaque);
        printf("  \"fix\": %s\n", fix ? "true" : "false");
        printf("}\n");
    } else {
        printf("neodax-detect は、画面キャプチャが通るかどうかを調べる汎用のプローブです。\n\n");
        printf("権限については、Accessibility は%s、ScreenCapture は%s。\n",
               trusted ? "許可されています" : "許可されていません",
               screen_ok == 1 ? "許可されています"
                              : (screen_ok == 0 ? "許可されていません" : "判定できません"));
        if (src)
            printf("シンボルについては、CGWindowListCreateImage は %s (取得元は %s です)。\n",
                   create ? "解決できました" : "解決できませんでした", src);
        else
            printf("シンボルについては、CGWindowListCreateImage は %s。\n",
                   create ? "解決できました" : "解決できませんでした");
        printf("メタデータについては、ウィンドウを %d 件取得できました%s%s%s。\n",
               nw < 0 ? 0 : nw,
               app ? " (対象は " : "", app ? app : "", app ? ")" : "");
        printf("ピクセルについては、%d 件を検査して、実データがあったのは %d 件です "
               "(不透明率の最大は %.2f であり、画面全体は %.2f です)。\n",
               tested, good, best.opaque, full.opaque);
        if (app) {
            printf("\n対象のウィンドウは、次のとおりです。\n");
            for (int i = 0; i < nw && i < 20; i++)
                printf("  ウィンドウ %-8d は、レイヤが %-4d であり、面積は %-10.0f で、所有者は %s です。\n",
                       wins[i].id, wins[i].layer, wins[i].area, wins[i].owner);
        }
        printf("\n判定は %s です。\n原因は %s です。\n", verdict, cause);
        if (fix) printf("対処は %s です。\n", fix);
    }
    return verdict[0] == 'O' ? 0 : 1;
}
