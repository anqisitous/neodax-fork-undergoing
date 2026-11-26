/* chained_style.c — 「}{連結スタイル」整形器 (C11, 依存なし)
 *
 * 既存の整形器 (clang-format / uncrustify) は使わない。この規格専用。
 *
 *   ネストを作らず、構文上の単位 (宣言・文) が連続している範囲を連結する。
 *   連結した行では、直前の単位の閉じ } の直後に次の単位のヘッダが空白なしで来る。
 *   最初の単位だけ普通に書く。最後の単位は本体を改行して桁 0 に落とす。
 *
 *       int  getPosition(){return position;
 *                         }int  getVelocity(){return velocity;
 *                         }int  getId(){
 *   return id;
 *       }
 *
 * 適用範囲 (既定はファイル全体):
 *   //[  …  //]   この中だけ整形しない (そのまま無視する)。
 *   マーカーが無いファイルは、整形結果全体を //[ … //] で囲む。
 *   --markers     従来どおりマーカー内だけを整形する。
 *
 * バイナリの扱い:
 *   入力は生バイトとして読む。NUL・BOM・CRLF・非 UTF-8 でも壊さない。
 *   改行は LF / CRLF を検出して保存 (--eol lf|crlf|keep)。
 *   --write は「読んだのと同じ inode に truncate して書き戻す」ので、
 *   パーミッション・所有者・ハードリンクが保たれ、途中で失敗しても
 *   一次的には元バイト列が残る (書けなければ rc=2)。
 *
 *   ./bin/neodax-style --check FILE...            未整形なら rc=1
 *   ./bin/neodax-style --diff  FILE...            差分を表示
 *   ./bin/neodax-style --write FILE...            その場で整形
 *   ./bin/neodax-style --check FILE... --markers  マーカー内だけを見る
 */
#define _POSIX_C_SOURCE 200809L
#include "chained_style.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ================================================================= */
/* 伸長バッファ (公開型は NxStyleBuf)                               */
/* ================================================================= */
typedef NxStyleBuf Buf;

void nx_style_free(Buf *b) {
    if (!b) return;
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fputs("chained_style: out of memory\n", stderr); exit(2); }
    return q;
}

static void buf_put(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + n + 1) c *= 2;
        b->p = xrealloc(b->p, c);
        b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void buf_ch(Buf *b, char c) { buf_put(b, &c, 1); }
static void buf_sp(Buf *b, size_t k) { while (k--) buf_ch(b, ' '); }

/* ================================================================= */
/* トークン                                                           */
/* ================================================================= */
enum { T_COMMENT, T_PP, T_PUNCT, T_WORD, T_STRING, T_CHAR };

typedef struct {
    int kind;
    int start, end; /* 元テキスト中のオフセット (end 排他) */
    char punct;     /* kind == T_PUNCT のときの文字 */
} Tok;

typedef struct {
    Tok *v;
    int n, cap;
} Toks;

static void tok_push(Toks *t, int kind, int s, int e, char p) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 256;
        t->v = xrealloc(t->v, (size_t)t->cap * sizeof *t->v);
    }
    t->v[t->n].kind = kind;
    t->v[t->n].start = s;
    t->v[t->n].end = e;
    t->v[t->n].punct = p;
    t->n++;
}

