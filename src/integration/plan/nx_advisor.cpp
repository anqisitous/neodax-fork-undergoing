#include "nx_advisor.hpp"

#include "nx_deepai.h"
#include "nx_json.h"
#include "nx_clean.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>

namespace {

struct Item {
    std::string id;        /* 対象タブ id (JSON 文字列そのまま) */
    std::string thread;    /* 参照している threadId (あれば) */
    std::string reason;
    int kind = 0;          /* 0=orphan 1=identity 2=duplicate 3=empty */
};

const nx_json_t *state_node(const nx_json_t *root) {
    if (!root) return NULL;
    const nx_json_t *st = nx_json_get(root, "state");
    return st && st->type == NX_JSON_OBJ ? st : root;
}

std::string id_str(const nx_json_t *tab) {
    const nx_json_t *id = nx_json_get(tab, "id");
    std::string s;
    if (!id) return s;
    if (id->type == NX_JSON_ARR) {
        /* ["chat",env,thread] / ["draft",id] を JSON 文字列として再構成 */
        s = "[";
        for (size_t i = 0; i < id->n; i++) {
            if (i) s += ",";
            s += "\"";
            s += nx_json_str(nx_json_at(id, i)) ? nx_json_str(nx_json_at(id, i)) : "";
            s += "\"";
        }
        s += "]";
    } else {
        const char *v = nx_json_str(id);
        if (v) s = v;
    }
    return s;
}

std::string thread_of(const nx_json_t *tab) {
    const nx_json_t *ref = nx_json_get(tab, "threadRef");
    if (!ref) {
        const nx_json_t *t = nx_json_get(tab, "target");
        if (t) ref = nx_json_get(t, "threadRef");
    }
    const char *v = ref ? nx_json_str(nx_json_get(ref, "threadId")) : NULL;
    return v ? v : "";
}

std::string env_of(const nx_json_t *tab) {
    const nx_json_t *ref = nx_json_get(tab, "threadRef");
    if (!ref) {
        const nx_json_t *t = nx_json_get(tab, "target");
        if (t) ref = nx_json_get(t, "threadRef");
    }
    const char *v = ref ? nx_json_str(nx_json_get(ref, "environmentId")) : NULL;
    return v ? v : "";
}

int label_index(const std::string &id, const char *tag) {
    /* ["chat",...] → 0 / ["draft",...] → 1 (無ければ -1) */
    if (id.find(std::string("\"") + tag + "\"") == 0 || id.find(std::string("[\"") + tag + "\"") == 0) return 0;
    return -1;
}

} // namespace

