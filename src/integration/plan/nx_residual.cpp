// nx_residual.cpp — 状態の残滓を形式非依存で集める
//
// 方針 (個別対応を増やさないための不変条件):
//   1. 状態 JSON を 1 回だけ辿る。タブの名前も種類も仮定しない。
//      「id を持つオブジェクト」を実体とし、その中の {environmentId, threadId}
//      を所有者参照として拾う。
//   2. 未解決 = 参照の相手が状態内に存在しない。生存判定はここでしない
//      (生存は「同じ owner を持つ実体が状態にあるか」まで)。
//   3. 重複 = id が同じ、または正準キー (kind + 面 + owner) が同じ。
//   4. 除去は発生源 (tabs 配列) で行い、元 JSON の生バイトを保持する。
//      これにより「消したのに別の場所に残っている」を構造的に作らない。

#include "nx_residual.hpp"

#include "../nx_json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const nx_json_t *state_node(const nx_json_t *root) {
    const nx_json_t *st = nx_json_get(root, "state");
    if (st && nx_json_get(st, "tabs")) return st;
    if (nx_json_get(root, "tabs")) return root;
    return NULL;
}

void set_str(char *dst, size_t cap, const char *s) {
    if (!dst || cap == 0) return;
    std::snprintf(dst, cap, "%s", s ? s : "");
}

/* 実体の中で「参照」として扱うキー。ここだけがアプリ依存の入り口で、
   キー名が無い状態でも (参照が無いだけなので) 正しく動く。 */
const char *const kOwnerEnvKeys[]  = {"environmentId", nullptr};
const char *const kOwnerIdKeys[]   = {"threadId", "surfaceId", nullptr};

const char *first_key(const nx_json_t *obj, const char *const *keys) {
    if (!obj || obj->type != NX_JSON_OBJ) return nullptr;
    for (int i = 0; keys[i]; i++) {
        const char *v = nx_json_str(nx_json_get(obj, keys[i]));
        if (v) return v;
    }
    return nullptr;
}

/* オブジェクト (または target) の中から所有者参照を取り出す。 */
int owner_of(const nx_json_t *n, char *env, size_t ecap, char *id, size_t icap) {
    if (!n) return 0;
    env[0] = '\0';
    id[0] = '\0';
    const nx_json_t *tr = nx_json_get(n, "threadRef");
    if (!tr) tr = nx_json_get(n, "owner");
    if (!tr) tr = n;
    const char *e = first_key(tr, kOwnerEnvKeys);
    const char *t = first_key(tr, kOwnerIdKeys);
    if (!e || !t) return 0;
    set_str(env, ecap, e);
    set_str(id, icap, t);
    return 1;
}

std::string canonical_of(const char *kind, const char *label,
                         int has_owner, const char *env, const char *id) {
    std::string s;
    s += kind ? kind : "";
    s += "|";
    s += label ? label : "";
    s += "|";
    if (has_owner) {
        s += env ? env : "";
        s += "/";
        s += id ? id : "";
    } else {
        s += "-";
    }
    return s;
}

} // namespace

