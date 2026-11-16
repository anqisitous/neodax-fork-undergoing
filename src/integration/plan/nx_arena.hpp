#ifndef NX_ARENA_HPP
#define NX_ARENA_HPP

/*
 * nx_arena — board を大量に持って、frame をまとめて進める探索。
 *
 * 何をするか:
 *   いまの状態を 1 枚の board (スナップショット) として記録し、
 *   そこから「取りうる操作」を適用した board を大量に作る。
 *   それぞれの board を先へ進め、先に finish (問題が消えた状態) に
 *   到達する手順を探す。
 *
 * なぜテンプレートか:
 *   操作が実際に動くかを 1 手ごとに予想すると、毎回の観測 (wait) が
 *   必要になり、リアルタイムに間に合わない。そこで
 *   「この board の形なら、この操作はこの結果になる」という型を
 *   先に作っておき (snapshot template)、動かさずに結果を予想する。
 *   予想が外れたときだけ実際に確かめる。
 *
 * 二人の player:
 *   ・player P (resolve) : 自分の board を finish させたい
 *   ・player Q (block)   : 相手を finish させたくない (board を崩す)
 *   互いに frame を進め、どちらかが finish したら終わる。
 *   input/output は取らない (人を介さない)。
 *
 * 速さ:
 *   1 frame で全 board を進める (board ごとに 1 手ずつ、まとめて)。
 *   実際の観測は frame の切れ目でだけ行う。board の予想は
 *   テンプレートの引き当てだけで済ませるので、メモリ上で完結する。
 */

#include <cstddef>

namespace nx {

/* ---- 操作 (move) ---- */
enum class Move {
    None = 0,
    Prune,        /* 残滓を除く (persist) */
    SyncLive,     /* 稼働中ストアを同じにする (live-sync) */
    PurgeBackup,  /* 退避を消す (purge-backup) */
    RepairSeq,    /* seq を実測最大+1 に直す (repair-seq) */
    Block,        /* 相手の finish を妨げる (board を崩す) */
    Count
};

const char *move_name(Move m);

/* ---- board (1 枚の状態) ---- */
struct Board {
    int  id;             /* 何番の board か */
    int  player;         /* 0 = resolve, 1 = block */
    int  n_tabs;         /* タブ数 */
    int  n_stale;        /* 残滓の数 */
    int  n_refs;         /* 未解決参照 */
    int  n_dups;         /* 重複 */
    bool backup_alive;   /* 退避キーが生きているか */
    bool seq_poisoned;   /* seq が桁違いか */
    int  depth;          /* 何手進んだか */
    int  parent;         /* どの board から来たか (-1 = 開始) */
    Move last_move;      /* 最後に指した手 */
    int  score;          /* 大きいほど finish に近い */

    bool finished() const {
        return n_stale == 0 && n_refs == 0 && n_dups == 0 &&
               !backup_alive && !seq_poisoned;
    }
};

/* ---- snapshot template ----
   同じ形の board には同じ結果を返す。実測して覚える。
   key は「board の形」を表す短い文字列。 */
struct Template {
    char  key[96];
    Move  move;
    int   d_stale;    /* その手で残滓がどう変わるか */
    int   d_refs;
    int   d_dups;
    int   clears_backup;
    int   clears_seq;
    int   hits;       /* 何回当たったか */
};

/* ---- arena (探索の器) ---- */
struct ArenaConfig {
    int max_boards;     /* 同時に持つ board の上限 */
    int max_frames;     /* 何 frame まで進めるか */
    int max_depth;      /* 1 つの board を何手まで伸ばすか */
    int templates;      /* テンプレートを何件まで覚えるか */
};

struct ArenaResult {
    int  frames;        /* 使った frame 数 */
    int  boards_made;   /* 作った board の総数 */
    int  boards_live;   /* 最後まで残った board 数 */
    int  predicted;     /* テンプレートで済ませた手数 (観測せず) */
    int  verified;      /* 実際に確かめた手数 */
    int  winner;        /* 0 = resolve, 1 = block, -1 = 引き分け/未完 */
    int  finish_depth;  /* finish まで何手だったか */
    bool finished;      /* finish に到達したか */
};

/* 状態から board を作る (観測はここで 1 回だけ)。 */
bool arena_seed(const char *state_json, Board *out);

/* 指定の move を適用した board を作る (メモリ上だけ。状態は書かない)。 */
bool arena_apply(const Board &from, Move m, Board *out);

/* board の形を表す key (テンプレートの引き当て用)。 */
void arena_key(const Board &b, char *out, size_t cap);

/* 探索する。状態には書かない。
   player_a / player_b は "resolve" / "block" など、誰を動かすか。 */
int arena_run(const char *state_json, const char *player_a, const char *player_b,
              const ArenaConfig &cfg, ArenaResult *out, void *out_file);

} // namespace nx

#endif /* NX_ARENA_HPP */
