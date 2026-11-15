// nx_recipe.cpp — 書き換え手順を保存し、段ごとに検証して実行する
//
// 目的:
//   アプリを最初から書き換えず、まず「手順書」を作って保存する。
//   手順が最終形になってから 1 段ずつ確かめる。
//
// 使う安全部品:
//   P4: APFS クローン / ad-hoc 再署名 / strict 検証 / 失敗時 rollback
//   P2: 各段の後の再観測 (observe-stub)
//   P3: シンボル解決 (resolve) と差分範囲契約 (patch lo/hi, verify-range)
//
// このファイルは状態ストアには一切触れない。対象は Mach-O とバンドルだけ。

#include "nx_recipe.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>

namespace {

/* ------------------------------------------------------------------ */
/* 小さなヘルパ                                                       */
/* ------------------------------------------------------------------ */

static bool exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

static std::string join(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

static unsigned char *read_file(const std::string &p, size_t *n) {
    *n = 0;
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    unsigned char *b = (unsigned char *)malloc((size_t)sz);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)sz, f);
    fclose(f);
    *n = got;
    return b;
}

static unsigned long rd32le(const unsigned char *p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}
static unsigned long rd32be(const unsigned char *p) {
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}
static unsigned long long rd64le(const unsigned char *p) {
    return (unsigned long long)p[0] | ((unsigned long long)p[1] << 8) |
           ((unsigned long long)p[2] << 16) | ((unsigned long long)p[3] << 24) |
           ((unsigned long long)p[4] << 32) | ((unsigned long long)p[5] << 40) |
           ((unsigned long long)p[6] << 48) | ((unsigned long long)p[7] << 56);
}

static void sha256_hex(const std::string &p, char *out, size_t cap) {
    out[0] = 0;
    size_t n = 0;
    unsigned char *b = read_file(p, &n);
    if (!b) return;
    unsigned char d[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(b, (CC_LONG)n, d);
    free(b);
    size_t w = 0;
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH && w + 3 < cap; i++)
        w += (size_t)snprintf(out + w, cap - w, "%02x", d[i]);
}

/* ------------------------------------------------------------------ */
/* Mach-O: シンボル解決と vaddr -> file offset                        */
/* ------------------------------------------------------------------ */

struct Slice {
    unsigned long long base = 0;    // FAT 内のスライス開始 (絶対)
    bool ok = false;
};

struct Sym {
    std::string name;
    unsigned long long vaddr = 0;
};

struct MachO {
    std::vector<unsigned char> buf;
    Slice slice;
    std::vector<Sym> syms;
    /* セグメント: vaddr, vmsize, fileoff(スライス内), filesize */
    struct Seg { unsigned long long va, vs, fo, fs; };
    std::vector<Seg> segs;
    bool parsed = false;
};