static int is_word_ch(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

/* コメント・文字列・文字リテラル・プリプロセッサ行を壊さずに切る。 */
static void tokenize(const char *s, int n, Toks *t) {
    int i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];

        if (c == (unsigned char)0x20 || c == (unsigned char)0x09 ||
            c == (unsigned char)0x0d || c == (unsigned char)0x0a) { i++; continue; }

        /* NUL 以降 (バイナリの後ろ) はコードとして扱わない。
         * ここで打ち切れば、残りは「最後のトークンの後ろ」として素通しになる。 */
        if (c == 0) break;

        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            int j = i;
            while (j < n && s[j] != '\n') j++;
            tok_push(t, T_COMMENT, i, j, 0);
            i = j;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            int j = i + 2;
            while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) j++;
            j = (j + 1 < n) ? j + 2 : n;
            tok_push(t, T_COMMENT, i, j, 0);
            i = j;
            continue;
        }

        /* 行頭 '#' — 行末まで (バックスラッシュ継続を含む) */
        if (c == '#' && (i == 0 || s[i - 1] == '\n')) {
            int j = i;
            for (;;) {
                int k = j;
                while (k < n && s[k] != '\n') k++;
                if (k < n && k > j && s[k - 1] == '\\') { j = k + 1; continue; }
                j = k;
                break;
            }
            tok_push(t, T_PP, i, j, 0);
            i = j;
            continue;
        }

        if (c == '"' || c == '\'') {
            int j = i + 1;
            while (j < n) {
                if (s[j] == '\\') { j += 2; continue; }
                if (s[j] == (char)c) { j++; break; }
                if (s[j] == '\n') break; /* 閉じ忘れ — 行で打ち切る */
                j++;
            }
            if (j > n) j = n;
            tok_push(t, c == '"' ? T_STRING : T_CHAR, i, j, 0);
            i = j;
            continue;
        }

        if (is_word_ch(c)) {
            int j = i + 1;
            while (j < n && is_word_ch((unsigned char)s[j])) j++;
            tok_push(t, T_WORD, i, j, 0);
            i = j;
            continue;
        }

        tok_push(t, T_PUNCT, i, i + 1, (char)c);
        i++;
    }
}

/* ================================================================= */
/* 構文単位                                                           */
/* ================================================================= */
enum { U_SIMPLE, U_BLOCK, U_COMMENT, U_PP };

typedef struct Unit {
    int kind;
    int start, end;
    int header_end; /* '{' の位置 (U_BLOCK) */
    int close;      /* '}' の位置 (U_BLOCK) */
    int tail;       /* else 節の後の '}' の位置 (>0 なら続きがある) */
    struct Unit *body;
    int nbody, capbody;
    int one_liner; /* 元テキストで 1 行に収まっている */
} Unit;

typedef struct {
    Unit *v;
    int n, cap;
} Units;

static Unit *unit_add(Units *u) {
    if (u->n == u->cap) {
        u->cap = u->cap ? u->cap * 2 : 64;
        u->v = xrealloc(u->v, (size_t)u->cap * sizeof *u->v);
    }
    memset(&u->v[u->n], 0, sizeof u->v[u->n]);
    return &u->v[u->n++];
}

static int parse_units(const char *s, const Tok *tk, int nt, int idx, Units *out);

