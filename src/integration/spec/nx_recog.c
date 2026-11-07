#include "nx_recog.h"
#include "nx_spec.h"
#include "nx_specs_builtin.h"

#include <string.h>
#include <stdlib.h>

/* 1 つの spec がバイト列にどれだけ合致するか採点 (0..100) */
static int score_spec(const nx_spec_t *s, const uint8_t *d, size_t n, char *reason, size_t rsz) {
    if (!s || !d) return -1;
    int score = 0;
    reason[0] = 0;

    if (s->magic && s->magic_len) {
        if ((size_t)s->magic_offset + s->magic_len > n) return -1;
        if (memcmp(d + s->magic_offset, s->magic, s->magic_len) != 0) return -1;
        score += 60;
        snprintf(reason, rsz, "magic matched");
    }
    if (s->check) {
        if (!s->check(d, n)) return -1;
        score += 40;
        snprintf(reason, rsz, "%scheck passed", reason[0] ? "magic+check; " : "check only; ");
    }
    /* magic も check も無い spec は弱い候補 (構造の妥当性で加点) */
    if ((!s->magic || !s->magic_len) && !s->check) {
        score = 10;
        /* フィールドが読めるかだけ確認 */
        for (int i = 0; i < s->n_fields; i++) {
            const nx_field_t *f = &s->fields[i];
            if (f->kind == NX_F_ABS && (size_t)(f->offset + f->width) <= n) score += 2;
        }
        if (reason[0] == 0) snprintf(reason, rsz, "no magic; structural fit");
    }
    return score > 100 ? 100 : score;
}

int nx_recog_identify(const uint8_t *data, size_t size,
                      nx_recog_hit_t *hits, int max) {
    nx_specs_builtin_register();
    int n = 0;
    for (int i = 0; i < nx_spec_count() && n < max; i++) {
        const nx_spec_t *s = nx_spec_at(i);
        char reason[128];
        int sc = score_spec(s, data, size, reason, sizeof(reason));
        if (sc < 0) continue;
        hits[n].spec = s;
        hits[n].score = sc;
        snprintf(hits[n].reason, sizeof(hits[n].reason), "%s", reason);
        n++;
    }
    /* スコア降順 (単純挿入ソート) */
    for (int i = 1; i < n; i++) {
        nx_recog_hit_t k = hits[i];
        int j = i - 1;
        while (j >= 0 && hits[j].score < k.score) { hits[j + 1] = hits[j]; j--; }
        hits[j + 1] = k;
    }
    return n;
}

static void print_field(const uint8_t *d, size_t n, const nx_field_t *f, FILE *out) {
    if (f->kind == NX_F_LENEX) {
        uint64_t l = 0;
        /* 直前フィールドの値を使うのが本来だが、ここでは offset を長さとみなさない。
           呼び出し側で長さが決まる構造は nx_recog_ws_frames 等が個別処理する。 */
        fprintf(out, "    %-14s : <len-prefixed payload>", f->name);
        (void)l;
        return;
    }
    uint64_t v = nx_spec_read_int(d, n, f);
    fprintf(out, "    %-14s : 0x%llx (%llu)",
            f->name, (unsigned long long)v, (unsigned long long)v);
    const char *en = nx_spec_enum_name(f, v);
    if (en) fprintf(out, "  [%s]", en);
    fprintf(out, "\n");
}

