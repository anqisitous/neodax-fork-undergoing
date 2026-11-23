/* main と差分表示は CLI 専用 (bin/neodax-style)。
 * ライブラリ (src/lib/libnxc_core.a) には入れないので、ここに分けて置く。 */
#include "chained_style.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef NxStyleBuf Buf;
static void *xrealloc(void *p, size_t n){ void *q = realloc(p, n ? n : 1); if (!q) { fputs("chained_style: out of memory\n", stderr); exit(2); } return q; }

/* 行に切り、LCS (最長共通部分列) で差分を出す。
 * バッファは NUL を含んでも良いが、比較は NUL まで (行は NUL 終端)。 */
typedef struct { const char *p; int len; } Line;

static Line *split_lines(const char *s, int n, int *count) {
    int cap = 64, k = 0;
    Line *v = xrealloc(NULL, (size_t)cap * sizeof *v);
    int i = 0;
    while (i < n) {
        int j = i;
        while (j < n && s[j] != '\n') j++;
        if (k == cap) { cap *= 2; v = xrealloc(v, (size_t)cap * sizeof *v); }
        v[k].p = s + i;
        v[k].len = j - i;
        k++;
        i = j + 1;
    }
    *count = k;
    return v;
}

static int line_eq(const Line *a, const Line *b) {
    return a->len == b->len && (a->len == 0 || !memcmp(a->p, b->p, (size_t)a->len));
}

/* LCS 表を埋め、蹴り出しで編集列を出す。サイズは (n+1)*(m+1)。 */
static void diff_print(const char *path, const char *a, int an, const char *b, int bn) {
    int la_n = 0, lb_n = 0;
    Line *la = split_lines(a, an, &la_n);
    Line *lb = split_lines(b, bn, &lb_n);

    printf("--- %s\n+++ %s (formatted)\n", path, path);

    size_t w = (size_t)lb_n + 1;
    unsigned *dp = xrealloc(NULL, ((size_t)la_n + 1) * w * sizeof *dp);
    for (int i = la_n; i >= 0; i--) {
        for (int j = lb_n; j >= 0; j--) {
            if (i == la_n || j == lb_n) { dp[(size_t)i * w + j] = 0; continue; }
            if (line_eq(&la[i], &lb[j])) dp[(size_t)i * w + j] = dp[(size_t)(i + 1) * w + (j + 1)] + 1;
            else {
                unsigned d = dp[(size_t)(i + 1) * w + j];
                unsigned r = dp[(size_t)i * w + (j + 1)];
                dp[(size_t)i * w + j] = d > r ? d : r;
            }
        }
    }

    int i = 0, j = 0, hunk = 0;
    while (i < la_n || j < lb_n) {
        int si = i, sj = j;
        while (i < la_n && j < lb_n && line_eq(&la[i], &lb[j])) { i++; j++; }
        if (i == la_n && j == lb_n) break;

        /* 変更範囲: 一致が再開するまで */
        int fi = i, fj = j;
        while (fi < la_n || fj < lb_n) {
            if (fi < la_n && fj < lb_n && line_eq(&la[fi], &lb[fj])) break;
            if (fj >= lb_n || (fi < la_n && dp[(size_t)(fi + 1) * w + fj] >= dp[(size_t)fi * w + (fj + 1)])) fi++;
            else fj++;
        }
        printf("@@ -%d,%d +%d,%d @@\n", si + 1, fi - si, sj + 1, fj - sj);
        for (int k = si; k < fi; k++) {
            printf("- ");
            if (la[k].len) fwrite(la[k].p, 1, (size_t)la[k].len, stdout);
            putchar('\n');
        }
        for (int l = sj; l < fj; l++) {
            printf("+ ");
            if (lb[l].len) fwrite(lb[l].p, 1, (size_t)lb[l].len, stdout);
            putchar('\n');
        }
        i = fi; j = fj; hunk++;
    }
    (void)hunk;
    free(dp);
    free(la);
    free(lb);
}