/* FAT なら arm64 スライスを選ぶ。Macho を埋める。 */
static bool macho_parse(MachO &m, const std::string &path) {
    size_t n = 0;
    unsigned char *b = read_file(path, &n);
    if (!b || n < 32) { free(b); return false; }
    m.buf.assign(b, b + n);
    free(b);

    const unsigned char *p = m.buf.data();
    unsigned long magic = rd32be(p);   // FAT は big-endian で判定
    if (magic == 0xCAFEBABEu) {
        /* FAT: arm64 を探す */
        unsigned long nfat = rd32be(p + 4);
        unsigned long long arm_base = 0;
        bool found = false;
        for (unsigned long i = 0; i < nfat; i++) {
            const unsigned char *a = p + 8 + i * 20;
            unsigned long cputype = rd32be(a);
            unsigned long off = rd32be(a + 8);
            if (cputype == 0x0100000Cu) { arm_base = off; found = true; break; }
        }
        if (!found) return false;
        m.slice.base = arm_base;
    } else {
        m.slice.base = 0;
    }
    m.slice.ok = true;

    unsigned long long base = m.slice.base;
    if (base + 32 > m.buf.size()) return false;
    const unsigned char *h = m.buf.data() + base;
    if (rd32le(h) != 0xFEEDFACFu) {
        /* 32-bit や big-endian は対象外 (AE は arm64 64bit) */
        return false;
    }
    unsigned long ncmds = rd32le(h + 16);

    const unsigned char *lc = h + 32;
    for (unsigned long i = 0; i < ncmds; i++) {
        if (lc + 8 > m.buf.data() + m.buf.size()) break;
        unsigned long cmd = rd32le(lc);
        unsigned long cmdsize = rd32le(lc + 4);
        if (cmdsize < 8) break;
        if (cmd == 0x19u) {   /* LC_SEGMENT_64 */
            const unsigned char *s = lc + 8;   /* segname[16] */
            unsigned long long va  = rd64le(s + 16);
            unsigned long long vs  = rd64le(s + 24);
            unsigned long long fo  = rd64le(s + 32);
            unsigned long long fs  = rd64le(s + 40);
            m.segs.push_back({va, vs, fo, fs});
        } else if (cmd == 0x02u) {   /* LC_SYMTAB */
            unsigned long symoff  = rd32le(lc + 8);
            unsigned long nsyms   = rd32le(lc + 12);
            unsigned long stroff  = rd32le(lc + 16);
            unsigned long strsize = rd32le(lc + 20);
            unsigned long long s_off = base + symoff;
            unsigned long long t_off = base + stroff;
            if (t_off + strsize > m.buf.size()) { lc += cmdsize; continue; }
            const char *strtab = (const char *)(m.buf.data() + t_off);
            for (unsigned long k = 0; k < nsyms; k++) {
                unsigned long long e = s_off + (unsigned long long)k * 16;
                if (e + 16 > m.buf.size()) break;
                const unsigned char *nl = m.buf.data() + e;
                unsigned long n_strx = rd32le(nl);
                unsigned char n_type = nl[4];
                unsigned long long n_value = rd64le(nl + 8);
                unsigned char ntype = n_type & 0x0e;
                if (ntype != 0x0e) continue;      /* N_SECT のみ */
                if (n_value == 0) continue;
                if (n_strx >= strsize) continue;
                const char *nm = strtab + n_strx;
                size_t maxl = strsize - n_strx;
                size_t l = strnlen(nm, maxl);
                if (l == 0 || l >= maxl) continue;
                m.syms.push_back({std::string(nm, l), n_value});
            }
        }
        lc += cmdsize;
    }
    m.parsed = true;
    return true;
}

/* vaddr -> ファイル内絶対オフセット。失敗なら false。 */
static bool vaddr_to_off(const MachO &m, unsigned long long vaddr,
                         unsigned long long *off) {
    for (const auto &s : m.segs) {
        if (vaddr >= s.va && vaddr < s.va + s.vs) {
            *off = m.slice.base + s.fo + (vaddr - s.va);
            return true;
        }
    }
    return false;
}

/* name を部分一致で探す。完全一致を優先。見つからなければ vaddr=0。 */
static unsigned long long find_sym(const MachO &m, const std::string &needle) {
    // 完全一致を最優先。部分一致では .cold などの例外経路を避け、
    // 同種が複数あるときは最も低いアドレス (本体) を採る。
    unsigned long long exact = 0, best = 0, cold = 0;
    for (const auto &s : m.syms) {
        if (s.name == needle) { exact = s.vaddr; break; }
        if (s.name.find(needle) == std::string::npos) continue;
        bool is_cold = s.name.find(".cold") != std::string::npos;
        if (is_cold) {
            if (!cold || s.vaddr < cold) cold = s.vaddr;
        } else {
            if (!best || s.vaddr < best) best = s.vaddr;
        }
    }
    return exact ? exact : (best ? best : cold);
}

/* target より後ろで最も近い定義シンボル。関数の終端の上限として使う。 */
static unsigned long long next_sym_after(const MachO &m, unsigned long long vaddr) {
    unsigned long long best = 0;
    for (const auto &s : m.syms) {
        if (s.vaddr > vaddr && (best == 0 || s.vaddr < best)) best = s.vaddr;
    }
    return best;
}

/* パッチの上限を決める。
 *
 * 「次のシンボル」は関数の終端とは限らない (ラベルや near thunk が
 * 4 バイト先に来ることがある)。実際に書く stub は 8 バイトなので、
 * 少なくとも off+8 を範囲に含める。次シンボルがそれより先なら、
 * そこを上限として契約を広げる。 */
static unsigned long long extent_hi(const MachO &m, unsigned long long vaddr,
                                    unsigned long long off) {
    unsigned long long hi = 0;
    unsigned long long nv = next_sym_after(m, vaddr);
    if (nv) vaddr_to_off(m, nv, &hi);
    if (hi < off + 8) hi = off + 8;
    if (hi > m.buf.size()) hi = off + 8;
    return hi;
}

