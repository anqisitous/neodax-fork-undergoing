/* advisor.c — repo 内の「関数どうしの関係」を解析し、書くべき定義を出す
 *
 * クラス関係ではなく、関数単位の依存と合成を見る。
 *
 *   ./bin/neodax-advisor advise [DIR]        未定義のまま参照されている関数を列挙
 *   ./bin/neodax-advisor deps NAME            NAME が合成している関数 (推移閉包)
 *   ./bin/neodax-advisor uses NAME            NAME を合成している側
 *   ./bin/neodax-advisor plan NAME            NAME を実装するときの材料と雛形
 *   ./bin/neodax-advisor graph                依存辺を TSV で出す
 *   ./bin/neodax-advisor funcs [DIR]          定義済み関数の一覧
 *
 * 対象は C 系 (.c .cc .cpp .cxx .h .hh .hpp .hxx .mm .java)。
 * 依存は libc のみ。整形の検査は style サブコマンドから chained_style を使う。
 */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "chained_style.h"

#define MAX_NAME 128

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("advisor: out of memory\n", stderr); exit(2); }
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fputs("advisor: out of memory\n", stderr); exit(2); }
    return q;
}

static char *xstrndup(const char *s, int n) {
    char *p = xmalloc((size_t)n + 1);
    memcpy(p, s, (size_t)n);
    p[n] = 0;
    return p;
}

/* ================================================================= */
/* 字句                                                               */
/* ================================================================= */
enum { K_IDENT, K_PUNCT };

typedef struct {
    int kind;
    int off;   /* 元テキスト内のバイト位置 */
    int len;
    int line;
    char ch;   /* K_PUNCT のときの文字 */
} Tk;

typedef struct {
    Tk *v;
    int n, cap;
    const char *src;
    int srclen;
} Tks;

static void tk_push(Tks *t, int kind, int off, int len, int line, char ch) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 4096;
        t->v = xrealloc(t->v, (size_t)t->cap * sizeof *t->v);
    }
    Tk *k = &t->v[t->n++];
    k->kind = kind; k->off = off; k->len = len; k->line = line; k->ch = ch;
}

static int ident_start(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}
static int ident_cont(unsigned char c) {
    return ident_start(c) || (c >= '0' && c <= '9');
}

/* コメント・文字列・文字リテラル・プリプロセッサ行を落として識別子だけ拾う。 */
static void lex(const char *s, int n, Tks *t) {
    int i = 0, line = 1;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];

        if (c == '\n') { line++; i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }

        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) {
                if (s[i] == '\n') line++;
                i++;
            }
            i = (i + 1 < n) ? i + 2 : n;
            continue;
        }
        /* 行頭 '#' の行は丸ごと落とす (マクロは追わない) */
        if (c == '#' && (i == 0 || s[i - 1] == '\n')) {
            while (i < n && s[i] != '\n') {
                if (s[i] == '\\' && i + 1 < n && s[i + 1] == '\n') { line++; i += 2; continue; }
                i++;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            int q = c;
            i++;
            while (i < n) {
                if (s[i] == '\\') { i += 2; continue; }
                if (s[i] == q) { i++; break; }
                if (s[i] == '\n') { line++; break; }
                i++;
            }
            continue;
        }
        if (ident_start(c)) {
            int j = i + 1;
            while (j < n && ident_cont((unsigned char)s[j])) j++;
            tk_push(t, K_IDENT, i, j - i, line, 0);
            i = j;
            continue;
        }
        tk_push(t, K_PUNCT, i, 1, line, (char)c);
        i++;
    }
}

/* ================================================================= */
/* 名前 → 関数の表 (オープンアドレッシング)                           */
/* ================================================================= */
typedef struct {
    char *name;
    char *file;
    int line;      /* 定義行 (0 = 未定義) */
    int end_line;
    int nargs;     /* 定義または呼び出しで観測した最大引数数 */
    int *callees;  /* 定義本体から呼ぶ関数の添字 */
    int ncallees, capcallees;
    int *callers;
    int ncallers, capcallers;
    int refs;      /* 呼び出し箇所の数 */
    int first_ref_file;  /* 最初の参照 (ファイル表の添字) */
    int first_ref_line;
} Func;

