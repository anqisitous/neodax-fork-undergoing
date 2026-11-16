#ifndef NX_RESIDUAL_HPP
#define NX_RESIDUAL_HPP

/*
 * nx_residual — 状態に残った「残滓」を状態の形に依存せず集める器。
 *
 * なぜ要るか:
 *   clean を「タブを消す」専用にすると、残滓の種類が増えるたびに
 *   判定を書き足すことになる (個別対応)。
 *   残滓は「タブ」ではなく「参照できない実体」であることが多い:
 *     面 (surface) の所有者が消えた / 会話が消えた / 環境が消えた /
 *     重複した / 上限を超えた / 参照が実際とは違う …
 *
 *   そこで、状態 JSON を 1 回だけ辿る汎用の走査を行い、
 *   見つかった実体 (entity) と、「その実体が参照する相手が状態内に
 *   存在するか (resolution)」を並べる。判定はこの 2 つだけで足りる。
 *
 * 意味 (これを守れば新しい残滓の種類も自動で拾える):
 *   - entity     : 状態が持つ実体 (id + kind + 所有者参照)
 *   - reference  : entity が指す相手 (kind + 期待される id)
 *   - unresolved : reference の相手が状態に存在しない => 残滓
 *   - duplicate  : 同じ id / 同じ正準 (kind+surface+owner) が複数 => 残滓
 *
 *   「消してよいか」はここでは言わない。判定は advise に委ねる
 *   (安全度の判断を 2 箇所に持たない)。
 */

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 つの実体。 */
typedef struct {
    size_t      index;          /* tabs 配列内の位置                            */
    char        id[192];        /* 状態が振った id                              */
    char        kind[64];       /* "chat" / "surface" / "draft" / 任意          */
    char        label[192];     /* 表示用 (surfaceId 等)                        */
    int         has_owner;      /* owner 参照を持つか                           */
    char        owner_env[160];
    char        owner_thread[160];
    int         owner_resolved; /* 同じ owner の「生存しうる」実体が状態にあるか */
    char        canonical[512]; /* 正準キー (重複検出用)                        */
} nx_residual_entity_t;

/* 未解決の参照 1 件。 */
typedef struct {
    size_t      entity_index;   /* どの実体が持つ参照か                          */
    char        entity_id[192];
    char        kind[64];       /* 参照の種類 (owner / surface / environment)    */
    char        want_env[160];
    char        want_id[160];
    char        why[200];       /* なぜ未解決と判定したか                        */
} nx_residual_ref_t;

/* 重複 1 件 (canonical が同じ 2 つ以上の実体)。 */
typedef struct {
    char        canonical[512];
    size_t      count;
    size_t      first_index;
} nx_residual_dup_t;

typedef struct {
    nx_residual_entity_t *entities;
    size_t                n_entities;
    nx_residual_ref_t    *refs;      /* 未解決の参照だけ */
    size_t                n_refs;
    nx_residual_dup_t    *dups;
    size_t                n_dups;
    size_t                n_tabs;    /* tabs 配列の元の長さ */
} nx_residual_map_t;

/* state_json を 1 回辿って map を作る。戻り値 0=成功。
   map は nx_residual_free で解放する。 */
int  nx_residual_collect(const char *state_json, nx_residual_map_t *out);
void nx_residual_free(nx_residual_map_t *m);

/* 残滓 (未解決参照 + 重複) を持つ実体の index 集合を返す。
   戻り値は件数。out は呼び出し側 free。 */
int  nx_residual_stale_indices(const nx_residual_map_t *m, int **out);

/* 「何が残っているか」を人間可読で出す。変更はしない。 */
int  nx_residual_dump(const char *source, const nx_residual_map_t *m, FILE *out);

/* 状態 JSON から、指定した index 集合の実体だけを取り除いた JSON を作る。
   それ以外 (base / version / 他タブ / 未知フィールド) はバイト列のまま保持する。
   activeId / recentIds が指す実体が消えた場合は、残る先頭へ付け替える。
   戻り値 0=成功, 1=除去対象なし, 負値=エラー。 */
int  nx_residual_prune_json(const char *state_json, const int *indices, int n_indices,
                            char **out);

#ifdef __cplusplus
}
#endif

/* リスク (安全度) の見積り。呼ぶコマンドを状態から選ぶための材料。
   すべて 0..1 で、高いほど「見落とすと害が大きい / 戻せない」。 */
typedef struct {
    double destructive;   /* 値を消す (戻すには再書き込みが要る)          */
    double order_sensitive; /* 適用順序を誤ると読めなくなる (WAL/seq/backup) */
    double live_sensitive;  /* 稼働中ストアとの不一致を残す (次の persist で戻る) */
    double subtle;          /* 見た目で気づきにくい (重複・別キーへの退避)     */
} nx_residual_risk_t;

nx_residual_risk_t nx_residual_risk(const nx_residual_map_t *m);

/* 見落としの種類。呼ぶコマンドを選ぶ理由づけ。 */
typedef enum {
    NX_MISS_NONE = 0,
    NX_MISS_ORPHAN_REF,    /* 所有者不在 (目に見えるが閉じられない) */
    NX_MISS_DUPLICATE,     /* 同じものが 2 つ (UI では区別が付かない) */
    NX_MISS_STORE_SPLIT,   /* 退避コピー側が解決先になっている */
    NX_MISS_STALE_BACKUP,  /* 退避コピーが残っている (いまは無害でも復活源になる) */
    NX_MISS_WAL_DAMAGE,    /* 追記の末尾が壊れて読めない */
} nx_miss_kind_t;

nx_miss_kind_t nx_residual_miss_kind(const nx_residual_map_t *m);

#endif /* NX_RESIDUAL_HPP */
