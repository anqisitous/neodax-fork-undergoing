#ifndef NX_CLEAN_HPP
#define NX_CLEAN_HPP

/*
 * nx_clean — 「消えたはずのものが残っている」状態を、観測から選んだ手順で消す。
 *
 * 位置づけ:
 *   clean は新しい修復の実装を増やさない。既存の部品を、状態から選んで
 *   順に呼ぶだけの薄い層にする (二重実装を作らない)。
 *
 *       観測 (nx_residual / nx_state)
 *         -> 種類の判定 (nx_miss_kind_t)
 *         -> 安全度の見積り (nx_residual_risk_t)
 *         -> 呼ぶコマンド列の選択
 *         -> 実行 (leveldb 追記 / tombstone / nx_appfix)
 *         -> 再観測して VERDICT を出す
 *
 *   種類が増えたら nx_residual の収集を増やす。呼ぶコマンドは
 *   「何が残っているか」から決まるので、clean 側に個別の分岐を足さない。
 *
 * 既定の安全側:
 *   - dry-run が既定ではないが、--dry-run で必ず何も変えずに出せる。
 *   - 稼働中ストアへの反映は既定で行う (--no-live で止める)。
 *     reload / 再起動は一切しない。
 *   - seq は実測最大+1 のみ。桁違いの値は書かない。
 */

#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int         live;        /* 稼働中ストアへも同じ値を反映する (既定 1) */
    int         dry_run;     /* 変更しない (既定 0) */
    FILE       *notes_out;   /* 選択した段を機械可読で出す先 (NULL 可) */
    const char *app;         /* 稼働中プロセスの名前 (NULL なら既定) */
    const char *store;       /* ストアのアセット名の一部 (NULL なら既定) */
    const char *key;         /* localStorage キー (NULL なら既定) */
} nx_clean_opts_t;

void nx_clean_opts_default(nx_clean_opts_t *o);

/* 戻り値: 0=健全 (CLEAN), 1=残った (STILL_DIRTY), 2=実行不能 */
int nx_clean_state(const char *state_path, const nx_clean_opts_t *opts, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_CLEAN_HPP */