typedef struct {
    Func *v;
    int n, cap;
    int *slot;     /* 名前ハッシュ → 添字+1 */
    int nslot;
} Funcs;

typedef struct {
    char **v;
    int n, cap;
} Files;

static unsigned hash_name(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

static int file_id(Files *f, const char *path) {
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->v[i], path)) return i;
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 64;
        f->v = xrealloc(f->v, (size_t)f->cap * sizeof *f->v);
    }
    f->v[f->n] = xstrndup(path, (int)strlen(path));
    return f->n++;
}

static void fs_grow(Funcs *fs);

static int fs_intern(Funcs *fs, const char *name, int len) {
    char buf[MAX_NAME];
    if (len >= MAX_NAME) len = MAX_NAME - 1;
    memcpy(buf, name, (size_t)len);
    buf[len] = 0;

    if (fs->nslot == 0) {
        fs->nslot = 1024;
        fs->slot = xmalloc((size_t)fs->nslot * sizeof *fs->slot);
        memset(fs->slot, 0, (size_t)fs->nslot * sizeof *fs->slot);
    }

    unsigned h = hash_name(buf) % (unsigned)fs->nslot;
    for (;;) {
        int s = fs->slot[h];
        if (s == 0) break;
        if (!strcmp(fs->v[s - 1].name, buf)) return s - 1;
        h = (h + 1) % (unsigned)fs->nslot;
    }

    fs_grow(fs);
    /* 伸長したらハッシュを作り直す */
    free(fs->slot);
    fs->nslot = 1024;
    while (fs->nslot < fs->n * 4) fs->nslot *= 2;
    fs->slot = xmalloc((size_t)fs->nslot * sizeof *fs->slot);
    memset(fs->slot, 0, (size_t)fs->nslot * sizeof *fs->slot);
    for (int i = 0; i < fs->n; i++) {
        unsigned g = hash_name(fs->v[i].name) % (unsigned)fs->nslot;
        while (fs->slot[g]) g = (g + 1) % (unsigned)fs->nslot;
        fs->slot[g] = i + 1;
    }

    h = hash_name(buf) % (unsigned)fs->nslot;
    while (fs->slot[h]) h = (h + 1) % (unsigned)fs->nslot;
    int id = fs->n++;
    fs->slot[h] = id + 1;
    return id;
}

static void fs_grow(Funcs *fs) {
    if (fs->n < fs->cap) return;
    fs->cap = fs->cap ? fs->cap * 2 : 256;
    fs->v = xrealloc(fs->v, (size_t)fs->cap * sizeof *fs->v);
    memset(&fs->v[fs->n], 0, (size_t)(fs->cap - fs->n) * sizeof *fs->v);
}

static Func *fs_find(Funcs *fs, const char *name) {
    if (!fs->nslot) return NULL;
    unsigned h = hash_name(name) % (unsigned)fs->nslot;
    for (;;) {
        int s = fs->slot[h];
        if (!s) return NULL;
        if (!strcmp(fs->v[s - 1].name, name)) return &fs->v[s - 1];
        h = (h + 1) % (unsigned)fs->nslot;
    }
}

static int fs_get(Funcs *fs, const char *name, int len) {
    char buf[MAX_NAME];
    if (len >= MAX_NAME) len = MAX_NAME - 1;
    memcpy(buf, name, (size_t)len);
    buf[len] = 0;
    Func *f = fs_find(fs, buf);
    if (f) return (int)(f - fs->v);
    int id = fs_intern(fs, name, len);
    fs->v[id].name = xstrndup(buf, len);
    return id;
}