/* arm64: mov w0,#0 ; ret */
static const unsigned char kStub[8] = {0x00, 0x00, 0x80, 0x52,
                                       0xC0, 0x03, 0x5F, 0xD6};

/* ------------------------------------------------------------------ */
/* 手順書 (recipe)                                                     */
/* ------------------------------------------------------------------ */

struct Step {
    std::string kind;                 // clone / verify-clone / patch / ...
    std::vector<std::pair<std::string, std::string>> kv;
    std::string get(const char *k) const {
        for (const auto &e : kv) if (e.first == k) return e.second;
        return std::string();
    }
};

struct Recipe {
    std::string app, work, lib_rel, lib_sha;
    bool complete = true;
    std::vector<Step> steps;
    bool rollback = true;
};

static void put_kv(FILE *o, const Step &s) {
    for (const auto &e : s.kv) fprintf(o, " %s=%s", e.first.c_str(), e.second.c_str());
}

static int recipe_write(const Recipe &r, const std::string &path, FILE *out) {
    FILE *f = path.empty() ? out : fopen(path.c_str(), "wb");
    if (!f) { if (out) fprintf(out, "  [recipe] cannot write %s\n", path.c_str()); return 2; }
    fprintf(f, "NXRECIPE 1\n");
    fprintf(f, "app=%s\n", r.app.c_str());
    fprintf(f, "work=%s\n", r.work.c_str());
    fprintf(f, "lib=%s\n", r.lib_rel.c_str());
    fprintf(f, "lib_sha256=%s\n", r.lib_sha.c_str());
    fprintf(f, "complete=%d\n", r.complete ? 1 : 0);
    for (const auto &s : r.steps) {
        fprintf(f, "step %s", s.kind.c_str());
        put_kv(f, s);
        fprintf(f, "\n");
    }
    fprintf(f, "onfail %s\n", r.rollback ? "rollback" : "stop");
    if (f != out) fclose(f);
    return 0;
}

static int recipe_read(const std::string &path, Recipe &r, FILE *out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) { if (out) fprintf(out, "  [recipe] cannot read %s\n", path.c_str()); return 2; }
    char line[4096];
    bool hdr = false;
    while (fgets(line, sizeof line, f)) {
        for (char *q = line; *q; q++) if (*q == '\n' || *q == '\r') { *q = 0; break; }
        if (!*line) continue;
        if (strncmp(line, "NXRECIPE", 8) == 0) { hdr = true; continue; }
        char *eq = strchr(line, '=');
        if (strncmp(line, "app=", 4) == 0) { r.app = line + 4; continue; }
        if (strncmp(line, "work=", 5) == 0) { r.work = line + 5; continue; }
        if (strncmp(line, "lib=", 4) == 0) { r.lib_rel = line + 4; continue; }
        if (strncmp(line, "lib_sha256=", 11) == 0) { r.lib_sha = line + 11; continue; }
        if (strncmp(line, "complete=", 9) == 0) { r.complete = atoi(line + 9) != 0; continue; }
        if (strncmp(line, "onfail ", 7) == 0) { r.rollback = strstr(line, "rollback") != NULL; continue; }
        if (strncmp(line, "step ", 5) == 0) {
            Step s;
            char *p = line + 5;
            char *sp = strchr(p, ' ');
            if (sp) { *sp = 0; s.kind = p; p = sp + 1; }
            else { s.kind = p; p = NULL; }
            while (p && *p) {
                char *k = p;
                char *e = strchr(p, '=');
                if (!e) break;
                *e = 0;
                char *v = e + 1;
                char *sp2 = strchr(v, ' ');
                if (sp2) *sp2 = 0;
                s.kv.push_back({std::string(k), std::string(v)});
                p = sp2 ? sp2 + 1 : NULL;
            }
            r.steps.push_back(s);
        }
        (void)eq;
    }
    fclose(f);
    if (!hdr) { if (out) fprintf(out, "  [recipe] not a recipe: %s\n", path.c_str()); return 2; }
    return 0;
}

} // namespace

/* ================================================================== */
/* build: 手順書を作って保存する (アプリは変更しない)                  */
/* ================================================================== */

