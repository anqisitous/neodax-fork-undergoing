#ifndef NX_STORE_FACTS_HPP
#define NX_STORE_FACTS_HPP

/*
 * nx_store_facts — 永続層について「何が事実か」を 1 箇所で決める核心ノード。
 *
 * なぜ要るか:
 *   状態の判定 (clean / keys / tabs / splice) が、それぞれ別のやり方で
 *   「キーが生きているか」「退避が残っているか」を推測していた。
 *   その結果、同じ状態について 3 つの指標が食い違った:
 *
 *     RESOLVED_IS_BACKUP  find_key の戻り値の末尾  → 常に 0 (退避は除外される)
 *     backup_exists       list_keys (seq を見ない) → 常に 1 (一度でも存在すれば真)
 *     keys の LIVE        key_is_live (seq 比較)   → 実際の生存
 *
 *   食い違いは「根拠が違う」ことから来る。根拠を 1 つにすれば食い違わない。
 *   このノードがその 1 つになる。
 *
 * 設計:
 *   事実 (fact) と、そこから導く判断 (judge) を分けて持つ。
 *     fact   : 観測しただけ。seq と削除レコードを見て「今の値」を決める。
 *     judge  : fact と規則から出す。根拠 (evidence) と予想 (expect) を添える。
 *   判断は「根拠 → 予想 → 結論」の組で返すので、後から検証できる。
 *
 * 安定性:
 *   この API は状態の形 (leveldb) に依存するが、呼ぶ側 (clean/keys/...) の
 *   都合には依存しない。新しい判定を足すときも、ここに fact を 1 つ増やす。
 */

#include <cstddef>

namespace nx {

/* ---- 事実: 観測しただけ ---- */
struct StoreFact {
    const char *name;       /* "canonical" / "backup" / "max_seq" */
    bool  exists;           /* キーが (seq を考慮して) 今生きているか */
    bool  resolved;         /* 読み書きの解決先か */
    unsigned long long seq; /* 最後に見た seq (0 = 不明) */
    const char *note;       /* 何を根拠にしたか (1 行) */
};

struct StoreFacts {
    StoreFact canonical;    /* workspace-tabs 本体 */
    StoreFact backup;       /* workspace-tabs .nx-backup (退避) */
    unsigned long long max_seq;
    int  is_dir;            /* leveldb ディレクトリか */
    int  n_facts;
    StoreFact facts[8];
};

/* 観測する。状態には書かない。 */
StoreFacts store_observe(const char *state_path);

/* ---- 判断: 根拠 → 予想 → 結論 ---- */
enum class Judge {
    Ok = 0,          /* 何もしなくてよい */
    StaleBackup,     /* 退避が残っている (いま読まれないが復活源になる) */
    Split,           /* 解決先が退避になっている */
    SeqPoisoned,     /* seq が桁違い */
    Unreadable,      /* 状態が読めない */
};

struct StoreVerdict {
    Judge       judge;
    const char *evidence;   /* 何を見てそう言えるか */
    const char *expect;     /* そう判断すると何が起きるはずか */
    int         confidence; /* 0..100 */
};

/* facts から判断する。規則はここ 1 箇所。 */
StoreVerdict store_judge(const StoreFacts &f);

/* 表示用の名前。 */
const char *store_judge_name(Judge j);

} // namespace nx

#endif /* NX_STORE_FACTS_HPP */