static void add_edge(int **arr, int *n, int *cap, int v) {
    for (int i = 0; i < *n; i++)
        if ((*arr)[i] == v) return;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *arr = xrealloc(*arr, (size_t)*cap * sizeof **arr);
    }
    (*arr)[(*n)++] = v;
}

/* ================================================================= */
/* ファイル読み込みと走査                                             */
/* ================================================================= */
static int is_source(const char *path) { return nx_style_is_source(path); }

typedef struct { char *path; } Found;

typedef struct {
    Found *v;
    int n, cap;
} FoundList;

static void found_push(FoundList *l, const char *path) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 128;
        l->v = xrealloc(l->v, (size_t)l->cap * sizeof *l->v);
    }
    l->v[l->n++].path = xstrndup(path, (int)strlen(path));
}

static int skip_dir(const char *name) {
    static const char *bad[] = {".git", "build", "bin", "node_modules", ".cache",
                                "lib", NULL};
    for (int i = 0; bad[i]; i++) if (!strcmp(name, bad[i])) return 1;
    return 0;
}

static void walk(const char *dir, FoundList *out) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (skip_dir(e->d_name)) continue;
            walk(path, out);
        } else if (S_ISREG(st.st_mode) && is_source(path)) {
            found_push(out, path);
        }
    }
    closedir(d);
}

static int read_file(const char *path, char **out, int *out_n) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int cap = 1 << 16, n = 0;
    char *p = xmalloc((size_t)cap);
    size_t r;
    while ((r = fread(p + n, 1, (size_t)(cap - n - 1), f)) > 0) {
        n += (int)r;
        if (n + 1 >= cap) {
            cap *= 2;
            p = xrealloc(p, (size_t)cap);
        }
    }
    int bad = ferror(f);
    fclose(f);
    if (bad) { free(p); return -1; }
    p[n] = 0;
    *out = p;
    *out_n = n;
    return 0;
}

/* ================================================================= */
/* 解析                                                               */
/* ================================================================= */
static const char *kw[] = {"if", "for", "while", "switch", "return", "sizeof",
                           "do", "else", "case", "catch", "new", "delete", NULL};

static int is_kw(const char *s, int len) {
    for (int i = 0; kw[i]; i++)
        if ((int)strlen(kw[i]) == len && !memcmp(kw[i], s, (size_t)len)) return 1;
    return 0;
}

/* リポジトリの外にある関数 (libc / POSIX)。依存辺にしない。 */
static int is_external(const char *s, int len) {
    static const char *ext[] = {
        "printf", "fprintf", "sprintf", "snprintf", "vprintf", "vfprintf",
        "vsnprintf", "scanf", "fscanf", "sscanf", "puts", "fputs", "putchar",
        "fputc", "fgetc", "fgets", "fopen", "fdopen", "fclose", "fread",
        "fwrite", "fseek", "ftell", "rewind", "fflush", "feof", "ferror",
        "perror", "remove", "rename", "tmpfile", "setvbuf",
        "strlen", "strcmp", "strncmp", "strcasecmp", "strcpy", "strncpy",
        "strcat", "strncat", "strchr", "strrchr", "strstr", "strtok",
        "strtok_r", "strdup", "strndup", "strspn", "strcspn", "strpbrk",
        "memcpy", "memmove", "memset", "memcmp", "memchr", "strerror",
        "malloc", "calloc", "realloc", "free", "atoi", "atol", "atoll",
        "strtol", "strtoul", "strtoll", "strtoull", "strtod", "qsort",
        "bsearch", "rand", "srand", "abort", "exit", "atexit", "getenv",
        "setenv", "system", "abs", "labs", "isalpha", "isdigit", "isalnum",
        "isspace", "isupper", "islower", "isxdigit", "toupper", "tolower",
        "read", "write", "close", "open", "lseek", "unlink", "getcwd",
        "chdir", "access", "sleep", "usleep", "getpid", "time", "clock",
        "gettimeofday", "localtime", "gmtime", "strftime", "mktime",
        "sqrt", "pow", "fabs", "sin", "cos", "tan", "floor", "ceil",
        "pthread_create", "pthread_join", "pthread_mutex_lock",
        "pthread_mutex_unlock", "pthread_mutex_init", "pthread_mutex_destroy",
        NULL
    };
    for (int i = 0; ext[i]; i++)
        if ((int)strlen(ext[i]) == len && !memcmp(ext[i], s, (size_t)len)) return 1;
    return 0;
}

