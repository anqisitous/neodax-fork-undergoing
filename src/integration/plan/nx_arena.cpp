#include "nx_arena.hpp"
#include "nx_residual.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>

namespace nx {

const char *move_name(Move m) {
    switch (m) {
        case Move::None:        return "none";
        case Move::Prune:       return "prune";
        case Move::SyncLive:    return "sync-live";
        case Move::PurgeBackup: return "purge-backup";
        case Move::RepairSeq:   return "repair-seq";
        case Move::Block:       return "block";
        default:                return "?";
    }
}

void arena_key(const Board &b, char *out, size_t cap) {
    /* board の「形」だけを key にする。数そのものではなく、
       0 か非 0 かで丸める。同じ形なら同じ結果になる。 */
    std::snprintf(out, cap, "t%d_s%d_r%d_d%d_b%d_q%d",
                  b.n_tabs > 0 ? 1 : 0,
                  b.n_stale  > 0 ? 1 : 0,
                  b.n_refs   > 0 ? 1 : 0,
                  b.n_dups   > 0 ? 1 : 0,
                  b.backup_alive ? 1 : 0,
                  b.seq_poisoned ? 1 : 0);
}

bool arena_seed(const char *state_json, Board *out) {
    if (!state_json || !out) return false;
    nx_residual_map_t m;
    if (nx_residual_collect(state_json, &m) != 0) return false;

    std::memset(out, 0, sizeof *out);
    out->id = 0;
    out->player = 0;
    out->n_tabs = (int)m.n_tabs;
    out->n_refs = (int)m.n_refs;
    out->n_dups = (int)m.n_dups;
    int *stale = nullptr;
    out->n_stale = nx_residual_stale_indices(&m, &stale);
    std::free(stale);
    out->parent = -1;
    out->last_move = Move::None;
    out->depth = 0;
    out->score = out->finished() ? 1000 : -(out->n_stale * 10 + out->n_refs * 5 + out->n_dups * 5);
    nx_residual_free(&m);
    return true;
}

/* テンプレートの引き当て。同じ形 + 同じ手なら、前の結果を使う。
   これがあるので「この手が動くか」を毎回予想しなくてよい。 */
static Template *tpl_find(std::vector<Template> &list, const char *key, Move m) {
    for (auto &t : list)
        if (t.move == m && std::strcmp(t.key, key) == 0) return &t;
    return nullptr;
}

static void tpl_remember(std::vector<Template> &list, const char *key, Move m,
                         const Board &from, Board &to, int cap) {
    Template t;
    std::memset(&t, 0, sizeof t);
    std::snprintf(t.key, sizeof t.key, "%s", key);
    t.move = m;
    t.d_stale = to.n_stale - from.n_stale;
    t.d_refs  = to.n_refs  - from.n_refs;
    t.d_dups  = to.n_dups  - from.n_dups;
    t.clears_backup = from.backup_alive && !to.backup_alive;
    t.clears_seq    = from.seq_poisoned && !to.seq_poisoned;
    t.hits = 1;
    if ((int)list.size() >= cap) list.erase(list.begin());   /* 古いものから捨てる */
    list.push_back(t);
}

/* 手を適用する。ここは純粋な計算。状態は書かない。
   テンプレートがあれば、それで結果を作る (速い)。
   無ければ規則で計算し、結果をテンプレートとして覚える。 */
bool arena_apply(const Board &from, Move m, Board *out) {
    if (!out) return false;
    *out = from;
    out->last_move = m;
    out->depth = from.depth + 1;
    out->parent = from.id;

    switch (m) {
        case Move::Prune:
            /* 残滓を全部除く。1 手で終わる。 */
            out->n_stale = 0;
            out->n_refs = 0;
            break;
        case Move::SyncLive:
            /* 稼働中を合わせる。残滓自体は減らない。 */
            break;
        case Move::PurgeBackup:
            out->backup_alive = false;
            break;
        case Move::RepairSeq:
            out->seq_poisoned = false;
            break;
        case Move::Block:
            /* 相手を finish させないため、board を崩す。
               残滓を 1 つ増やす (相手の finish を遠ざける)。 */
            out->n_stale = from.n_stale + 1;
            out->n_refs = from.n_refs + (from.n_refs > 0 ? 0 : 1);
            break;
        default:
            break;
    }
    out->score = out->finished()
        ? 1000 + (64 - out->depth > 0 ? 64 - out->depth : 0)
        : -(out->n_stale * 10 + out->n_refs * 5 + out->n_dups * 5);
    return true;
}

/* その board で取りうる手。形から決まるので、観測はいらない。 */
static int legal_moves(const Board &b, int player, Move *out, int cap) {
    int n = 0;
    auto add = [&](Move m) { if (n < cap) out[n++] = m; };

    if (player == 0) {
        /* resolve: finish に近づく手だけ。 */
        if (b.n_stale > 0 || b.n_refs > 0 || b.n_dups > 0) add(Move::Prune);
        if (b.backup_alive) add(Move::PurgeBackup);
        if (b.seq_poisoned) add(Move::RepairSeq);
        if (b.n_stale == 0 && b.n_refs == 0 && b.n_dups == 0) add(Move::SyncLive);
    } else {
        /* block: resolve の finish を妨げる手。 */
        add(Move::Block);
    }
    if (n == 0) add(Move::None);
    return n;
}

int arena_run(const char *state_json, const char *player_a, const char *player_b,
              const ArenaConfig &cfg, ArenaResult *res, void *out_file) {
    FILE *o = out_file ? (FILE *)out_file : stdout;
    if (!res) return -1;
    std::memset(res, 0, sizeof *res);
    res->winner = -1;

    Board seed;
    if (!arena_seed(state_json, &seed)) {
        std::fprintf(o, "  [arena] 状態を board にできない\n");
        return 2;
    }

    /* 最初の seed は退避と seq を呼び側から受け取る必要がある。
       ここでは board の形だけで判断する (観測は 1 回で済ませる)。 */
    std::fprintf(o, "=== arena: %s vs %s ===\n",
                 player_a ? player_a : "resolve", player_b ? player_b : "block");
    std::fprintf(o, "seed: tabs=%d stale=%d refs=%d dups=%d backup=%d seq=%d\n",
                 seed.n_tabs, seed.n_stale, seed.n_refs, seed.n_dups,
                 seed.backup_alive, seed.seq_poisoned);

    std::vector<Board> boards;
    std::vector<Template> tpls;
    boards.push_back(seed);
    res->boards_made = 1;

    int max_boards = cfg.max_boards > 0 ? cfg.max_boards : 64;
    int max_depth  = cfg.max_depth  > 0 ? cfg.max_depth  : 8;
    int max_frames = cfg.max_frames > 0 ? cfg.max_frames : 32;

    /* ---- frame をまとめて進める ----
       1 frame = 1 回の「全 board を 1 手ずつ進める」処理。
       board を増やすのも、手を適用するのも、テンプレートで予想する。
       実際に状態を読むのは、finish した board を確かめるときだけ。 */
    int frame = 0;
    for (; frame < max_frames; frame++) {
        int before = (int)boards.size();

        /* resolve と block が交互に指す。1 frame で両方進める。 */
        for (int turn = 0; turn < 2; turn++) {
            /* 手番は交互。turn 0 が player_a、turn 1 が player_b。
               名前から役割を決める: "block" なら崩す側、それ以外は finish 側。 */
            const char *who = turn == 0 ? player_a : player_b;
            int player = (who && std::strcmp(who, "block") == 0) ? 1 : 0;

            size_t n_now = boards.size();
            for (size_t i = 0; i < n_now; i++) {
                if ((int)boards.size() >= max_boards) break;
                const Board &b = boards[i];
                if (b.player != player) continue;
                if (b.finished()) continue;
                if (b.depth >= max_depth) continue;

                Move legal[8];
                int nl = legal_moves(b, player, legal, 8);
                for (int k = 0; k < nl; k++) {
                    if ((int)boards.size() >= max_boards) break;

                    Board nb;
                    char key[96];
                    arena_key(b, key, sizeof key);
                    Template *t = tpl_find(tpls, key, legal[k]);
                    if (t) {
                        /* テンプレートで予想する。状態を読まない。 */
                        nb = b;
                        nb.last_move = legal[k];
                        nb.depth = b.depth + 1;
                        nb.parent = b.id;
                        nb.n_stale += t->d_stale;
                        nb.n_refs  += t->d_refs;
                        nb.n_dups  += t->d_dups;
                        if (t->clears_backup) nb.backup_alive = false;
                        if (t->clears_seq)    nb.seq_poisoned = false;
                        if (nb.n_stale < 0) nb.n_stale = 0;
                        if (nb.n_refs  < 0) nb.n_refs  = 0;
                        if (nb.n_dups  < 0) nb.n_dups  = 0;
                        nb.score = nb.finished()
                            ? 1000 + (64 - nb.depth > 0 ? 64 - nb.depth : 0)
                            : -(nb.n_stale * 10 + nb.n_refs * 5 + nb.n_dups * 5);
                        nb.id = (int)boards.size();
                        nb.player = 1 - player;   /* 手番を渡す */
                        t->hits++;
                        res->predicted++;
                    } else {
                        arena_apply(b, legal[k], &nb);
                        nb.id = (int)boards.size();
                        nb.player = 1 - player;
                        tpl_remember(tpls, key, legal[k], b, nb, cfg.templates > 0 ? cfg.templates : 256);
                        res->verified++;
                    }
                    boards.push_back(nb);
                    res->boards_made++;

                    if (nb.finished()) {
                        res->finished = true;
                        res->finish_depth = nb.depth;
                        res->winner = player;   /* finish させた側の勝ち */
                    }
                }
            }
        }

        /* 進みが無ければ frame を打ち切る (同じ board を回し続けない)。 */
        if ((int)boards.size() == before) break;
    }

    res->frames = frame;
    res->boards_live = (int)boards.size();

    /* ---- 結果 ---- */
    std::fprintf(o, "boards=%d frames=%d (max %d)\n",
                 res->boards_made, res->frames, max_frames);
    std::fprintf(o, "predicted=%d verified=%d (template hits=%zu)\n",
                 res->predicted, res->verified, tpls.size());

    /* 最良の board (finish に一番近いもの) と、そこへ至る手順。 */
    int best = -1;
    for (size_t i = 0; i < boards.size(); i++)
        if (best < 0 || boards[i].score > boards[best].score) best = (int)i;

    if (best >= 0) {
        std::fprintf(o, "best board #%d score=%d finished=%s depth=%d\n",
                     boards[best].id, boards[best].score,
                     boards[best].finished() ? "yes" : "no", boards[best].depth);
        /* 手順を逆にたどる。 */
        std::vector<const Board *> line;
        for (int i = best; i >= 0; i = boards[i].parent) line.push_back(&boards[i]);
        std::fprintf(o, "line:");
        for (int i = (int)line.size() - 1; i >= 0; i--) {
            if (line[i]->parent < 0) continue;
            std::fprintf(o, " %s", move_name(line[i]->last_move));
        }
        std::fprintf(o, "\n");
    }

    std::fprintf(o, "FINISHED=%s winner=%s\n",
                 res->finished ? "yes" : "no",
                 res->winner == 0 ? (player_a ? player_a : "resolve")
                 : res->winner == 1 ? (player_b ? player_b : "block") : "-");
    return res->finished ? 0 : 1;
}

} // namespace nx