static int parse_units(const char *s, const Tok *tk, int nt, int idx, Units *out) {
    while (idx < nt) {
        const Tok *t = &tk[idx];

        if (t->kind == T_PUNCT && t->punct == '}') break;

        if (t->kind == T_COMMENT || t->kind == T_PP) {
            Unit *u = unit_add(out);
            u->kind = t->kind == T_PP ? U_PP : U_COMMENT;
            u->start = t->start;
            u->end = t->end;
            idx++;
            continue;
        }

        int j = idx, done = 0;
        while (j < nt) {
            const Tok *tj = &tk[j];

            if (tj->kind == T_PUNCT && tj->punct == '}') break;

            if (tj->kind == T_PUNCT && tj->punct == '{') {
                int depth = 1, k = j + 1;
                while (k < nt && depth > 0) {
                    if (tk[k].kind == T_PUNCT && tk[k].punct == '{') depth++;
                    else if (tk[k].kind == T_PUNCT && tk[k].punct == '}') depth--;
                    k++;
                }
                if (depth != 0) { /* 閉じ無し — ここから先は触らない */
                    Unit *u = unit_add(out);
                    u->kind = U_PP;
                    u->start = tk[idx].start;
                    u->end = tk[nt - 1].end;
                    return nt;
                }
                int close = k - 1;
                int next = k;
                /* 先に個数を確保しておく。本体を parse_units で作る間に
                 * out->v が再確保されても、ここで指した要素は無効にならない。 */
                int self = out->n;
                Unit *u = unit_add(out);
                memset(u, 0, sizeof *u);
                u->kind = U_BLOCK;
                u->start = tk[idx].start;
                u->header_end = tk[j].start;
                u->close = tk[close].start;
                u->end = tk[close].end;
                u->tail = -1;   /* まだ else 節は無い */
                if (next < nt && tk[next].kind == T_PUNCT && tk[next].punct == ';') {
                    u->end = tk[next].end;
                    next++;
                }
                /* `} else {` / `} else if (x) {` / `} while (...);` は
                 * 直前のブロックの続き。別単位にすると else が孤立する。
                 * 続きは `tail` (閉じ '}' の位置) に覚え、間のテキストは
                 * そのまま持ち回る (レンダリングで中身を変えない)。 */
                for (;;) {
                    int p = next, after = -1, else_at = -1;
                    if (p < nt && tk[p].kind == T_WORD && tk[p].end - tk[p].start == 4 &&
                        !strncmp(s + tk[p].start, "else", 4)) {
                        int q = p + 1;
                        if (q < nt && tk[q].kind == T_WORD && tk[q].end - tk[q].start == 2 &&
                            !strncmp(s + tk[q].start, "if", 2)) q++;      /* else if */
                        while (q < nt && tk[q].kind == T_PUNCT && tk[q].punct != '{' &&
                               tk[q].punct != ';' && tk[q].punct != '}')
                            q++;                                  /* (条件) */
                        if (q < nt && tk[q].kind == T_PUNCT && tk[q].punct == '{') {
                            int depth2 = 1, m = q + 1;
                            while (m < nt && depth2 > 0) {
                                if (tk[m].kind == T_PUNCT && tk[m].punct == '{') depth2++;
                                else if (tk[m].kind == T_PUNCT && tk[m].punct == '}') depth2--;
                                m++;
                            }
                            if (depth2 == 0) { after = m; else_at = tk[p].start; }
                        }
                    } else if (p < nt && tk[p].kind == T_WORD && tk[p].end - tk[p].start == 5 &&
                               !strncmp(s + tk[p].start, "while", 5) && tk[idx].kind == T_WORD &&
                               tk[idx].end - tk[idx].start == 2 && !strncmp(s + tk[idx].start, "do", 2)) {
                        /* do { } while (...); */
                        int q = p + 1;
                        while (q < nt && tk[q].kind == T_PUNCT && tk[q].punct != ';' && tk[q].punct != '}')
                            q++;
                        if (q < nt && tk[q].kind == T_PUNCT && tk[q].punct == ';') after = q + 1;
                    }
                    if (after < 0) break;
                    if (else_at >= 0) u->tail = else_at;
                    u->end = tk[after - 1].end;
                    next = after;
                }
                {
                    int i, nl = 0;
                    for (i = u->start; i < u->end; i++)
                        if (s[i] == '\n') { nl = 1; break; }
                    u->one_liner = nl ? 0 : 1;
                }
                {
                    Units body = {0};
                    parse_units(s, tk, nt, j + 1, &body);
                    u = &out->v[self];
                    u->body = body.v;
                    u->nbody = body.n;
                }
                idx = next;
                done = 1;
                break;
            }

            if (tj->kind == T_PUNCT && tj->punct == ';') {
                Unit *u = unit_add(out);
                u->kind = U_SIMPLE;
                u->start = tk[idx].start;
                u->end = tj->end;
                idx = j + 1;
                done = 1;
                break;
            }

            j++;
        }

        if (!done) break;
    }
    return idx;
}

/* ================================================================= */
/* 描画                                                               */
/* ================================================================= */
#define INDENT 4

typedef struct {
    const char *base; /* ファイル全体の先頭 */
    const char *s;    /* 範囲の先頭 (base + a) */
    int n;
    Buf *out;
    int marker_mode; /* 1: //[//] 内だけ整形 */
} Ctx;

/* 前後の空白を落とし、内部の改行・連続空白を 1 個の空白に畳む。 */
static void put_clean(Buf *o, const char *s, int a, int b) {
    int wrote = 0;
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    for (int i = a; i < b; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!wrote) continue;
            if (o->n && o->p[o->n - 1] == ' ') continue;
            buf_ch(o, ' ');
            continue;
        }
        buf_ch(o, (char)c);
        wrote = 1;
    }
}

static void render_units(Ctx *c, const Unit *u, int n, int indent, int chain);
static void append_tail(Ctx *c, const Unit *u, int indent);
static void bounce_else_body(Ctx *c, const Unit *u, int open_at, int indent);

static void put_indent(Buf *o, int k) { buf_sp(o, (size_t)k); }