extern "C" int nx_recipe_build(const char *app_path, const char *target_hint,
                               const char *out_path, FILE *out) {
    if (!out) out = stdout;
    if (!app_path) { fprintf(out, "recipe: --path required\n"); return 2; }
    std::string app = app_path;
    if (!exists(app)) { fprintf(out, "  [recipe] app not found: %s\n", app.c_str()); return 2; }

    std::string lib_rel = "Contents/Frameworks/AfterFXLib.framework/Versions/A/AfterFXLib";
    std::string lib = join(app, lib_rel);
    if (!exists(lib)) { fprintf(out, "  [recipe] AfterFXLib not found\n"); return 2; }

    MachO mo;
    if (!macho_parse(mo, lib)) {
        fprintf(out, "  [recipe] cannot parse Mach-O: %s\n", lib.c_str());
        return 2;
    }
    fprintf(out, "  [recipe] parsed %zu symbols, %zu segments\n",
            mo.syms.size(), mo.segs.size());

    /* P4⑤: 侵襲の小さい順に候補を持つ。先に解決できたものを主手順に採る。 */
    struct Cand { const char *label; const char *needle; };
    const Cand cands[] = {
        {"ngl-only",      "NglControllerC1"},     /* 構築だけ外す (最小) */
        {"ngl-only-alt",  "NglControllerC2"},
        {"license-setup", "DoSetup"},             /* 大枠 */
        {"shouldcheck",   "ShouldCheckLicense"},  /* 旧経路 */
    };

    std::string chosen_label, chosen_sym;
    unsigned long long chosen_vaddr = 0, chosen_off = 0, hi_off = 0;
    bool have_patch = false;

    for (const auto &c : cands) {
        unsigned long long v = find_sym(mo, c.needle);
        if (!v) continue;
        unsigned long long off = 0;
        if (!vaddr_to_off(mo, v, &off)) continue;
        unsigned long long hi = extent_hi(mo, v, off);
        /* target_hint があれば、それを含む候補を優先 */
        if (target_hint && *target_hint &&
            chosen_label.empty() && chosen_label != target_hint &&
            std::string(c.label).find(target_hint) == std::string::npos) {
            /* まだ確定しない。下でヒント一致を優先するため一旦記録 */
        }
        if (!have_patch) {
            chosen_label = c.label; chosen_sym = c.needle;
            chosen_vaddr = v; chosen_off = off; hi_off = hi;
            have_patch = true;
        }
    }

    /* ヒント指定があれば、それに合うシンボルを直接探して差し替える */
    if (target_hint && *target_hint) {
        unsigned long long v = find_sym(mo, target_hint);
        unsigned long long off = 0;
        if (v && vaddr_to_off(mo, v, &off)) {
            unsigned long long hi = extent_hi(mo, v, off);
            chosen_label = "hint:" + std::string(target_hint);
            chosen_sym = target_hint;
            chosen_vaddr = v; chosen_off = off; hi_off = hi;
            have_patch = true;
        }
    }

    Recipe r;
    r.app = app;
    r.work = app + ".neodax-work";
    r.lib_rel = lib_rel;

    char sha[80];
    sha256_hex(lib, sha, sizeof sha);
    r.lib_sha = sha;
    r.complete = have_patch;

    Step s1; s1.kind = "clone"; r.steps.push_back(s1);

    Step s2; s2.kind = "verify-clone";
    s2.kv.push_back({"sha", r.lib_sha}); r.steps.push_back(s2);

    if (have_patch) {
        Step sp; sp.kind = "patch";
        sp.kv.push_back({"mode", chosen_label});
        sp.kv.push_back({"sym", chosen_sym});
        char t[64];
        snprintf(t, sizeof t, "0x%llx", chosen_vaddr); sp.kv.push_back({"vaddr", t});
        snprintf(t, sizeof t, "0x%llx", chosen_off);   sp.kv.push_back({"off", t});
        snprintf(t, sizeof t, "0x%llx", hi_off);       sp.kv.push_back({"hi", t});
        /* stub bytes */
        char hx[32]; int w = 0;
        for (int i = 0; i < 8; i++) w += snprintf(hx + w, sizeof hx - w, "%02x", kStub[i]);
        sp.kv.push_back({"hex", hx});
        r.steps.push_back(sp);

        /* P3: 差分が範囲内に収まることの契約検証 */
        Step vr; vr.kind = "verify-range";
        snprintf(t, sizeof t, "0x%llx", chosen_off); vr.kv.push_back({"lo", t});
        snprintf(t, sizeof t, "0x%llx", hi_off);     vr.kv.push_back({"hi", t});
        r.steps.push_back(vr);

        /* P2: 各段の後の再観測。stub が実体に残っているかを読む */
        Step ob; ob.kind = "observe-stub";
        snprintf(t, sizeof t, "0x%llx", chosen_off); ob.kv.push_back({"off", t});
        ob.kv.push_back({"hex", hx});
        r.steps.push_back(ob);
    } else {
        fprintf(out, "  [recipe] 対象シンボルを解決できませんでした。\n"
                     "           手順は未完成 (complete=0)。\n"
                     "           --target <部分一致> で対象を指定してください。\n");
    }

    Step rs; rs.kind = "resign"; r.steps.push_back(rs);
    Step vs; vs.kind = "verify-sign"; r.steps.push_back(vs);

    if (out_path && *out_path) {
        int rc = recipe_write(r, out_path, out);
        if (rc == 0)
            fprintf(out, "  [recipe] saved: %s (complete=%d, steps=%zu)\n",
                    out_path, r.complete ? 1 : 0, r.steps.size());
        return r.complete ? 0 : 1;
    }
    fprintf(out, "  [recipe] (not saved; use -o FILE)\n");
    for (const auto &s : r.steps) {
        fprintf(out, "    step %s", s.kind.c_str());
        put_kv(out, s);
        fprintf(out, "\n");
    }
    return r.complete ? 0 : 1;
}

