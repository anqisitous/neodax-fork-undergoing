#include "nx_pile.hpp"

#include <algorithm>
#include <set>

namespace nx {

static uint64_t fnv1a(const void *p, size_t n, uint64_t h = 1469598103934665603ULL) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

std::vector<std::string> State::delta(const State &a, const State &b) {
    std::vector<std::string> out;
    for (const auto &kv : a.vals) {
        auto it = b.vals.find(kv.first);
        if (it == b.vals.end() || it->second != kv.second) out.push_back(kv.first);
    }
    for (const auto &kv : b.vals)
        if (!a.vals.count(kv.first)) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

uint64_t Pile::hash_state(const State &s) const {
    uint64_t h = 1469598103934665603ULL;
    for (const auto &kv : s.vals) {          // map は順序保証がある
        h = fnv1a(kv.first.data(), kv.first.size(), h);
        h = fnv1a(&kv.second, sizeof kv.second, h);
    }
    return h;
}

uint64_t Pile::push_root(uint64_t at, State s, const std::string &why) {
    Frame f;
    f.id = next_id_++;
    f.at = at;
    f.why = why;
    f.state = std::move(s);
    f.parent = 0;
    f.hash = hash_state(f.state);
    frames_.push_back(std::move(f));
    uint64_t id = frames_.back().id;
    by_hash_[frames_.back().hash].push_back(id);
    seq_[id] = next_id_;
    return id;
}

uint64_t Pile::derive(uint64_t parent, uint64_t at, State s, const std::string &why) {
    const Frame *pf = get(parent);
    if (!pf) return 0;
    uint64_t h = hash_state(s);

    // 親と同じ状態なら「derive しても何も起きない」。積まない。
    // これが「意味のない処理」を落とす仕組み。
    if (h == pf->hash && s == pf->state) return parent;

    // 兄弟に同じ状態があれば再利用する (重複排除)。
    for (uint64_t c : pf->children) {
        const Frame *cf = get(c);
        if (cf && cf->hash == h && cf->state == s) return c;
    }

    uint64_t pid = parent;
    Frame f;
    f.id = next_id_++;
    f.at = at;
    f.why = why;
    f.state = std::move(s);
    f.parent = pid;
    f.hash = h;
    frames_.push_back(std::move(f));
    uint64_t nid = frames_.back().id;
    by_hash_[frames_.back().hash].push_back(nid);
    seq_[nid] = next_id_;
    for (auto &e : frames_) if (e.id == pid) { e.children.push_back(nid); break; }
    return nid;
}

const Frame *Pile::get(uint64_t id) const {
    for (const auto &f : frames_) if (f.id == id) return &f;
    return nullptr;
}

bool Pile::same_state(uint64_t a, uint64_t b) const {
    const Frame *fa = get(a), *fb = get(b);
    if (!fa || !fb) return false;
    if (fa->hash != fb->hash) return false;
    return fa->state == fb->state;
}

std::vector<uint64_t> Pile::changed_descendants(uint64_t from) const {
    std::vector<uint64_t> out;
    if (from == 0 && !frames_.empty()) from = frames_.front().id;
    const Frame *root = get(from);
    if (!root) return out;
    std::vector<uint64_t> q = root->children;
    std::set<uint64_t> seen{from};
    while (!q.empty()) {
        std::vector<uint64_t> next;
        for (uint64_t id : q) {
            if (!seen.insert(id).second) continue;
            const Frame *f = get(id);
            if (!f) continue;
            const Frame *p = get(f->parent);
            // 親と状態が違う = 実際に意味のある変化があった点
            if (!p || p->hash != f->hash || !(p->state == f->state)) out.push_back(id);
            for (uint64_t c : f->children) next.push_back(c);
        }
        q.swap(next);
    }
    return out;
}

} // namespace nx

// ==================================================================
// 命令のオペランドから「書き込み先」を取り出す
// ==================================================================

#include <cstring>

namespace nx {

// ARM64 の書き込み先を、レジスタ名の集合として返す。
//   str/strb/strh x0, [x1, #8]   -> メモリ [x1,#8] (x0 は読むだけ)
//   mov x0, x1                   -> x0
//   ldr x0, [x1]                 -> x0 (メモリは読むだけ)
//   stp x0, x1, [x2]             -> メモリ [x2]
// 戻り値: 書き込み先の数。
int writes_of(const char *mnemonic, const char *operands,
              char out[][16], int cap) {
    if (!mnemonic || !operands || cap <= 0) return 0;
    int n = 0;

    auto add = [&](const char *s, size_t len) {
        if (n >= cap || len == 0 || len >= 16) return;
        for (int i = 0; i < n; i++)
            if (strlen(out[i]) == len && !strncmp(out[i], s, len)) return;
        memcpy(out[n], s, len);
        out[n][len] = 0;
        n++;
    };

    // ストア系: メモリが書き込み先。保存するレジスタではない。
    const char *st = mnemonic;
    bool is_store = !strncmp(st, "str", 3) || !strncmp(st, "stp", 3) ||
                    !strncmp(st, "stur", 4) || !strncmp(st, "stxr", 4);
    bool is_load  = !strncmp(st, "ldr", 3) || !strncmp(st, "ldp", 3) ||
                    !strncmp(st, "ldur", 4);

    if (is_store) {
        // [ ... ] の中のベースレジスタ + オフセットを 1 つのキーにする
        const char *b = strchr(operands, '[');
        if (b) {
            const char *e = strchr(b, ']');
            if (e) {
                char key[64];
                size_t len = (size_t)(e - b + 1);
                if (len >= sizeof key) len = sizeof key - 1;
                memcpy(key, b, len);
                key[len] = 0;
                add(key, len);
            }
        }
        return n;
    }

    if (is_load) {
        // 書き込み先は最初のオペランド (レジスタ)。メモリは読むだけ。
        const char *p = operands;
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',' && *e != ' ' && *e != '[') e++;
        if (e > p) add(p, (size_t)(e - p));
        return n;
    }

