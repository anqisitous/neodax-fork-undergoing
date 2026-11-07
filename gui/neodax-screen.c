/*
 * neodax-screen.c — 画面 GUI を「キャプチャせずに」構造として渡す (純 C)
 *
 * 旧 gui/neodax-screen.mm を .mm なしで書き直したもの。
 * CGWindowListCopyWindowInfo は権限不要で、ピクセル以外の情報を全部返す。
 *
 * 使い方:
 *   neodax-screen                      全ウィンドウをテキストで
 *   neodax-screen --json               機械可読 JSON
 *   neodax-screen --svg FILE           レイアウトを SVG で書き出し
 *   neodax-screen --app NAME           対象を絞る
 *   neodax-screen --prompt             LLM に渡す要約
 *   neodax-screen --min-area N         面積フィルタ
 *
 * ビルド:
 *   cc -std=c11 -O2 neodax-screen.c -framework CoreGraphics -o neodax-screen
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>

typedef struct {
    int id, pid, layer, onscreen, mem, sharing;
    double alpha;
    double x, y, w, h;
    char owner[256], name[256];
} win_t;

static int collect(const char *app, win_t *out, int max) {
    CFArrayRef arr = CGWindowListCopyWindowInfo(
        kCGWindowListOptionAll | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!arr) return -1;
    int n = 0;
    for (CFIndex i = 0; i < CFArrayGetCount(arr) && n < max; i++) {
        CFDictionaryRef d = (CFDictionaryRef)CFArrayGetValueAtIndex(arr, i);
        CFStringRef owner = (CFStringRef)CFDictionaryGetValue(d, kCGWindowOwnerName);
        CFStringRef name = (CFStringRef)CFDictionaryGetValue(d, kCGWindowName);
        CFNumberRef num = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowNumber);
        CFNumberRef pid = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowOwnerPID);
        CFNumberRef lay = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowLayer);
        CFNumberRef mem = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowMemoryUsage);
        CFNumberRef shr = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowSharingState);
        CFNumberRef alp = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowAlpha);
        CFBooleanRef ons = (CFBooleanRef)CFDictionaryGetValue(d, kCGWindowIsOnscreen);
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
        if (mem) CFNumberGetValue(mem, kCFNumberIntType, &w.mem);
        if (shr) CFNumberGetValue(shr, kCFNumberIntType, &w.sharing);
        if (alp) CFNumberGetValue(alp, kCFNumberDoubleType, &w.alpha);
        w.onscreen = (ons && CFBooleanGetValue(ons)) ? 1 : 0;
        CGRect r = CGRectNull;
        if (b && CGRectMakeWithDictionaryRepresentation(b, &r)) {
            w.x = r.origin.x; w.y = r.origin.y; w.w = r.size.width; w.h = r.size.height;
        }
        snprintf(w.owner, sizeof w.owner, "%s", ob);
        snprintf(w.name, sizeof w.name, "%s", nb);
        out[n++] = w;
    }
    CFRelease(arr);
    return n;
}

static int cmp_z(const void *a, const void *b) {
    const win_t *x = (const win_t *)a, *y = (const win_t *)b;
    if (x->layer != y->layer) return x->layer < y->layer ? -1 : 1;
    return x->id - y->id;
}

static void screen_bounds(double *sw, double *sh) {
    CGDirectDisplayID d = CGMainDisplayID();
    *sw = (double)CGDisplayPixelsWide(d);
    *sh = (double)CGDisplayPixelsHigh(d);
    if (*sw < 1 || *sh < 1) { *sw = 1920; *sh = 1080; }
}

static void jesc(const char *s, char *o, size_t n) {
    size_t j = 0;
    for (size_t i = 0; s[i] && j + 7 < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { o[j++] = '\\'; o[j++] = (char)c; }
        else if (c < 0x20) { j += (size_t)snprintf(o + j, n - j, "\\u%04x", c); }
        else o[j++] = (char)c;
    }
    o[j] = 0;
}

int main(int argc, char **argv) {
    const char *app = NULL, *svgpath = NULL;
    int json = 0, prompt = 0;
    double minarea = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--prompt")) prompt = 1;
        else if (!strcmp(argv[i], "--app") && i + 1 < argc) app = argv[++i];
        else if (!strcmp(argv[i], "--svg") && i + 1 < argc) svgpath = argv[++i];
        else if (!strcmp(argv[i], "--min-area") && i + 1 < argc) minarea = atof(argv[++i]);
    }

    win_t wins[512];
    int n = collect(app, wins, 512);
    if (n < 0) { fprintf(stderr, "window server に接続できません\n"); return 2; }

    int m = 0;
    for (int i = 0; i < n; i++) {
        if (wins[i].w * wins[i].h < minarea) continue;
        if (wins[i].w < 1 || wins[i].h < 1) continue;
        wins[m++] = wins[i];
    }
    qsort(wins, m, sizeof(win_t), cmp_z);

    double SW, SH;
    screen_bounds(&SW, &SH);

    if (json) {
        printf("{\n  \"capture_used\": false,\n");
        printf("  \"screen\": { \"width\": %.0f, \"height\": %.0f },\n", SW, SH);
        printf("  \"window_count\": %d,\n  \"windows\": [\n", m);
        for (int i = 0; i < m; i++) {
            char eo[600], en[600];
            jesc(wins[i].owner, eo, sizeof eo);
            jesc(wins[i].name, en, sizeof en);
            printf("    { \"id\": %d, \"pid\": %d, \"owner\": \"%s\", \"title\": \"%s\", "
                   "\"layer\": %d, \"onscreen\": %s, \"alpha\": %.2f, \"memory\": %d, "
                   "\"sharing\": %d, \"x\": %.0f, \"y\": %.0f, \"w\": %.0f, \"h\": %.0f }%s\n",
                   wins[i].id, wins[i].pid, eo, en, wins[i].layer,
                   wins[i].onscreen ? "true" : "false", wins[i].alpha, wins[i].mem,
                   wins[i].sharing, wins[i].x, wins[i].y, wins[i].w, wins[i].h,
                   (i + 1 < m) ? "," : "");
        }
        printf("  ]\n}\n");
        return 0;
    }

    if (prompt) {
        printf("SCREEN_STRUCTURE_BEGIN\n");
        printf("capture_used=false   (TCC ScreenCapture 不要)\n");
        printf("screen=%.0fx%.0f  windows=%d  app=%s\n\n", SW, SH, m, app ? app : "(all)");
        printf("OWNER\tWINDOWS\tTOTAL_AREA\tTITLES\n");
        for (int i = 0; i < m; i++) {
            int seen = 0;
            for (int k = 0; k < i; k++)
                if (!strcmp(wins[k].owner, wins[i].owner)) { seen = 1; break; }
            if (seen) continue;
            int cnt = 0; double area = 0; char titles[512] = {0};
            for (int k = i; k < m; k++) {
                if (strcmp(wins[k].owner, wins[i].owner)) continue;
                cnt++; area += wins[k].w * wins[k].h;
                if (wins[k].name[0]) {
                    if (titles[0]) strncat(titles, " | ", sizeof titles - strlen(titles) - 1);
                    strncat(titles, wins[k].name, sizeof titles - strlen(titles) - 1);
                }
            }
            printf("%s\t%d\t%.0f\t%s\n", wins[i].owner, cnt, area, titles[0] ? titles : "(no title)");
        }
        printf("\nLAYOUT (layer asc = back to front)\n");
        printf("LAYER\tID\tX\tY\tW\tH\tOWNER\tTITLE\n");
        for (int i = 0; i < m; i++)
            printf("%d\t%d\t%.0f\t%.0f\t%.0f\t%.0f\t%s\t%s\n",
                   wins[i].layer, wins[i].id, wins[i].x, wins[i].y, wins[i].w, wins[i].h,
                   wins[i].owner, wins[i].name[0] ? wins[i].name : "");
        printf("SCREEN_STRUCTURE_END\n");
        return 0;
    }

    if (svgpath) {
        double minx = 0, miny = 0, maxx = SW, maxy = SH;
        for (int i = 0; i < m; i++) {
            if (wins[i].x < minx) minx = wins[i].x;
            if (wins[i].y < miny) miny = wins[i].y;
            if (wins[i].x + wins[i].w > maxx) maxx = wins[i].x + wins[i].w;
            if (wins[i].y + wins[i].h > maxy) maxy = wins[i].y + wins[i].h;
        }
        double pad = 24, vw = maxx - minx + pad * 2, vh = maxy - miny + pad * 2;
        FILE *f = fopen(svgpath, "w");
        if (!f) { fprintf(stderr, "書き込み不可: %s\n", svgpath); return 3; }
        fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
        fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %.0f %.0f\" "
                   "width=\"%.0f\" height=\"%.0f\" font-family=\"Menlo,monospace\">\n", vw, vh, vw, vh);
        fprintf(f, "<rect x=\"0\" y=\"0\" width=\"%.0f\" height=\"%.0f\" fill=\"#0d1117\"/>\n", vw, vh);
        fprintf(f, "<text x=\"12\" y=\"18\" fill=\"#8b949e\" font-size=\"11\">"
                   "NeoDAX structure view - no screen capture (TCC-free)  %d windows  %.0fx%.0f</text>\n",
                m, SW, SH);
        for (int i = 0; i < m; i++) {
            double X = wins[i].x - minx + pad, Y = wins[i].y - miny + pad;
            unsigned h = (unsigned)(wins[i].pid * 2654435761u) ^ (unsigned)(wins[i].layer * 40503u);
            double hue = (double)(h % 360);
            char fill[64];
            snprintf(fill, sizeof fill, "hsl(%.0f,55%%,38%%)", hue);
            double op = 0.30 + 0.55 * (wins[i].alpha > 0 ? wins[i].alpha : 1);
            fprintf(f, "<g><rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" "
                       "rx=\"4\" fill=\"%s\" fill-opacity=\"%.2f\" stroke=\"%s\" stroke-opacity=\"0.95\"/>\n",
                    X, Y, wins[i].w, wins[i].h, fill, op, fill);
            char eo[600];
            jesc(wins[i].owner, eo, sizeof eo);
            fprintf(f, "<text x=\"%.1f\" y=\"%.1f\" fill=\"#e6edf3\" font-size=\"11\" "
                       "font-weight=\"bold\">%s</text>\n", X + 6, Y + 15, eo);
            char en[600];
            jesc(wins[i].name, en, sizeof en);
            if (wins[i].name[0])
                fprintf(f, "<text x=\"%.1f\" y=\"%.1f\" fill=\"#c9d1d9\" font-size=\"10\">%s</text>\n",
                        X + 6, Y + 28, en);
            fprintf(f, "<text x=\"%.1f\" y=\"%.1f\" fill=\"#8b949e\" font-size=\"9\">"
                       "id=%d layer=%d pid=%d %.0fx%.0f</text>\n",
                    X + 6, Y + 40, wins[i].id, wins[i].layer, wins[i].pid, wins[i].w, wins[i].h);
            fprintf(f, "</g>\n");
        }
        fprintf(f, "</svg>\n");
        fclose(f);
        printf("%s\n", svgpath);
        return 0;
    }

    printf("これは NeoDAX の構造ビューです。画面キャプチャは使用しておらず、TCC の ScreenCapture 権限も不要です。\n");
    printf("画面は %.0fx%.0f であり、ウィンドウは %d 件です%s%s%s。\n\n",
           SW, SH, m, app ? " (対象は " : "", app ? app : "", app ? ")" : "");
    printf("%-11s %-6s %-8s %-8s %-8s %-8s %-22s %s\n",
           "LAYER", "ID", "X", "Y", "W", "H", "OWNER", "TITLE");
    for (int i = 0; i < m; i++)
        printf("%-11d %-6d %-8.0f %-8.0f %-8.0f %-8.0f %-22.22s %s\n",
               wins[i].layer, wins[i].id, wins[i].x, wins[i].y, wins[i].w, wins[i].h,
               wins[i].owner, wins[i].name);
    return 0;
}