int nx_recog_explain(const uint8_t *data, size_t size, FILE *out) {
    if (!out) out = stdout;
    nx_recog_hit_t hits[16];
    int n = nx_recog_identify(data, size, hits, 16);
    fprintf(out, "\n  === structure recognition (%zu bytes) ===\n", size);
    if (n == 0) { fprintf(out, "    no known structure matched\n"); return 0; }
    for (int i = 0; i < n; i++) {
        int weak = hits[i].score < 30;   /* magic も check も無い構造 = 参考扱い */
        fprintf(out, "    [%2d%%] %-20s family=%-16s %s%s\n",
                hits[i].score, hits[i].spec->name, hits[i].spec->family,
                hits[i].spec->summary, weak ? "  (weak)" : "");
        if (i == 0 && !weak) {
            const nx_spec_t *s = hits[i].spec;
            for (int k = 0; k < s->n_fields; k++)
                print_field(data, size, &s->fields[k], out);
        }
    }
    if (n > 0 && hits[0].score < 30)
        fprintf(out, "    note: no magic/check matched; field dump suppressed (avoid false positives)\n");
    return n;
}

/* ---- WebSocket フレーム列の分解 ---------------------------------- */

int nx_recog_ws_frames(const uint8_t *data, size_t size, FILE *out) {
    if (!out) out = stdout;
    nx_specs_builtin_register();
    const nx_spec_t *ws = nx_spec_by_name("ws_frame");
    if (!ws) { fprintf(out, "  ws_frame spec not registered\n"); return 0; }

    fprintf(out, "\n  === websocket frame decoding ===\n");
    size_t pos = 0;
    int frames = 0;
    while (pos + 2 <= size && frames < 64) {
        const uint8_t *p = data + pos;
        uint64_t fin = (p[0] >> 7) & 1;
        uint64_t opcode = p[0] & 0x0f;
        uint64_t masked = (p[1] >> 7) & 1;
        uint64_t plen = p[1] & 0x7f;
        /* 既知の opcode 以外、RSV 立っている、先頭が余白/ゼロは終端とみなす */
        int known = (opcode == 0 || opcode == 1 || opcode == 2 ||
                     (opcode >= 8 && opcode <= 10));
        if (!known || (p[0] & 0x70)) break;
        size_t hdr = 2;
        uint64_t len = plen;
        if (plen == 126) { if (pos + 4 > size) break; len = ((uint64_t)p[2] << 8) | p[3]; hdr = 4; }
        else if (plen == 127) { if (pos + 10 > size) break; len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | p[2 + i]; hdr = 10; }
        size_t masklen = masked ? 4 : 0;
        if (pos + hdr + masklen + len > size) break;

        const char *opn = nx_spec_enum_name(&ws->fields[4], opcode);
        fprintf(out, "    frame[%d] off=%zu  fin=%llu opcode=%llu(%s) mask=%llu len=%llu\n",
                frames, pos, (unsigned long long)fin, (unsigned long long)opcode,
                opn ? opn : "?", (unsigned long long)masked, (unsigned long long)len);

        const uint8_t *payload = data + pos + hdr + masklen;
        /* マスクされていれば解除して表示用バッファにコピー */
        uint8_t *un = NULL;
        const uint8_t *view = payload;
        if (masked && len) {
            un = (uint8_t *)malloc((size_t)len);
            if (un) {
                const uint8_t *mk = data + pos + hdr;
                for (size_t i = 0; i < len; i++) un[i] = payload[i] ^ mk[i % 4];
                view = un;
            }
        }
        if (opcode == 0x8 || opcode == 0x9 || opcode == 0xA) {
            fprintf(out, "              (control frame)\n");
        } else if (opcode == 0x1) {
            size_t show = len > 64 ? 64 : (size_t)len;
            fprintf(out, "              text: \"%.*s%s\"\n", (int)show, (const char *)view,
                    len > 64 ? "..." : "");
        } else if (opcode == 0x2) {
            size_t show = len > 32 ? 32 : (size_t)len;
            fprintf(out, "              binary: ");
            for (size_t i = 0; i < show; i++) fprintf(out, "%02x ", view[i]);
            fprintf(out, "%s\n", len > 32 ? "..." : "");
        }
        free(un);
        pos += hdr + masklen + (size_t)len;
        frames++;
        if (fin) { /* 続けて次のフレームを試みる */ }
    }
    if (frames == 0) fprintf(out, "    no valid websocket frames\n");
    return frames;
}