/* ヘッダ (先頭トークン〜'{' の手前) を 1 行に詰める。
 * 最後のトークンが '{' に接する形でも、予約語と識別子の間は空白 1 個を残す。
 * 見た目は "int  getX(){..." のように、ちょうど 1 個になる。 */
static void put_header(Buf *o, const char *base, const Unit *u) {
    put_clean(o, base, u->start, u->header_end);
}

static int header_len(const char *s, const Unit *u) {
    Buf tmp = {0};
    put_header(&tmp, s, u);
    int r = (int)tmp.n;
    free(tmp.p);
    return r;
}

static void put_inline_body(Buf *o, const char *s, const Unit *u) {
    put_clean(o, s, u->header_end + 1, u->close);
}

/* コメントは中身を変えない。ブロックコメントは
 * 2 行目以降の `*` の桁が揃っているので、その相対位置を保つ。 */
static void render_comment(Ctx *c, const Unit *u, int indent) {
    const char *s = c->s;
    int i = u->start;
    int multiline = 0;
    for (int k = u->start; k < u->end; k++) if (s[k] == '\n') { multiline = 1; break; }

    put_indent(c->out, indent);
    while (i < u->end) {
        if (s[i] == '\n') {
            buf_ch(c->out, '\n');
            i++;
            while (i < u->end && (s[i] == ' ' || s[i] == '\t')) i++;
            if (i < u->end) put_indent(c->out, indent);
            /* ブロックコメントなら `*` の前に元の空白 1 個を戻す */
            if (multiline && i < u->end && s[i] == '*') buf_ch(c->out, ' ');
            continue;
        }
        buf_ch(c->out, s[i]);
        i++;
    }
}

/* else 節 / do-while の続きがあるブロックを、節の連鎖として描く。
 * `} else {` は閉じ } と同じ行に置き、節の本体はそのブロックの
 * インデントで描く。続きのバイト列は元のテキストをそのまま使うので、
 * 中身を組み立て直さない。 */
static void render_block(Ctx *c, const Unit *u, int indent) {
    Buf *o = c->out;
    put_indent(o, indent);
    put_header(o, c->base, u);
    if (u->one_liner && u->nbody <= 1) {
        buf_ch(o, '{');
        put_inline_body(o, c->s, u);
        buf_ch(o, '}');
        append_tail(c, u, indent);
        return;
    }
    buf_ch(o, '{');
    buf_ch(o, '\n');
    render_units(c, u->body, u->nbody, indent + INDENT, 0);
    put_indent(o, indent);
    buf_ch(o, '}');
    append_tail(c, u, indent);
}

/* else 節 / do-while の続きを、閉じ '}' の後ろにそのまま戻す。 */
/* else 節を `} else {` の形で描く。{ の手前は 1 行に詰め、
 * 本体はそのブロックのインデントで描く。
 *   最後の節なら閉じ '}' を行頭に戻し、さらに else が続くなら続けて描く。 */
static void append_tail(Ctx *c, const Unit *u, int indent) {
    int pos = u->tail;
    if (pos < 0) return;

    /* `else` / `else if (...)` のヘッダ部 (最初の '{' まで) */
    int hdr_end = pos;
    int k = pos;
    while (k < u->end && c->base[k] != (char)0x7b && c->base[k] != (char)0x7d) k++;
    if (k < u->end && c->base[k] == (char)0x7b) hdr_end = k;

    Buf tmp = {0};
    put_clean(&tmp, c->base, pos, hdr_end);
    /* 閉じ `}` の直後に空白 1 個を置いて `} else {` にする。 */
    buf_ch(c->out, (char)0x20);
    buf_put(c->out, tmp.p ? tmp.p : "", tmp.n);
    buf_ch(c->out, (char)0x20);
    bounce_else_body(c, u, hdr_end, indent);
    free(tmp.p);
}