extern "C" int nx_advisor_derive(const char *state_path, FILE *out) {
    if (!out) return -1;
    char *raw = nx_deepai_load_state(state_path);
    if (!raw) { fprintf(out, "advisor: 状態を読めない: %s\n", state_path); return -1; }

    nx_json_t *root = nx_json_parse(raw, strlen(raw));
    free(raw);
    const nx_json_t *st = state_node(root);
    const nx_json_t *tabs = st ? nx_json_get(st, "tabs") : NULL;
    if (!tabs || tabs->type != NX_JSON_ARR) {
        fprintf(out, "advisor: tabs が無い (壊れた状態)\n");
        nx_json_free(root);
        return -1;
    }

    std::vector<Item> items;
    std::map<std::string, std::vector<std::string>> by_thread;

    for (size_t i = 0; i < tabs->n; i++) {
        const nx_json_t *tab = nx_json_at(tabs, i);
        std::string id = id_str(tab);
        std::string th = thread_of(tab);
        std::string env = env_of(tab);
        int is_draft = label_index(id, "draft") == 0 || id.find("\"draft\"") != std::string::npos;
        int is_surface = id.find("\"surface\"") != std::string::npos;

        if (is_surface && th.empty()) {
            items.push_back({id, th, "owner の無い surface (閉じられない)", 0});
        } else if (is_draft && !th.empty()) {
            items.push_back({id, th, "id は draft のまま、実体は既存 thread", 1});
        } else if (is_draft && th.empty() && env.empty()) {
            items.push_back({id, th, "空の draft (実体を参照していない)", 3});
        }
        if (!th.empty()) {
            /* 同じ thread でも chat と surface は別物 (正規の対)。
               重複とみなすのは「同じ thread + 同じ種別」。 */
            const nx_json_t *tg = nx_json_get(tab, "target");
            const char *k = tg ? nx_json_str(nx_json_get(tg, "kind")) : NULL;
            by_thread[th + "|" + (k ? k : "?")].push_back(id);
        }
    }

    /* 同じ thread を 2 つのタブが指している = 複製。片方だけ残す。 */
    for (auto &kv : by_thread) {
        if (kv.second.size() < 2) continue;
        for (size_t i = 1; i < kv.second.size(); i++)
            items.push_back({kv.second[i], kv.first, "同じ thread + 種別の複製タブ", 2});
    }

    int n_orphan = 0, n_identity = 0, n_dup = 0, n_empty = 0;
    for (auto &it : items) {
        if (it.kind == 0) n_orphan++;
        else if (it.kind == 1) n_identity++;
        else if (it.kind == 2) n_dup++;
        else n_empty++;
    }

    fprintf(out, "PROCEDURE\ttabs=%zu\torphan=%d\tidentity=%d\tduplicate=%d\tempty=%d\n",
            tabs->n, n_orphan, n_identity, n_dup, n_empty);

    /* 手順は「安全な順」に導出する。個別のタブ名には依存しない。
         1. identity の正規化 (中身は消さない。id を実体に合わせる)
         2. duplicate の除去 (残すのは先頭 = 表示中のもの)
         3. empty draft の除去
         4. orphan surface の除去
         5. 永続層へ 1 回だけ書く
         6. 全ウィンドウの live store を同じ値に
         7. 再観測して VERDICT
       1〜4 はどれも「観測から決まる」。実行は clean が同じ判定で行う。 */
    int step = 0;
    if (n_identity) fprintf(out, "  STEP %d normalize  identity=%d  guard=none   (id を threadRef に合わせる)\n", ++step, n_identity);
    if (n_dup)      fprintf(out, "  STEP %d dedupe     duplicate=%d guard=keep-first\n", ++step, n_dup);
    if (n_empty)    fprintf(out, "  STEP %d drop       empty=%d     guard=unreferenced\n", ++step, n_empty);
    if (n_orphan)   fprintf(out, "  STEP %d drop       orphan=%d    guard=owner-missing\n", ++step, n_orphan);
    fprintf(out, "  STEP %d persist    once         guard=seq+1\n", ++step);
    fprintf(out, "  STEP %d live       all-windows  guard=no-reload\n", ++step);
    fprintf(out, "  STEP %d verify     advise       must=VERDICT=CLEAN\n", ++step);

    for (auto &it : items)
        fprintf(out, "  ITEM kind=%d id=%s thread=%s  %s\n",
                it.kind, it.id.c_str(), it.thread.c_str(), it.reason.c_str());

    /* 根拠の強さ: 観測だけで決まるものは high。 */
    fprintf(out, "CONFIDENCE\t%s\n",
            (n_orphan + n_identity + n_dup + n_empty) ? "high" : "high(no-op)");

    nx_json_free(root);
    return 0;
}

extern "C" int nx_advisor_run(const char *state_path, int apply, FILE *out) {
    if (nx_advisor_derive(state_path, out) != 0) return -1;
    if (!apply) {
        fprintf(out, "  (dry-run: 実行しない。--apply で clean を 1 回だけ呼ぶ)\n");
        return 0;
    }
    /* 実行は既存の 1 つの永続手続き (clean) に委ねる。ここで二重実装しない。 */
    nx_clean_opts_t o;
    nx_clean_opts_default(&o);
    o.live = 1;
    o.dry_run = 0;
    o.notes_out = out;
    fprintf(out, "EXEC\tclean\n");
    int rc = nx_clean_state(state_path, &o, out);
    fprintf(out, "RE-DERIVE\t");
    nx_advisor_derive(state_path, out);
    return rc;
}