/* ファイルを丸ごと読む (NUL を含んでも落とさない)。 */
static int read_whole(const char *path, Buf *b) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char tmp[65536];
    size_t r;
    while ((r = fread(tmp, 1, sizeof tmp, f)) > 0) {
        if (b->n + r + 1 > b->cap) {
            b->cap = (b->n + r + 1) * 2;
            b->p = xrealloc(b->p, b->cap);
        }
        memcpy(b->p + b->n, tmp, r);
        b->n += r;
    }
    int bad = ferror(f);
    fclose(f);
    return bad ? -1 : 0;
}

/* ================================================================= */
/* main                                                               */
/* ================================================================= */
static void usage(void) {
    fputs("usage: neodax-style [--check|--write|--diff] FILE...\n"
          "                    [--mode all|outside|markers]\n"
          "  --check  未整形なら rc=1 (書き込まない)\n"
          "  --write  その場で整形 (同じ inode に書き戻す)\n"
          "  --diff   差分を表示\n"
          "  --mode   整形の範囲 (既定 all)\n"
          "             all     : ファイル全体。//[ //] は無視して中も整形\n"
          "             outside : マーカー外だけ整形。//[ //] の中はそのまま\n"
          "             markers : マーカー内だけ整形 (従来モード)\n"
          "  --markers  --mode markers の別名\n"
          "  --outside  --mode outside の別名\n", stderr);
}

int main(int argc, char **argv) {
    int mode = 0;
    const char *style = "all";
    const char **files = xrealloc(NULL, sizeof(char *) * (size_t)(argc + 1));
    int nfiles = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check")) mode = 1;
        else if (!strcmp(argv[i], "--write")) mode = 2;
        else if (!strcmp(argv[i], "--diff")) mode = 3;
        else if (!strcmp(argv[i], "--markers")) style = "markers";
        else if (!strcmp(argv[i], "--outside")) style = "outside";
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) style = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else files[nfiles++] = argv[i];
    }
    int changed = 0, checked = 0;
    for (int f = 0; f < nfiles; f++) {
        struct stat st;
        if (stat(files[f], &st) != 0) { fprintf(stderr, "neodax-style: %s: %s\n", files[f], strerror(errno)); continue; }
        if (!S_ISREG(st.st_mode)) continue;
        /* 対象外の拡張子 (シェル・Markdown など) は黙って飛ばす。
           neodax-ui style と同じ判定を共有する。 */
        if (!nx_style_is_source(files[f])) continue;

        Buf in = {0}, out = {0};
        if (read_whole(files[f], &in) != 0) {
            fprintf(stderr, "neodax-style: %s: 読めません\n", files[f]);
            nx_style_free(&in);
            continue;
        }
        checked++;
        if (nx_style_file(files[f], style, &out) != 0) {
            fprintf(stderr, "neodax-style: %s: 整形できません (または不明な --mode %s)\n", files[f], style);
            nx_style_free(&in); nx_style_free(&out);
            return 2;
        }
        if (nx_style_same(&in, &out)) { nx_style_free(&in); nx_style_free(&out); continue; }
        changed++;

        if (mode == 2) {
            if (nx_style_save(files[f], &out) != 0) {
                fprintf(stderr, "neodax-style: %s: 書き込めません\n", files[f]);
                nx_style_free(&in); nx_style_free(&out);
                return 2;
            }
            printf("formatted: %s\n", files[f]);
        } else if (mode == 3) {
            diff_print(files[f], in.p ? in.p : "", (int)in.n, out.p ? out.p : "", (int)out.n);
        }
        nx_style_free(&in);
        nx_style_free(&out);
    }

    if (mode == 1) {
        if (changed) fprintf(stderr, "%d ファイルが未整形です。/ 検査 %d ファイル\n", changed, checked);
        else fprintf(stderr, "OK: %d ファイルすべて整形済み\n", checked);
        return changed ? 1 : 0;
    }
    if (mode == 3) fprintf(stderr, "%d ファイルに差分 / 検査 %d ファイル\n", changed, checked);
    free(files);
    return 0;
}