/* else 節の本体を描く。中身は単位分解しなおす (レンダリング中の現在位置に依存しない)。 */
static void bounce_else_body(Ctx *c, const Unit *u, int open_at, int indent) {
    const char *base = c->base;
    int depth = 1, k = open_at + 1;
    while (k < u->end && depth > 0) {
        if (base[k] == (char)0x7b) depth++;
        else if (base[k] == (char)0x7d) depth--;
        k++;
    }
    int close_at = k - 1;
    if (depth != 0) return;

    Toks t = {0};
    Units inner = {0};
    tokenize(base + open_at + 1, close_at - open_at - 1, &t);
    parse_units(base + open_at + 1, t.v, t.n, 0, &inner);

    /* else 側の本体も、前の本体と同じ規則で描く:
     * 元から 1 行なら同じ行に畳み、そうでなければ 1 単位ずつ改行。 */
    Ctx sub = *c;
    sub.base = base + open_at + 1;
    sub.s = base + open_at + 1;
    buf_ch(c->out, (char)0x7b);
    if (u->one_liner && inner.n <= 1) {
        Unit fake;
        memset(&fake, 0, sizeof fake);
        fake.header_end = -1;
        fake.close = close_at - open_at - 1;
        if (inner.n == 1) {
            put_clean(c->out, sub.base, inner.v[0].start, inner.v[0].end);
        }
        buf_ch(c->out, (char)0x7d);
    } else {
        buf_ch(c->out, '\n');
        render_units(&sub, inner.v, inner.n, indent + INDENT, u->one_liner ? 0 : 0);
        put_indent(c->out, indent);
        buf_ch(c->out, '}');
    }
    free(t.v);
    free(inner.v);
}

/* 連結対象: ブロック単位で、本体が単純文 1 個、かつ元が複数行。 */
static int chainable(const Unit *u) {
    return u->kind == U_BLOCK && !u->one_liner && u->nbody == 1 && u->body[0].kind == U_SIMPLE &&
           u->tail < 0;
}

/* 本体の描画。桁を depth==0 で 0 にするのは連結列の最後だけ。 */
static void render_body_of(Ctx *c, const Unit *u, int indent) {
    for (int i = 0; i < u->nbody; i++) {
        const Unit *b = &u->body[i];
        switch (b->kind) {
        case U_SIMPLE:  put_indent(c->out, indent); put_clean(c->out, c->s, b->start, b->end); break;
        case U_COMMENT: render_comment(c, b, indent); break;
        case U_PP:      buf_put(c->out, c->s + b->start, (size_t)(b->end - b->start)); break;
        default:        render_block(c, b, indent); break;
        }
        buf_ch(c->out, '\n');
    }
}

static void render_chain(Ctx *c, const Unit *g, int n, int indent) {
    Buf *o = c->out;
    int col = indent + header_len(c->base, &g[0]);

    put_indent(o, indent);
    put_header(o, c->base, &g[0]);
    buf_ch(o, '{');
    put_inline_body(o, c->s, &g[0]);
    buf_ch(o, '\n');

    for (int i = 1; i < n - 1; i++) {
        put_indent(o, col);
        buf_ch(o, '}');
        put_header(o, c->base, &g[i]);
        buf_ch(o, '{');
        put_inline_body(o, c->s, &g[i]);
        buf_ch(o, '\n');
    }

    /* 最後の単位だけ本体を改行し、桁 0 に落とす (ネストしない)。 */
    const Unit *last = &g[n - 1];
    put_indent(o, col);
    buf_ch(o, '}');
    put_header(o, c->base, last);
    buf_ch(o, '{');
    buf_ch(o, '\n');
    render_body_of(c, last, 0);
    put_indent(o, indent);
    buf_ch(o, '}');
    append_tail(c, last, indent);
}

static int blank_between(const char *s, int prev_end, int start) {
    int nl = 0;
    for (int i = prev_end; i < start; i++)
        if (s[i] == '\n') nl++;
    return nl >= 2;
}

