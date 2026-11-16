#include "nx_store_facts.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>

extern "C" {
#include "../nx_state.h"
}

namespace nx {

/* 状態パスから leveldb のディレクトリを得る (CURRENT の有無で判定)。 */
static void store_dir(const char *state_path, char *out, size_t cap, int *is_dir) {
    std::snprintf(out, cap, "%s", state_path ? state_path : "");
    *is_dir = 0;
    if (!state_path || !*state_path) return;
    char cur[4200];
    std::snprintf(cur, sizeof cur, "%s/CURRENT", state_path);
    struct stat st;
    if (stat(cur, &st) == 0) { *is_dir = 1; return; }
    /* ファイルを指していたら親をディレクトリとみなす */
    char *sl = std::strrchr(out, '/');
    if (sl) { *sl = '\0'; *is_dir = 1; }
}

StoreFacts store_observe(const char *state_path) {
    StoreFacts f;
    std::memset(&f, 0, sizeof f);

    char dir[4096];
    store_dir(state_path, dir, sizeof dir, &f.is_dir);
    if (!*dir) return f;

    /* 本体: seq を見て「今生きているか」を決める。
       list_keys ではなく key_is_live を使うのが要点。
       list_keys は seq も削除も見ないので「一度でも存在したキー」を返す。 */
    f.canonical.name = "canonical";
    f.canonical.exists = nx_state_key_is_live(dir, "workspace-tabs") != 0;
    f.canonical.resolved = 1;   /* 正準キーは常に解決先 (find_key が退避を除外する) */
    f.canonical.note = "nx_state_key_is_live(seq 比較で最後の put/delete を見る)";

    /* 退避: 同じく seq で見る。ここが以前 backup_exists で誤っていた箇所。 */
    f.backup.name = "backup";
    f.backup.exists = nx_state_key_is_live(dir, "nx-backup") != 0;
    f.backup.resolved = 0;      /* find_key が除外するので解決先にはならない */
    f.backup.note = "nx_state_key_is_live(seq 比較。list_keys は削除を見ない)";

    f.max_seq = nx_state_max_seq(dir, "workspace-tabs");

    /* seq は代表値だけ入れる (個別 seq は list_keys では取れないため不明=0) */
    f.canonical.seq = f.max_seq;
    f.backup.seq = 0;

    f.facts[0] = f.canonical;
    f.facts[1] = f.backup;
    f.n_facts = 2;
    return f;
}

const char *store_judge_name(Judge j) {
    switch (j) {
        case Judge::Ok:           return "ok";
        case Judge::StaleBackup:  return "stale-backup";
        case Judge::Split:        return "store-split";
        case Judge::SeqPoisoned:  return "seq-poisoned";
        case Judge::Unreadable:   return "unreadable";
    }
    return "?";
}

StoreVerdict store_judge(const StoreFacts &f) {
    /* 規則はここ 1 箇所。各判定に「根拠」と「予想」を必ず添える。
       根拠が無い判定は返さない (推測で動かない)。 */

    if (f.n_facts == 0 || !f.is_dir) {
        return { Judge::Unreadable, "leveldb のディレクトリが見当たらない",
                 "読み書きする対象が無い", 100 };
    }

    /* 桁違いの seq。以後の書き込みが読まれなくなる。 */
    if (f.max_seq >= 100000000ULL) {
        return { Judge::SeqPoisoned,
                 "max_seq が 1e8 以上 (通常の追記ではありえない値)",
                 "実測最大+1 で書き直さないと以後の書き込みが無視される", 100 };
    }

    /* 解決先が退避。find_key は退避を除外するので通常は起きないが、
       古い状態が残っている場合に備えて事実だけ見る。 */
    if (f.backup.resolved) {
        return { Judge::Split, "退避キーが解決先になっている",
                 "本体を書いても退避側に吸われ、次に本体が書かれた瞬間に戻る", 100 };
    }

    /* 退避が「生きている」。これが今回の穴。
       以前は backup_exists (list_keys) を見ていたため、
       tombstone を書いても永久に真のままで冪等でなかった。 */
    if (f.backup.exists) {
        return { Judge::StaleBackup,
                 "退避キーが seq 上まだ生きている (削除レコードが最後ではない)",
                 "本体が次に書かれたとき、退避側が復活源になりうる", 90 };
    }

    return { Judge::Ok, "正準キーが生きていて、退避は削除済み",
             "このまま何もしなくてよい", 100 };
}

} // namespace nx