extern "C" int nx_residual_collect(const char *state_json, nx_residual_map_t *out) {
    if (!out) return -1;
    std::memset(out, 0, sizeof(*out));
    if (!state_json) return -1;

    nx_json_t *root = nx_json_parse(state_json, std::strlen(state_json));
    if (!root) return -1;
    const nx_json_t *st = state_node(root);
    const nx_json_t *tabs = st ? nx_json_get(st, "tabs") : nullptr;
    if (!tabs || tabs->type != NX_JSON_ARR) {
        nx_json_free(root);
        return -1;
    }

    const size_t n = tabs->n;
    out->n_tabs = n;
    out->n_entities = n;
    out->entities = (nx_residual_entity_t *)std::calloc(n ? n : 1, sizeof(*out->entities));
    if (!out->entities) { nx_json_free(root); return -1; }

    std::vector<size_t> with_owner;   /* owner を持つ実体の index */
    std::vector<size_t> chat_like;    /* chat 系 (面ではなく会話) の index */

    for (size_t i = 0; i < n; i++) {
        const nx_json_t *tab = nx_json_at(tabs, i);
        nx_residual_entity_t &e = out->entities[i];
        e.index = i;
        set_str(e.id, sizeof e.id, nx_json_str(nx_json_get(tab, "id")));

        const nx_json_t *tg = nx_json_get(tab, "target");
        if (!tg) tg = tab;
        const char *kind = nx_json_str(nx_json_get(tg, "kind"));
        set_str(e.kind, sizeof e.kind, kind);

        const char *label = nx_json_str(nx_json_get(tg, "surfaceId"));
        if (!label) label = nx_json_str(nx_json_get(tg, "title"));
        if (!label) label = e.id;
        set_str(e.label, sizeof e.label, label);

        e.has_owner = owner_of(tg, e.owner_env, sizeof e.owner_env,
                               e.owner_thread, sizeof e.owner_thread);
        set_str(e.canonical, sizeof e.canonical,
                canonical_of(e.kind, e.label, e.has_owner,
                             e.owner_env, e.owner_thread).c_str());

        if (e.has_owner) with_owner.push_back(i);
        /* 面 (surface) を所有できるのは会話系。ここに "surface" 以外の
           名前が来ても動くよう、面そのものを除いた集合で判定する。
           名前 (kind) が分からない状態では、owner を持つ実体すべてを
           「所有者になりうる」とみなす (安全側 = 消しすぎない)。 */
        if (!(e.kind[0] && std::strcmp(e.kind, "surface") == 0)) chat_like.push_back(i);
    }

    /* --- 未解決参照: 所有者が状態内に居るか --- */
    out->refs = (nx_residual_ref_t *)std::calloc(n ? n : 1, sizeof(*out->refs));
    if (!out->refs) { nx_residual_free(out); nx_json_free(root); return -1; }
    size_t nrefs = 0;
    for (size_t k = 0; k < with_owner.size(); k++) {
        const size_t i = with_owner[k];
        nx_residual_entity_t &e = out->entities[i];
        int found = 0;
        for (size_t j = 0; j < chat_like.size(); j++) {
            if (chat_like[j] == i) { found = 1; break; }
            const nx_residual_entity_t &o = out->entities[chat_like[j]];
            if (!o.has_owner) continue;
            if (std::strcmp(o.owner_env, e.owner_env) == 0 &&
                std::strcmp(o.owner_thread, e.owner_thread) == 0) {
                found = 1;
                break;
            }
        }
        e.owner_resolved = found;
        if (found) continue;
        nx_residual_ref_t &r = out->refs[nrefs++];
        r.entity_index = i;
        set_str(r.entity_id, sizeof r.entity_id, e.id);
        set_str(r.kind, sizeof r.kind, "owner");
        set_str(r.want_env, sizeof r.want_env, e.owner_env);
        set_str(r.want_id, sizeof r.want_id, e.owner_thread);
        std::snprintf(r.why, sizeof r.why,
                      "所有者 %s/%s が状態内に存在しない (面は閉じられない)",
                      e.owner_env, e.owner_thread);
    }
    out->n_refs = nrefs;

    /* --- 重複: id または正準キーが同じ --- */
    out->dups = (nx_residual_dup_t *)std::calloc(n ? n : 1, sizeof(*out->dups));
    if (!out->dups) { nx_residual_free(out); nx_json_free(root); return -1; }
    std::vector<int> seen(n, 0);
    size_t ndups = 0;
    for (size_t i = 0; i < n; i++) {
        if (seen[i]) continue;
        size_t count = 1;
        for (size_t j = i + 1; j < n; j++) {
            if (seen[j]) continue;
            int same_id = out->entities[i].id[0] &&
                          std::strcmp(out->entities[i].id, out->entities[j].id) == 0;
            int same_canon =
                std::strcmp(out->entities[i].canonical, out->entities[j].canonical) == 0;
            if (same_id || same_canon) {
                seen[j] = 1;
                count++;
            }
        }
        if (count > 1) {
            nx_residual_dup_t &d = out->dups[ndups++];
            set_str(d.canonical, sizeof d.canonical, out->entities[i].canonical);
            d.count = count;
            d.first_index = i;
        }
    }
    out->n_dups = ndups;

    nx_json_free(root);
    return 0;
}