static void render_units(Ctx *c, const Unit *u, int n, int indent, int chain) {
    int i = 0, prev_end = -1;
    Buf *o = c->out;

    while (i < n) {
        if (chain) {
            int j = i;
            while (j < n && chainable(&u[j])) j++;
            if (j - i >= 2) {
                if (prev_end >= 0 && blank_between(c->s, prev_end, u[i].start)) buf_ch(o, '\n');
                render_chain(c, &u[i], j - i, indent);
                prev_end = u[j - 1].end;
                buf_ch(o, '\n');
                i = j;
                continue;
            }
        }

        if (prev_end >= 0 && blank_between(c->s, prev_end, u[i].start)) {
            if (o->n && o->p[o->n - 1] == '\n' && (o->n < 2 || o->p[o->n - 2] != '\n'))
                buf_ch(o, '\n');
        }

        switch (u[i].kind) {
        case U_COMMENT: render_comment(c, &u[i], indent); break;
        case U_PP:      buf_put(o, c->s + u[i].start, (size_t)(u[i].end - u[i].start)); break;
        case U_SIMPLE:  put_indent(o, indent); put_clean(o, c->s, u[i].start, u[i].end); break;
        default:        render_block(c, &u[i], indent); break;
        }
        buf_ch(o, '\n');
        prev_end = u[i].end;
        i++;
    }
}

/* ================================================================= */
/* マーカー                                                           */
/* ================================================================= */
static int line_is(const char *s, int a, int b, const char *word) {
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return (int)(b - a) == (int)strlen(word) && !memcmp(s + a, word, (size_t)(b - a));
}

/* マーカーを 1 つ探す。無ければ 0。
 * 見つけたら *mark に行頭、*is_open に開きか閉じか、
 * *body_start に本文先頭 (開きなら次行頭) を入れる。 */
typedef struct { int mark, is_open, body_start; } NxMarker;

static int next_marker(const char *s, int n, int from, NxMarker *m) {
    int p, ls;
    for (p = from, ls = from; p <= n; p++) {
        if (p == n || s[p] == '\n') {
            if (line_is(s, ls, p, "//[")) {
                m->mark = ls; m->is_open = 1; m->body_start = (p < n) ? p + 1 : p;
                return 1;
            }
            if (line_is(s, ls, p, "//]")) {
                m->mark = ls; m->is_open = 0; m->body_start = ls;
                return 1;
            }
            ls = p + 1;
        }
    }
    return 0;
}

/* マーカー行の次 (行頭) へ進める。 */
static int after_marker(const char *s, int n, int mark) {
    while (mark < n && s[mark] != '\n') mark++;
    return (mark < n) ? mark + 1 : n;
}

/* ----------------------------------------------------------------- */
/* 部分化: ここから下は関数 1 つ = 1 概念。組み合わせるだけでモードが増える  */
/* ----------------------------------------------------------------- */

/* [1] 字句→単位→描画 (範囲を指定し、基準桁も指定する)。
 * 他のすべての関数はこの 1 つに落ちる。 */
void nx_style_span(const char *s, int a, int b, int indent, Buf *out) {
    Toks t = {0};
    Units u = {0};
    Ctx c;
    if (b <= a) return;
    tokenize(s + a, b - a, &t);
    parse_units(s + a, t.v, t.n, 0, &u);
    /* トークンのオフセットは範囲先頭基準。base も範囲先頭に合わせる。 */
    c.base = s + a;
    c.s = s + a;
    c.n = b - a;
    c.out = out;
    c.marker_mode = 0;
    render_units(&c, u.v, u.n, indent, 1);
    /* 最後のトークンの後ろ (NUL 終端や改行など) はバイトのまま返す。 */
    if (t.n > 0 && out->n > 0) {
        int tail = t.v[t.n - 1].end;
        if (tail < b - a) {
            int k = tail;
            while (k < b - a && (s[a + k] == (char)0x20 || s[a + k] == (char)0x09 ||
                                  s[a + k] == (char)0x0d || s[a + k] == (char)0x0a)) k++;
            if (k < b - a) {
                /* 直前が改行なら、先頭の改行 1 個は落として二重空白を避ける */
                while (tail < k && s[a + tail] == (char)0x0a) tail++;
                buf_put(out, s + a + tail, (size_t)(b - a - tail));
            }
        }
    }
    free(t.v);
    free(u.v);
}

/* [2] 範囲の先頭実トークンが行頭から何桁目にあるか (=-1 は空)。 */
int nx_style_indent_of(const char *s, int a, int b) {
    int ls = a;
    for (int i = a; i < b; i++) {
        if (s[i] == '\n') { ls = i + 1; continue; }
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\r') continue;
        /* 範囲が行の途中から始まっている場合 (a が行頭でない) は、
         * その行頭まで戻って桁を数える。前に置いたバイト列を無視しない。 */
        int ls2 = ls;
        while (ls2 > 0 && s[ls2 - 1] != (char)0x0a) ls2--;
        /* BOM は桁として数えない */
        if (ls2 == 0 && nx_style_skip_bom(s, i) > 0) ls2 = 3;
        return i - ls2;
    }
    return -1;
}