    // 一般命令: 最初のオペランドが書き込み先。
    // 「xN = xN への読み取り専用」命令は除く (cmp/cmn/tst は書かない)。
    if (!strncmp(st, "cmp", 3) || !strncmp(st, "cmn", 3) ||
        !strncmp(st, "tst", 3) || !strncmp(st, "b", 1) ||
        !strncmp(st, "cb", 2) || !strncmp(st, "tb", 2))
        return 0;

    const char *p = operands;
    while (*p == ' ') p++;
    const char *e = p;
    while (*e && *e != ',' && *e != ' ' && *e != '[') e++;
    if (e > p) add(p, (size_t)(e - p));
    return n;
}

} // namespace nx

namespace nx {

std::vector<uint64_t> Pile::find_by_hash(uint64_t from, uint64_t hash) const {
    // まず索引で「その状態を持つ frame」を直接引く (木をまたいで O(1))。
    auto it = by_hash_.find(hash);
    if (it == by_hash_.end()) return {};

    // 次に、引いた候補を範囲で絞る。舐めるのは候補だけで、木全体を BFS しない。
    //
    // 範囲の意味:
    //   from == 0  … 範囲指定なし。全ての木をまたいで参照してよい
    //   from != 0  … from の子孫だけ (from 自身は含めない)
    // 権限 (どこまで見てよいか) は from==0 で広げられるが、
    // 参照 (実際に舐める範囲) は常に候補だけに限られる。
    std::vector<uint64_t> out;
    if (from == 0) return it->second;          // 明示的な「範囲なし」

    std::set<uint64_t> scope = descendants_of(from);
    for (uint64_t id : it->second)
        if (scope.count(id)) out.push_back(id);
    return out;
}

std::vector<uint64_t> Pile::find_exact(uint64_t from, const State &want,
                                       const std::string &why) const {
    // まず指紋で絞る。ここで落ちるものが大半。
    uint64_t h = hash_state(want);
    std::vector<uint64_t> cand = find_by_hash(from, h);

    // 指紋が一致したものだけ厳密比較 (衝突はここで落ちる)。
    std::vector<uint64_t> out;
    for (uint64_t id : cand) {
        const Frame *f = get(id);
        if (!f) continue;
        if (!why.empty() && f->why != why) continue;
        if (f->state == want) out.push_back(id);
    }
    return out;
}

} // namespace nx

namespace nx {

std::set<uint64_t> Pile::descendants_of(uint64_t from) const {
    std::set<uint64_t> out;
    const Frame *root = get(from);
    if (!root) return out;
    std::vector<uint64_t> q = root->children;
    while (!q.empty()) {
        std::vector<uint64_t> next;
        for (uint64_t id : q) {
            if (!out.insert(id).second) continue;
            const Frame *f = get(id);
            if (!f) continue;
            for (uint64_t c : f->children) next.push_back(c);
        }
        q.swap(next);
    }
    return out;
}

} // namespace nx