extern "C" void nx_residual_free(nx_residual_map_t *m) {
    if (!m) return;
    std::free(m->entities);
    std::free(m->refs);
    std::free(m->dups);
    std::memset(m, 0, sizeof(*m));
}

extern "C" int nx_residual_stale_indices(const nx_residual_map_t *m, int **out) {
    if (out) *out = nullptr;
    if (!m || !out) return 0;
    int *idx = (int *)std::calloc(m->n_tabs ? m->n_tabs : 1, sizeof(int));
    if (!idx) return 0;
    std::vector<int> mark(m->n_tabs, 0);

    for (size_t i = 0; i < m->n_refs; i++)
        if (m->refs[i].entity_index < mark.size()) mark[m->refs[i].entity_index] = 1;

    /* 重複は「後から現れた方」を残滓とする (先頭は base として保持)。 */
    for (size_t i = 0; i < m->n_dups; i++) {
        const size_t first = m->dups[i].first_index;
        for (size_t j = first + 1; j < m->n_tabs; j++) {
            if (std::strcmp(m->entities[first].canonical, m->entities[j].canonical) == 0 ||
                (m->entities[first].id[0] &&
                 std::strcmp(m->entities[first].id, m->entities[j].id) == 0))
                mark[j] = 1;
        }
    }

    int n = 0;
    for (size_t i = 0; i < m->n_tabs; i++) if (mark[i]) idx[n++] = (int)i;
    *out = idx;
    return n;
}

extern "C" int nx_residual_dump(const char *source, const nx_residual_map_t *m,
                                FILE *out) {
    if (!out) out = stdout;
    if (!m) return -1;
    std::fprintf(out, "\n  === residual map ===\n");
    if (source) std::fprintf(out, "  source: %s\n", source);
    std::fprintf(out, "  entities: %zu\n", m->n_entities);
    for (size_t i = 0; i < m->n_entities; i++) {
        const nx_residual_entity_t &e = m->entities[i];
        std::fprintf(out, "    [%2zu] %-9s %-28s owner=%-8s %s%s\n", e.index,
                     e.kind[0] ? e.kind : "?",
                     e.label[0] ? e.label : "?",
                     e.has_owner ? "yes" : "-",
                     e.has_owner ? e.owner_env : "",
                     e.has_owner ? (e.owner_resolved ? " (resolved)" : " (UNRESOLVED)") : "");
    }
    std::fprintf(out, "  unresolved references: %zu\n", m->n_refs);
    for (size_t i = 0; i < m->n_refs; i++) {
        const nx_residual_ref_t &r = m->refs[i];
        std::fprintf(out, "    entity[%2zu] id=%-28s %s\n", r.entity_index, r.entity_id,
                     r.why);
    }
    std::fprintf(out, "  duplicates: %zu\n", m->n_dups);
    for (size_t i = 0; i < m->n_dups; i++)
        std::fprintf(out, "    x%zu  %s\n", m->dups[i].count, m->dups[i].canonical);
    return 0;
}