/* [3] 基準桁を自分で決める範囲整形 (先頭トークンの桁に合わせる)。 */
void nx_style_span_auto(const char *s, int a, int b, Buf *out) {
    int ind = nx_style_indent_of(s, a, b);
    nx_style_span(s, a, b, ind < 0 ? 0 : ind, out);
}

/* [4] 先頭 BOM を飛ばした本文開始位置。 */
int nx_style_skip_bom(const char *s, int n) {
    if (n >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF) return 3;
    return 0;
}

/* [5] マーカーの次の位置 (行頭)。 */

/* マーカーが 1 つでもあるか。無ければ整形時に付ける。 */
static int has_marker(const char *s, int n) {
    NxMarker m;
    return next_marker(s, n, 0, &m);
}

static int has_nonspace(const char *s, int a, int b) {
    for (int i = a; i < b; i++)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n') return 1;
    return 0;
}

/* [6] ファイル全体を整形する。
 * まだ //[ //] が無いファイルは、整形結果全体を //[ ... //] で囲む。
 * 既にマーカーがあるファイルは、二重に付けず中身だけ整形する。 */
void nx_style_all(const char *s, int n, Buf *out) {
    int a = nx_style_skip_bom(s, n);
    if (has_marker(s, n) || !has_nonspace(s, a, n)) {
        buf_put(out, s, (size_t)a);
        nx_style_span_auto(s, a, n, out);
        return;
    }
    buf_put(out, s, (size_t)a);
    buf_put(out, "//[\n", 4);
    nx_style_span_auto(s, a, n, out);
    if (out->n == 0 || out->p[out->n - 1] != '\n') buf_ch(out, '\n');
    buf_put(out, "//]\n", 4);
}

/* [7] マーカー内だけを整形する (従来モード)。外側はバイト単位で無改造。 */
void nx_style_markers(const char *s, int n, Buf *out) {
    int bom = nx_style_skip_bom(s, n);
    int copied = 0;  /* out に送済みの元位置 */
    int open_at = -1; /* マーカー内の本文開始位置 (-1 は外) */
    NxMarker m;

    buf_put(out, s, (size_t)bom);
    copied = bom;

    for (int from = bom; next_marker(s, n, from, &m); from = after_marker(s, n, m.mark)) {
        if (m.is_open && open_at < 0) {
            buf_put(out, s + copied, (size_t)(m.body_start - copied));
            open_at = m.body_start;
        } else if (!m.is_open && open_at >= 0) {
            nx_style_span_auto(s, open_at, m.mark, out);
            copied = m.mark;
            open_at = -1;
        }
    }
    if (open_at < 0) buf_put(out, s + copied, (size_t)((size_t)n - (size_t)copied));
    else nx_style_span_auto(s, open_at, n, out);
}

/* [8] マーカー内はそのまま残し、マーカー外だけを整形する。
 * マーカーが 1 つも無ければ nx_style_all と同じ結果になる。 */
void nx_style_outside(const char *s, int n, Buf *out) {
    /* マーカーが無いファイルは「整形してマーカーを付ける」に回す。 */
    if (!has_marker(s, n)) { nx_style_all(s, n, out); return; }
    int copied = nx_style_skip_bom(s, n); /* 整形区間の先頭 */
    int inside = 0;                      /* 1 = マーカー内 (無改造) */
    NxMarker m;

    for (int from = copied; next_marker(s, n, from, &m); from = after_marker(s, n, m.mark)) {
        if (m.is_open && !inside) {
            nx_style_span_auto(s, copied, m.mark, out); /* マーカー手前まで整形 */
            buf_put(out, s + m.mark, (size_t)(m.body_start - m.mark)); /* `//[` 行は無改造 */
            copied = m.body_start;
            inside = 1;
        } else if (!m.is_open && inside) {
            /* マーカー内は無改造。`//]` 行まで通す */
            buf_put(out, s + copied, (size_t)(after_marker(s, n, m.mark) - copied));
            copied = after_marker(s, n, m.mark);
            inside = 0;
        }
    }
    if (!inside) nx_style_span_auto(s, copied, n, out);
    else buf_put(out, s + copied, (size_t)((size_t)n - (size_t)copied));
}