/* 大文字・数字・_ だけの名前はマクロ呼び出しとみなす (BITS(...) など)。 */
static int is_macro_name(const char *s, int len) {
    int upper = 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'a' && c <= 'z') return 0;
        if (c >= 'A' && c <= 'Z') upper = 1;
    }
    return upper;
}

/* メンバ呼び出し (obj.foo() / p->foo() / ns::foo()) は関数どうしの辺にしない。 */
static int is_member_call(const Tks *t, int name_tok) {
    if (name_tok >= 1 && t->v[name_tok - 1].kind == K_PUNCT &&
        (t->v[name_tok - 1].ch == '.' || t->v[name_tok - 1].ch == ':')) return 1;
    if (name_tok >= 2 && t->v[name_tok - 1].kind == K_PUNCT &&
        t->v[name_tok - 1].ch == '>' && t->v[name_tok - 2].kind == K_PUNCT &&
        t->v[name_tok - 2].ch == '-') return 1;
    return 0;
}

typedef struct {
    int start_tok, end_tok, func;
} Range;

typedef struct {
    Range *v;
    int n, cap;
} Ranges;

static void range_push(Ranges *r, int s, int e, int f) {
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 32;
        r->v = xrealloc(r->v, (size_t)r->cap * sizeof *r->v);
    }
    r->v[r->n].start_tok = s;
    r->v[r->n].end_tok = e;
    r->v[r->n].func = f;
    r->n++;
}

typedef struct {
    Funcs funcs;
    Files files;
} Repo;