extern "C" int nx_residual_prune_json(const char *state_json, const int *indices,
                                      int n_indices, char **out) {
    if (out) *out = nullptr;
    if (!state_json) return -1;
    if (!indices || n_indices <= 0) return 1;

    nx_json_t *root = nx_json_parse(state_json, std::strlen(state_json));
    if (!root) return -1;
    const nx_json_t *st = state_node(root);
    const nx_json_t *tabs = st ? nx_json_get(st, "tabs") : nullptr;
    if (!tabs || tabs->type != NX_JSON_ARR) { nx_json_free(root); return -1; }

    auto drop = [&](size_t i) {
        for (int k = 0; k < n_indices; k++)
            if ((size_t)indices[k] == i) return 1;
        return 0;
    };

    /* 状態の外枠 (state の他フィールド / version 等) を壊さないよう、
       tabs 配列だけを差し替える。前後は元バイトをそのまま使う。 */
    const char *raw = nullptr;
    const size_t tabs_len = nx_json_raw(tabs, &raw);
    const size_t tabs_begin = tabs->raw_begin;
    const size_t src_len = std::strlen(state_json);

    std::string body;
    body.reserve(tabs_len);
    body += "[";
    int kept = 0;
    size_t first_kept = SIZE_MAX;
    for (size_t i = 0; i < tabs->n; i++) {
        if (drop(i)) continue;
        const char *tr = nullptr;
        size_t tlen = nx_json_raw(nx_json_at(tabs, i), &tr);
        if (kept++) body += ",";
        else first_kept = i;
        body.append(tr, tlen);
    }
    body += "]";

    std::string result;
    result.reserve(src_len + body.size());
    result.append(state_json, tabs_begin);
    result += body;
    result.append(state_json + tabs_begin + tabs_len, src_len - tabs_begin - tabs_len);

    /* activeId / recentIds が消えた実体を指していたら、残る先頭へ付け替える
       (消したのに「存在しない active」が残る、という別の残滓を作らない)。 */
    const nx_json_t *active = nx_json_get(st, "activeId");
    const char *active_id = nx_json_str(active);
    int active_gone = 0;
    if (active_id && active_id[0]) {
        active_gone = 1;
        for (size_t i = 0; i < tabs->n; i++) {
            if (drop(i)) continue;
            const char *id = nx_json_str(nx_json_get(nx_json_at(tabs, i), "id"));
            if (id && std::strcmp(id, active_id) == 0) { active_gone = 0; break; }
        }
    }
    if (active_gone) {
        size_t at = result.find("\"activeId\":");
        size_t len = 0;
        const char *araw = nullptr;
        if (at != std::string::npos) {
            len = nx_json_raw(active, &araw);
            if (first_kept != SIZE_MAX) {
                const char *kraw = nullptr;
                const size_t klen =
                    nx_json_raw(nx_json_get(nx_json_at(tabs, first_kept), "id"), &kraw);
                result.replace(at + 11, len, kraw, klen);
            } else {
                result.replace(at + 11, len, "null");
            }
        }
    }

    nx_json_free(root);
    *out = ::strdup(result.c_str());
    return *out ? 0 : -1;
}

nx_residual_risk_t nx_residual_risk(const nx_residual_map_t *m) {
    nx_residual_risk_t r{0, 0, 0, 0};
    if (!m) return r;
    if (m->n_refs) {
        r.destructive = 0.6;
        r.live_sensitive = 0.7;   /* 永続層だけ直すと次の persist で戻る */
        r.order_sensitive = 0.3;  /* WAL が壊れていれば先に直す必要がある */
    }
    if (m->n_dups) {
        r.subtle = 0.8;           /* UI では 2 つ見分けが付かない */
        r.destructive = r.destructive > 0.5 ? r.destructive : 0.3;
    }
    return r;
}

nx_miss_kind_t nx_residual_miss_kind(const nx_residual_map_t *m) {
    if (!m) return NX_MISS_NONE;
    if (m->n_refs) return NX_MISS_ORPHAN_REF;
    if (m->n_dups) return NX_MISS_DUPLICATE;
    return NX_MISS_NONE;
}