/* [9] 2 つの出力を比べて同じバイト列か。差分判定と --check の共通部品。 */
int nx_style_same(const Buf *a, const Buf *b) {
    return a->n == b->n && (a->n == 0 || !memcmp(a->p, b->p, a->n));
}

/* ファイル I/O (実体は下) */
static int read_file(const char *path, Buf *b);
static int write_file(const char *path, const char *data, size_t n);

/* [10] 名前から整形関数を選ぶ。CLI はこれを 1 行呼ぶだけでよい。 */
int nx_style_run(const char *mode, const char *s, int n, Buf *out) {
    if (!mode || !strcmp(mode, "all"))      { nx_style_all(s, n, out);     return 0; }
    if (!strcmp(mode, "markers"))           { nx_style_markers(s, n, out); return 0; }
    if (!strcmp(mode, "outside"))           { nx_style_outside(s, n, out); return 0; }
    return -1;
}

/* [11] 改行コードを保存する。出力側は常に LF を出すので、
 * 入力が CRLF 中心なら LF を CRLF に戻す (バイトを壊さないため)。 */
static int mostly_crlf(const char *s, int n) {
    int lf = 0, crlf = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] != '\n') continue;
        lf++;
        if (i > 0 && s[i - 1] == '\r') crlf++;
    }
    return lf > 0 && crlf * 2 > lf;
}

static void lf_to_crlf(Buf *b) {
    Buf o = {0};
    for (size_t i = 0; i < b->n; i++) {
        if (b->p[i] == '\n' && (i == 0 || b->p[i - 1] != '\r')) buf_ch(&o, '\r');
        buf_ch(&o, b->p[i]);
    }
    nx_style_free(b);
    *b = o;
}

/* [12] ファイルを読んで整形する。
 * 入力は生バイトとして素通しするので、NUL・BOM・非 UTF-8 でも末尾を落とさない。 */
int nx_style_file(const char *path, const char *mode, Buf *out) {
    Buf in = {0};
    if (read_file(path, &in) != 0) { nx_style_free(&in); return -1; }
    int rc = nx_style_run(mode, in.p ? in.p : "", (int)in.n, out);
    if (rc == 0 && mostly_crlf(in.p ? in.p : "", (int)in.n)) lf_to_crlf(out);
    nx_style_free(&in);
    return rc;
}

/* [12] 整形結果を書き戻す (同じ inode に truncate して書く)。 */
int nx_style_save(const char *path, const Buf *b) {
    return write_file(path, b->p ? b->p : "", b->n);
}

/* ================================================================= */
/* ファイル I/O                                                       */
/* ================================================================= */
static int read_file(const char *path, Buf *b) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char tmp[65536];
    size_t r;
    while ((r = fread(tmp, 1, sizeof tmp, f)) > 0) buf_put(b, tmp, r);
    int bad = ferror(f);
    fclose(f);
    return bad ? -1 : 0;
}

/* 読んだのと同じ inode に書き戻す (権限・所有者・リンクを保つ)。 */
static int write_file(const char *path, const char *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (n && fwrite(data, 1, n, f) != n) { fclose(f); return -1; }
    if (fflush(f) != 0) { fclose(f); return -1; }
    if (fclose(f) != 0) return -1;
    return 0;
}

/* ================================================================= */
/* 対象判定                                                           */
/* ================================================================= */
/* 整形の対象にする拡張子か。シェル・Markdown・データは触らない
 * (C 系の `{ } ;` を前提にした規格なので、他言語に当てると壊れる)。 */
int nx_style_is_source(const char *path) {
    static const char *ext[] = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh",
                                ".hpp", ".hxx", ".mm", ".java", NULL};
    const char *dot = path ? strrchr(path, '.') : NULL;
    if (!dot) return 0;
    for (int i = 0; ext[i]; i++)
        if (!strcmp(dot, ext[i])) return 1;
    return 0;
}