/* 対応する閉じ括弧を探す。見つからなければ -1。 */
static int match_paren(const Tks *t, int open, char o, char c) {
    int depth = 0;
    for (int i = open; i < t->n; i++) {
        if (t->v[i].kind != K_PUNCT) continue;
        if (t->v[i].ch == o) depth++;
        else if (t->v[i].ch == c) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

static int count_args(const Tks *t, int open, int close) {
    if (close <= open + 1) return 0;
    int depth = 0, n = 1;
    for (int i = open + 1; i < close; i++) {
        if (t->v[i].kind != K_PUNCT) continue;
        char c = t->v[i].ch;
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
        else if (c == ',' && depth == 0) n++;
    }
    return n;
}

static void parse_file(Repo *r, const char *path) {
    char *src = NULL;
    int sn = 0;
    if (read_file(path, &src, &sn) != 0) return;

    Tks t = {0};
    t.src = src;
    t.srclen = sn;
    lex(src, sn, &t);

    int fid = file_id(&r->files, path);
    Ranges ranges = {0};

    /* 第 1 段: 定義を拾う */
    for (int i = 0; i + 1 < t.n; i++) {
        if (t.v[i].kind != K_PUNCT || t.v[i].ch != '(') continue;
        int p = i - 1;
        if (p < 0 || t.v[p].kind != K_IDENT) continue;
        if (is_kw(src + t.v[p].off, t.v[p].len)) continue;

        int close = match_paren(&t, i, '(', ')');
        if (close < 0) continue;
        if (close + 1 >= t.n) continue;
        if (t.v[close + 1].kind != K_PUNCT || t.v[close + 1].ch != '{') continue;

        const char *nm = src + t.v[p].off;
        int nl = t.v[p].len;
        if (is_kw(nm, nl) || is_external(nm, nl) || is_macro_name(nm, nl)) continue;
        if (nm[0] == '_' && nl > 1 && nm[1] == '_') continue;

        int body_close = match_paren(&t, close + 1, '{', '}');
        if (body_close < 0) continue;

        int id = fs_get(&r->funcs, nm, nl);
        Func *f = &r->funcs.v[id];
        if (f->line == 0) {                 /* 最初の定義を採る */
            f->file = r->files.v[fid];
            f->line = t.v[p].line;
            f->end_line = t.v[body_close].line;
            f->nargs = count_args(&t, i, close);
            range_push(&ranges, close + 1, body_close, id);
        }
        i = body_close;
    }

    /* 第 2 段: 呼び出しを拾い、囲んでいる定義に辺を張る */
    for (int i = 0; i + 1 < t.n; i++) {
        if (t.v[i].kind != K_PUNCT || t.v[i].ch != '(') continue;
        int p = i - 1;
        if (p < 0 || t.v[p].kind != K_IDENT) continue;
        if (is_kw(src + t.v[p].off, t.v[p].len)) continue;

        int close = match_paren(&t, i, '(', ')');
        if (close < 0) continue;

        if (is_kw(src + t.v[p].off, t.v[p].len)) continue;
        if (is_external(src + t.v[p].off, t.v[p].len)) continue;
        if (is_macro_name(src + t.v[p].off, t.v[p].len)) continue;
        if (is_member_call(&t, p)) continue;

        int id = fs_get(&r->funcs, src + t.v[p].off, t.v[p].len);
        Func *callee = &r->funcs.v[id];
        int nargs = count_args(&t, i, close);
        if (nargs > callee->nargs) callee->nargs = nargs;
        callee->refs++;
        if (callee->first_ref_line == 0) {
            callee->first_ref_file = fid;
            callee->first_ref_line = t.v[p].line;
        }

        for (int k = 0; k < ranges.n; k++) {
            if (i > ranges.v[k].start_tok && i < ranges.v[k].end_tok) {
                Func *caller = &r->funcs.v[ranges.v[k].func];
                add_edge(&caller->callees, &caller->ncallees, &caller->capcallees, id);
                add_edge(&callee->callers, &callee->ncallers, &callee->capcallers,
                         ranges.v[k].func);
                break;
            }
        }
    }

    free(ranges.v);
    free(t.v);
    free(src);
}

/* ================================================================= */
/* 出力                                                               */
/* ================================================================= */
static void print_loc(const Repo *r, int f) {
    const Func *x = &r->funcs.v[f];
    if (x->line > 0) printf("%s:%d", x->file, x->line);
    else if (x->first_ref_line > 0)
        printf("%s:%d (参照)", r->files.v[x->first_ref_file], x->first_ref_line);
    else printf("(未参照)");
}

static void cmd_advise(const Repo *r, int limit) {
    int defined = 0, missing = 0;
    for (int i = 0; i < r->funcs.n; i++) {
        if (r->funcs.v[i].line > 0) defined++;
        else if (r->funcs.v[i].refs > 0) missing++;
    }
    printf("functions\t%d\n", defined);
    printf("referenced_but_undefined\t%d\n", missing);
    printf("\n# 未定義のまま参照されている関数 (実装すべき候補)\n");
    printf("refs\targs\tlocation\tname\n");

    /* refs の多い順に (件数は高々数千なので選択ソートで足りる) */
    char *done = xmalloc((size_t)r->funcs.n + 1);
    memset(done, 0, (size_t)r->funcs.n + 1);
    int shown = 0;
    for (int k = 0; k < r->funcs.n; k++) {
        int best = -1;
        for (int i = 0; i < r->funcs.n; i++) {
            if (done[i]) continue;
            const Func *f = &r->funcs.v[i];
            if (f->line > 0 || f->refs == 0) continue;
            if (best < 0 || f->refs > r->funcs.v[best].refs) best = i;
        }
        if (best < 0) break;
        done[best] = 1;
        if (limit > 0 && shown >= limit) break;
        printf("%d\t%d\t", r->funcs.v[best].refs, r->funcs.v[best].nargs);
        print_loc(r, best);
        printf("\t%s\n", r->funcs.v[best].name);
        shown++;
    }
    free(done);
}

static void cmd_funcs(const Repo *r) {
    printf("line\targs\tdeps\tname\tfile\n");
    for (int i = 0; i < r->funcs.n; i++) {
        const Func *f = &r->funcs.v[i];
        if (f->line == 0) continue;
        printf("%d\t%d\t%d\t%s\t%s\n", f->line, f->nargs, f->ncallees, f->name, f->file);
    }
}

static void cmd_deps(const Repo *r, const char *name) {
    Func *f = fs_find((Funcs *)&r->funcs, name);
    if (!f) { fprintf(stderr, "advisor: 未知の関数 %s\n", name); return; }

    printf("%s\t", f->name);
    int id = (int)(f - r->funcs.v);
    print_loc(r, id);
    printf("\n# 合成している関数 (直接)\n");
    for (int i = 0; i < f->ncallees; i++) {
        printf("  -> ");
        print_loc(r, f->callees[i]);
        printf("\t%s\n", r->funcs.v[f->callees[i]].name);
    }

    /* 推移閉包 */
    char *seen = xmalloc((size_t)r->funcs.n + 1);
    memset(seen, 0, (size_t)r->funcs.n + 1);
    int *stack = xmalloc((size_t)(r->funcs.n + 1) * sizeof *stack);
    int sp = 0;
    for (int i = 0; i < f->ncallees; i++) stack[sp++] = f->callees[i];

    printf("\n# 依存ファイル (推移閉包)\n");
    while (sp > 0) {
        int v = stack[--sp];
        if (seen[v]) continue;
        seen[v] = 1;
        const Func *g = &r->funcs.v[v];
        printf("  %s", g->file ? g->file : "(未定義)");
        if (g->line > 0) printf(":%d", g->line);
        printf("\t%s\n", g->name);
        for (int i = 0; i < g->ncallees; i++)
            if (!seen[g->callees[i]]) stack[sp++] = g->callees[i];
    }
    free(stack);
    free(seen);
}

static void cmd_uses(const Repo *r, const char *name) {
    Func *f = fs_find((Funcs *)&r->funcs, name);
    if (!f) { fprintf(stderr, "advisor: 未知の関数 %s\n", name); return; }
    printf("%s\t", f->name);
    print_loc(r, (int)(f - r->funcs.v));
    printf("\n# 合成している側 (直接)\n");
    for (int i = 0; i < f->ncallers; i++) {
        printf("  <- ");
        print_loc(r, f->callers[i]);
        printf("\t%s\n", r->funcs.v[f->callers[i]].name);
    }
}

static void cmd_graph(const Repo *r) {
    printf("# from\tto\tlocation\n");
    for (int i = 0; i < r->funcs.n; i++) {
        const Func *f = &r->funcs.v[i];
        for (int k = 0; k < f->ncallees; k++) {
            const Func *g = &r->funcs.v[f->callees[k]];
            printf("%s\t%s\t%s", f->name, g->name, f->file ? f->file : "?");
            if (f->line > 0) printf(":%d", f->line);
            printf("\n");
        }
    }
}

static void cmd_plan(const Repo *r, const char *name) {
    Func *f = fs_find((Funcs *)&r->funcs, name);
    if (!f) {
        fprintf(stderr, "advisor: 未知の関数 %s (どこからも参照されていない)\n", name);
        return;
    }
    int id = (int)(f - r->funcs.v);

    if (f->line > 0) {
        printf("# 実装済み\n");
        printf("%s\t", f->name);
        print_loc(r, id);
        printf("\t%d 行目まで\n", f->end_line);
        if (f->ncallees == 0) printf("# 他の関数に依存しない\n");
        else {
            printf("# この関数が使うもの (これだけ読めばよい)\n");
            for (int i = 0; i < f->ncallees; i++) {
                printf("  %s\t", r->funcs.v[f->callees[i]].name);
                print_loc(r, f->callees[i]);
                printf("\n");
            }
        }
        return;
    }

    /* 未定義 — 書くべき定義を出す */
    printf("# 未定義: %s\n", f->name);
    printf("# 参照 %d 箇所 / 呼び出し時の最大引数 %d\n", f->refs, f->nargs);

    /* 参照元のファイルを集める (どこに置くべきかの判断材料) */
    printf("# 参照元\n");
    printf("  ");
    print_loc(r, id);
    printf("\n");

    /* そのファイルで使える近傍の関数 = そのファイル内の定義 */
    printf("# 同じファイルで使える関数 (合成の候補)\n");
    const char *host = (f->first_ref_line > 0) ? r->files.v[f->first_ref_file] : NULL;
    int shown = 0;
    if (host) {
        for (int i = 0; i < r->funcs.n; i++) {
            const Func *g = &r->funcs.v[i];
            if (g->line == 0 || !g->file || strcmp(g->file, host) != 0) continue;
            printf("  %s\t%s:%d\n", g->name, g->file, g->line);
            if (++shown >= 20) break;
        }
    }
    if (shown == 0) printf("  (なし)\n");

    /* 雛形: 引数だけ呼び出し側から推定する */
    printf("\n# 書くべき定義 (雛形)\n");
    printf("/* %s */\n", f->name);
    printf("static int %s(", f->name);
    for (int i = 0; i < f->nargs; i++) printf("%sarg%d", i ? ", int " : "int ", i + 1);
    if (f->nargs == 0) printf("void");
    printf(") {\n    return 0;   /* TODO */\n}\n");
}

/* ================================================================= */
/* main                                                               */
/* ================================================================= */
static void usage(void) {
    fputs("usage: neodax-advisor advise [DIR]     未定義のまま参照されている関数\n"
          "       neodax-advisor funcs  [DIR]     定義済み関数の一覧\n"
          "       neodax-advisor deps   NAME      その関数が合成しているもの\n"
          "       neodax-advisor uses   NAME      その関数を合成している側\n"
          "       neodax-advisor plan   NAME      実装するときの材料と雛形\n"
          "       neodax-advisor graph            依存辺を TSV で出す\n", stderr);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    const char *cmd = argv[1];

    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help")) { usage(); return 0; }

    const char *dir = ".";
    const char *name = NULL;
    int wants_name = !strcmp(cmd, "deps") || !strcmp(cmd, "uses") || !strcmp(cmd, "plan");
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) { dir = argv[++i]; continue; }
        if (argv[i][0] == '-') continue;
        if (wants_name && !name) { name = argv[i]; continue; }
        dir = argv[i];
    }
    if (wants_name && !name) {
        usage();
        return 2;
    }

    Repo r = {{0}, {0}};
    FoundList fl = {0};
    walk(dir, &fl);
    for (int i = 0; i < fl.n; i++) parse_file(&r, fl.v[i].path);

    int rc = 0;
    if (!strcmp(cmd, "advise")) cmd_advise(&r, 40);
    else if (!strcmp(cmd, "funcs")) cmd_funcs(&r);
    else if (!strcmp(cmd, "deps")) cmd_deps(&r, name);
    else if (!strcmp(cmd, "uses")) cmd_uses(&r, name);
    else if (!strcmp(cmd, "plan")) cmd_plan(&r, name);
    else if (!strcmp(cmd, "graph")) cmd_graph(&r);
    else { usage(); rc = 2; }

    for (int i = 0; i < fl.n; i++) free(fl.v[i].path);
    free(fl.v);
    return rc;
}
