#include "nx_sim.h"
#include "nx_json.h"
#include "nx_deepai.h"   /* nx_asar_* : profile 抽出用 (任意) */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* プロファイル                                                        */
/* ------------------------------------------------------------------ */

const nx_sim_profile_t *nx_sim_profile_default(void) {
    static const nx_sim_profile_t p = {
        "generic-tabs",
        "kind", "surfaceId", "threadRef", "environmentId", "threadId",
        "chat,draft,page,new-tab", "surface",
        1
    };
    return &p;
}

const nx_sim_profile_t *nx_sim_profile_deepai(void) {
    static const nx_sim_profile_t p = {
        "deepai-workspace-tabs",
        "kind", "surfaceId", "threadRef", "environmentId", "threadId",
        "chat", "surface",
        1
    };
    return &p;
}

/* ------------------------------------------------------------------ */
/* 小さなヘルパ                                                        */
/* ------------------------------------------------------------------ */

static int list_has(const char *csv, const char *v) {
    if (!csv || !v) return 0;
    size_t vl = strlen(v);
    const char *p = csv;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        if (n == vl && strncmp(p, v, n) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

static const nx_json_t *state_node(const nx_json_t *root) {
    const nx_json_t *st = nx_json_get(root, "state");
    if (st && nx_json_get(st, "tabs")) return st;
    if (nx_json_get(root, "tabs")) return root;
    return NULL;
}

typedef struct {
    const char *id;
    const char *kind;
    const char *surface;
    const char *owner_env;
    const char *owner_thread;
    int         has_owner;
} sim_tab_t;

/* 表示用 owner 文字列を組み立てる (戻り値は静的な交互バッファ) */
static const char *owner_label(const sim_tab_t *t) {
    static char buf[4][340];
    static int turn = 0;
    turn = (turn + 1) & 3;
    if (!t->has_owner) { snprintf(buf[turn], sizeof(buf[turn]), "-"); return buf[turn]; }
    snprintf(buf[turn], sizeof(buf[turn]), "%s/%s",
             t->owner_env ? t->owner_env : "?",
             t->owner_thread ? t->owner_thread : "?");
    return buf[turn];
}

static int owner_eq(const sim_tab_t *a, const sim_tab_t *b) {
    if (!a->has_owner || !b->has_owner) return 0;
    return a->owner_env && b->owner_env && a->owner_thread && b->owner_thread &&
           strcmp(a->owner_env, b->owner_env) == 0 &&
           strcmp(a->owner_thread, b->owner_thread) == 0;
}

static int owner_known(const sim_tab_t *tabs, int n, const sim_tab_t *s,
                       const nx_sim_profile_t *p) {
    for (int i = 0; i < n; i++) {
        if (!list_has(p->chat_kinds, tabs[i].kind)) continue;
        if (owner_eq(&tabs[i], s)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* asar からプロファイル手掛かりを抽出 (任意)                          */
/* ------------------------------------------------------------------ */

static int contains(const char *h, const char *n) {
    return h && n && strstr(h, n) != NULL;
}

static void extract_profile_hints(const char *asar_path, FILE *out) {
    nx_asar_t *a = nx_asar_open(asar_path);
    if (!a) {
        fprintf(out, "    [rules] asar を開けません (既定プロファイルで継続): %s\n", asar_path);
        return;
    }
    const nx_asar_entry_t *store = nx_asar_find(a, "WorkspaceTabStore-");
    const nx_asar_entry_t *tabs_e = nx_asar_find(a, "WorkspaceTabs-");
    char *js = store ? nx_asar_read_text(a, store) : NULL;
    char *tjs = tabs_e ? nx_asar_read_text(a, tabs_e) : NULL;

    fprintf(out, "    [rules] profile hints from %s\n", asar_path);
    if (store) fprintf(out, "      store bundle : %s\n", store->path);
    if (contains(js, "threadRef") && contains(js, "environmentId"))
        fprintf(out, "      owner keys   : threadRef{environmentId,threadId}  [detected]\n");
    if (contains(js, "surfaceId"))
        fprintf(out, "      surface key  : surfaceId                          [detected]\n");
    if (contains(js, "workspace-tabs"))
        fprintf(out, "      storage key  : *:workspace-tabs:v1                 [detected]\n");
    if (contains(js, "slice(0,200)") || contains(js, "0,200"))
        fprintf(out, "      tab limit    : 200                                [detected]\n");
    if (contains(tjs, "byThreadKey"))
        fprintf(out, "      aggregation  : all threads (byThreadKey)          [detected]\n");
    if (contains(tjs, "Open the owning chat first"))
        fprintf(out, "      close guard  : owner != current -> abort          [detected]\n");
    free(js);
    free(tjs);
    nx_asar_free(a);
}

/* ------------------------------------------------------------------ */
/* 本体                                                                */
/* ------------------------------------------------------------------ */

int nx_sim_run(const char *state_json, const nx_sim_profile_t *profile,
               const char *asar_path, FILE *out) {
    if (!out) out = stdout;
    if (!profile) profile = nx_sim_profile_default();
    if (!state_json) { fprintf(out, "  [sim] no state\n"); return -1; }

    fprintf(out, "\n  === app-state simulation [profile: %s] ===\n", profile->name);
    if (asar_path) extract_profile_hints(asar_path, out);

    nx_json_t *root = nx_json_parse(state_json, strlen(state_json));
    if (!root) { fprintf(out, "  [sim] state is not valid JSON\n"); return -1; }
    const nx_json_t *st = state_node(root);
    const nx_json_t *tabs = st ? nx_json_get(st, "tabs") : NULL;
    if (!tabs || tabs->type != NX_JSON_ARR) {
        fprintf(out, "  [sim] no tabs array\n");
        nx_json_free(root);
        return -1;
    }

    /* --- step 1: migrate / validate (store の u()/d() 相当) --- */
    sim_tab_t *vt = (sim_tab_t *)calloc(tabs->n + 1, sizeof(*vt));
    int n = 0, dropped = 0, dup = 0;
    char **seen_ids = (char **)calloc(tabs->n + 1, sizeof(char *));
    char **seen_keys = (char **)calloc(tabs->n + 1, sizeof(char *));
    int n_ids = 0, n_keys = 0;
    size_t limit = 200;

    for (size_t i = 0; i < tabs->n && i < limit; i++) {
        const nx_json_t *tab = nx_json_at(tabs, i);
        const nx_json_t *tg = nx_json_get(tab, "target");
        const char *id = nx_json_str(nx_json_get(tab, "id"));
        const char *kind = nx_json_str(nx_json_get(tg, profile->kind_key));
        const char *surf = nx_json_str(nx_json_get(tg, profile->surface_key));
        const nx_json_t *own = nx_json_get(tg, profile->owner_key);
        const char *env = nx_json_str(nx_json_get(own, profile->owner_env_key));
        const char *th  = nx_json_str(nx_json_get(own, profile->owner_thread_key));
        int is_surface = list_has(profile->surface_kinds, kind);
        int is_chat = list_has(profile->chat_kinds, kind);
        int has_owner = (env && th);

        if (!id || !id[0] || !kind) { dropped++; continue; }
        if ((is_surface || is_chat) && !has_owner) { dropped++; continue; }
        if (is_surface && (!surf || !surf[0])) { dropped++; continue; }

        int is_dup = 0;
        for (int k = 0; k < n_ids; k++) if (strcmp(seen_ids[k], id) == 0) is_dup = 1;
        char key[512];
        snprintf(key, sizeof(key), "%s|%s|%s|%s", kind,
                 surf ? surf : "", env ? env : "", th ? th : "");
        for (int k = 0; k < n_keys; k++) if (strcmp(seen_keys[k], key) == 0) is_dup = 1;
        if (is_dup) { dup++; continue; }

        seen_ids[n_ids++] = (char *)id;
        {
            size_t kl = strlen(key) + 1;
            char *kc = (char *)malloc(kl);
            if (kc) memcpy(kc, key, kl);
            seen_keys[n_keys++] = kc;
        }
        sim_tab_t t;
        memset(&t, 0, sizeof(t));
        t.id = id; t.kind = kind; t.surface = surf;
        t.owner_env = env; t.owner_thread = th; t.has_owner = has_owner;
        vt[n++] = t;
    }
    for (int k = 0; k < n_keys; k++) free(seen_keys[k]);
    free(seen_ids);
    free(seen_keys);

    const char *active_id = nx_json_str(nx_json_get(st, "activeId"));
    fprintf(out, "\n  [1] load/migrate: %d tabs kept (%d invalid, %d duplicate dropped, limit %zu)\n",
            n, dropped, dup, limit);
    for (int i = 0; i < n; i++)
        fprintf(out, "      %-9s %-30s owner=%s\n",
                vt[i].kind,
                vt[i].surface ? vt[i].surface : (vt[i].id ? vt[i].id : "?"),
                owner_label(&vt[i]));

    /* --- step 2: surface 集約 (全 owner) --- */
    int cur = -1;
    for (int i = 0; i < n; i++)
        if (active_id && vt[i].id && strcmp(vt[i].id, active_id) == 0) { cur = i; break; }
    fprintf(out, "\n  [2] tab strip aggregation\n");
    fprintf(out, "      activeId   : %s\n", active_id ? active_id : "(none)");
    fprintf(out, "      current    : %s\n", cur >= 0 ? owner_label(&vt[cur]) : "(none)");
    for (int i = 0; i < n; i++) {
        if (!list_has(profile->surface_kinds, vt[i].kind)) continue;
        int same = (cur >= 0) && owner_eq(&vt[cur], &vt[i]);
        fprintf(out, "      surface %-28s owner=%s%s\n",
                vt[i].surface ? vt[i].surface : "?", owner_label(&vt[i]),
                same ? "  [current owner]" : "  [other owner]");
    }

    /* --- step 3: close シミュレーション --- */
    fprintf(out, "\n  [3] close-tab simulation\n");
    int n_stuck = 0;
    for (int i = 0; i < n; i++) {
        if (!list_has(profile->surface_kinds, vt[i].kind)) {
            fprintf(out, "      %-9s %-28s -> closable\n", vt[i].kind,
                    vt[i].id ? vt[i].id : "?");
            continue;
        }
        int same = (cur >= 0) && owner_eq(&vt[cur], &vt[i]);
        int known = owner_known(vt, n, &vt[i], profile);
        const char *verdict;
        if (!profile->close_requires_owner) verdict = "closable";
        else if (same) verdict = "closable (current owner)";
        else if (known) verdict = "closable after navigation to owner";
        else { verdict = "STUCK (owner unreachable; close aborts)"; n_stuck++; }
        fprintf(out, "      surface %-28s owner=%s -> %s\n",
                vt[i].surface ? vt[i].surface : "?", owner_label(&vt[i]), verdict);
    }

    fprintf(out, "\n  [result] %d stuck surface tab(s)\n", n_stuck);
    if (n_stuck)
        fprintf(out, "           fix: remove those tabs from state (base components untouched)\n");

    free(vt);
    nx_json_free(root);
    return 0;
}