/* ================================================================== */
/* 探索用の小 API: シンボル位置と呼び出し元 (BL) の列挙             */
/* ================================================================== */

extern "C" int nx_recipe_locate_symbol(const char *lib, const char *name,
                                        nx_site_t *out) {
    if (!lib || !name || !out) return 1;
    MachO mo;
    if (!macho_parse(mo, lib)) return 1;
    unsigned long long v = find_sym(mo, name);
    if (!v) return 1;
    unsigned long long off = 0;
    if (!vaddr_to_off(mo, v, &off)) return 1;
    out->vaddr = v;
    out->off = off;
    return 0;
}

extern "C" int nx_recipe_find_callers(const char *lib,
                                       unsigned long long target_vaddr,
                                       nx_site_t *out, int max) {
    if (!lib || !out || max <= 0) return 0;
    MachO mo;
    if (!macho_parse(mo, lib)) return 0;
    int n = 0;
    for (const auto &s : mo.segs) {
        if (s.fs == 0) continue;
        unsigned long long base = mo.slice.base + s.fo;
        if (base + s.fs > mo.buf.size()) continue;
        for (unsigned long long i = 0; i + 4 <= s.fs && n < max; i += 4) {
            unsigned long w = rd32le(mo.buf.data() + base + i);
            if ((w & 0xFC000000u) != 0x94000000u) continue;   /* BL */
            long long imm = (long long)(w & 0x03FFFFFFu);
            if (imm & (1LL << 25)) imm |= ~((1LL << 26) - 1);  /* sign-extend */
            unsigned long long pc = s.va + i;
            unsigned long long dst = pc + (unsigned long long)(imm * 4);
            if (dst != target_vaddr) continue;
            out[n].vaddr = pc;
            out[n].off = base + i;
            n++;
        }
    }
    return n;
}

extern "C" int nx_recipe_read_word(const char *lib, unsigned long long off,
                                   unsigned long *word) {
    if (!lib || !word) return 1;
    FILE *f = fopen(lib, "rb");
    if (!f) return 1;
    fseek(f, (long)off, SEEK_SET);
    unsigned char b[4];
    size_t got = fread(b, 1, 4, f);
    fclose(f);
    if (got != 4) return 1;
    *word = rd32le(b);
    return 0;
}

/* ================================================================== */
/* show                                                               */
/* ================================================================== */

extern "C" int nx_recipe_show(const char *recipe_path, FILE *out) {
    if (!out) out = stdout;
    Recipe r;
    if (recipe_read(recipe_path ? recipe_path : "", r, out) != 0) return 2;
    fprintf(out, "app       = %s\n", r.app.c_str());
    fprintf(out, "work      = %s\n", r.work.c_str());
    fprintf(out, "lib       = %s\n", r.lib_rel.c_str());
    fprintf(out, "lib_sha256= %s\n", r.lib_sha.c_str());
    fprintf(out, "complete  = %d\n", r.complete ? 1 : 0);
    fprintf(out, "onfail    = %s\n", r.rollback ? "rollback" : "stop");
    for (size_t i = 0; i < r.steps.size(); i++) {
        fprintf(out, "  %2zu. %s", i + 1, r.steps[i].kind.c_str());
        put_kv(out, r.steps[i]);
        fprintf(out, "\n");
    }
    return 0;
}

/* ================================================================== */
/* run: 段ごとに検証して実行 (既定 dry-run)                           */
/* ================================================================== */

namespace {

static std::string work_lib(const Recipe &r) { return join(r.work, r.lib_rel); }
static std::string orig_lib(const Recipe &r) { return join(r.app, r.lib_rel); }

static bool read_bytes_at(const std::string &path, unsigned long long off,
                          unsigned char *dst, size_t n) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, (long)off, SEEK_SET);
    size_t got = fread(dst, 1, n, f);
    fclose(f);
    return got == n;
}

static bool write_bytes_at(const std::string &path, unsigned long long off,
                           const unsigned char *src, size_t n) {
    FILE *f = fopen(path.c_str(), "r+b");
    if (!f) return false;
    fseek(f, (long)off, SEEK_SET);
    size_t wr = fwrite(src, 1, n, f);
    fclose(f);
    return wr == n;
}

static bool hex_to_bytes(const std::string &h, unsigned char *out, size_t n) {
    if (h.size() < n * 2) return false;
    for (size_t i = 0; i < n; i++) {
        auto v = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int a = v(h[i * 2]), b = v(h[i * 2 + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (unsigned char)((a << 4) | b);
    }
    return true;
}

static int do_rollback(const Recipe &r, FILE *out) {
    fprintf(out, "  [recipe] ROLLBACK: 原版は未変更。作業コピーを破棄します\n");
    std::string cmd = "rm -rf '" + r.work + "'";
    int rc = system(cmd.c_str());
    fprintf(out, "  [recipe] rollback rc=%d (%s)\n", rc, r.work.c_str());
    return rc == 0 ? 0 : 1;
}

} // namespace

extern "C" int nx_recipe_run(const char *recipe_path, int apply, int upto, FILE *out) {
    if (!out) out = stdout;
    Recipe r;
    if (recipe_read(recipe_path ? recipe_path : "", r, out) != 0) return 2;

    fprintf(out, "=== recipe run (%s) app=%s ===\n",
            apply ? "APPLY" : "DRY-RUN", r.app.c_str());
    if (!r.complete) {
        fprintf(out, "  [recipe] 未完成の手順です (complete=0)。実行しません。\n"
                     "           先に手順を完成させてください。\n");
        return 2;
    }

    int limit = (upto > 0 && (size_t)upto < r.steps.size()) ? upto : (int)r.steps.size();

    for (int i = 0; i < limit; i++) {
        const Step &s = r.steps[i];
        fprintf(out, "[%2d/%d] %s\n", i + 1, (int)r.steps.size(), s.kind.c_str());

        bool ok = true;
        if (s.kind == "clone") {
            if (!apply) {
                fprintf(out, "   dry: cp -c -R '%s' '%s'\n", r.app.c_str(), r.work.c_str());
            } else {
                if (exists(r.work)) system(("rm -rf '" + r.work + "'").c_str());
                std::string cmd = "cp -c -R '" + r.app + "' '" + r.work + "'";
                fprintf(out, "   run: %s\n", cmd.c_str());
                ok = (system(cmd.c_str()) == 0) && exists(r.work);
            }
        } else if (s.kind == "verify-clone") {
            std::string wl = apply ? work_lib(r) : orig_lib(r);
            char sha[80]; sha256_hex(wl, sha, sizeof sha);
            std::string want = s.get("sha");
            ok = !want.empty() && want == sha;
            fprintf(out, "   %s sha=%s\n", ok ? "PASS" : "FAIL", sha);
        } else if (s.kind == "patch") {
            unsigned long long off = strtoull(s.get("off").c_str(), NULL, 0);
            unsigned long long hi  = strtoull(s.get("hi").c_str(), NULL, 0);
            std::string hx = s.get("hex");
            size_t nbytes = hx.size() / 2;
            unsigned char stub[64];
            if (nbytes == 0 || nbytes > sizeof stub ||
                !hex_to_bytes(hx, stub, nbytes)) { ok = false; }
            std::string target = apply ? work_lib(r) : orig_lib(r);
            unsigned char before[64];
            bool rb = ok && read_bytes_at(target, off, before, nbytes);
            if (!rb) { ok = false; }
            fprintf(out, "   mode=%s sym=%s vaddr=%s off=0x%llx hi=0x%llx n=%zu\n",
                    s.get("mode").c_str(), s.get("sym").c_str(),
                    s.get("vaddr").c_str(), off, hi, nbytes);
            if (off + nbytes > hi) { fprintf(out, "   FAIL: patch range outside site\n"); ok = false; }
            if (apply && ok) {
                ok = write_bytes_at(target, off, stub, nbytes);
                unsigned char after[64];
                ok = ok && read_bytes_at(target, off, after, nbytes) &&
                     memcmp(after, stub, nbytes) == 0;
                fprintf(out, "   %s wrote %zu bytes\n", ok ? "PASS" : "FAIL", nbytes);
            } else {
                fprintf(out, "   dry: would write %zu bytes\n", nbytes);
            }
        } else if (s.kind == "verify-range") {
            unsigned long long lo = strtoull(s.get("lo").c_str(), NULL, 0);
            unsigned long long hi = strtoull(s.get("hi").c_str(), NULL, 0);
            if (!apply) {
                fprintf(out, "   dry: assert diffs within file 0x%llx..0x%llx\n", lo, hi);
            } else {
                size_t na = 0, nb = 0;
                unsigned char *a = read_file(orig_lib(r), &na);
                unsigned char *b = read_file(work_lib(r), &nb);
                if (!a || !b || na != nb) { ok = false; }
                else {
                    long long first = -1, last = -1; size_t nd = 0;
                    for (size_t k = 0; k < na; k++) {
                        if (a[k] == b[k]) continue;
                        nd++;
                        if (first < 0) first = (long long)k;
                        last = (long long)k;
                    }
                    fprintf(out, "   diffs=%zu first=0x%llx last=0x%llx\n",
                            nd, (unsigned long long)first, (unsigned long long)last);
                    ok = nd > 0 && first >= (long long)lo && last < (long long)hi;
                }
                free(a); free(b);
                fprintf(out, "   %s range contract\n", ok ? "PASS" : "FAIL");
            }
        } else if (s.kind == "observe-stub") {
            unsigned long long off = strtoull(s.get("off").c_str(), NULL, 0);
            std::string hx = s.get("hex");
            size_t nbytes = hx.size() / 2;
            unsigned char want[64], got[64];
            if (nbytes == 0 || nbytes > sizeof want || !hex_to_bytes(hx, want, nbytes))
                ok = false;
            std::string target = apply ? work_lib(r) : orig_lib(r);
            bool rb = ok && read_bytes_at(target, off, got, nbytes);
            if (!apply) fprintf(out, "   dry: re-observe %zu bytes at 0x%llx\n", nbytes, off);
            else {
                ok = rb && memcmp(got, want, nbytes) == 0;
                fprintf(out, "   %s re-observed %zu bytes\n", ok ? "PASS" : "FAIL", nbytes);
            }
        } else if (s.kind == "resign") {
            if (!apply) fprintf(out, "   dry: codesign --force --deep --sign - '%s'\n", r.work.c_str());
            else {
                std::string cmd = "codesign --force --deep --sign - '" + r.work + "'";
                ok = (system(cmd.c_str()) == 0);
                fprintf(out, "   %s resign (adhoc)\n", ok ? "PASS" : "FAIL");
            }
        } else if (s.kind == "verify-sign") {
            if (!apply) fprintf(out, "   dry: codesign --verify --strict '%s'\n", r.work.c_str());
            else {
                std::string cmd = "codesign --verify --strict '" + r.work + "' 2>/dev/null";
                ok = (system(cmd.c_str()) == 0);
                fprintf(out, "   %s signature\n", ok ? "PASS" : "FAIL");
            }
        } else {
            fprintf(out, "   (unknown step; skipped)\n");
        }

        if (!ok) {
            fprintf(out, "  [%2d] FAILED: %s\n", i + 1, s.kind.c_str());
            if (apply && r.rollback) do_rollback(r, out);
            return 1;
        }
    }
    fprintf(out, "=== recipe run: OK (%d steps, %s) ===\n",
            limit, apply ? "applied" : "dry-run");
    return 0;
}
