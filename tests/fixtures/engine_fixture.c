/*
 * engine_fixture.c — test fixture for the advanced static analysis.
 *
 * A miniature search engine with the shape of a board-game AI (shogi-like
 * naming, but nothing here is shogi specific): board state, move generation,
 * a recursive negamax search, evaluation, a transposition table and a
 * parallel worker pool with a message-style dispatch.
 *
 * The point of the fixture is to give the static analyzer real symbols to
 * trace — the regression ring should light up on SURVEY / CLASSIFY / TRACE
 * / PREDICT / PARALLEL / VERIFY, and the caller chains for board_state /
 * legal_moves / current_score should resolve.
 *
 * Build:  cc -O1 -o engine_fixture engine_fixture.c
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define BOARD_SIZE     81
#define MAX_MOVES      128
#define MAX_DEPTH      64
#define TABLE_SIZE     4096
#define WORKERS        4

typedef struct {
    int  cells[BOARD_SIZE];
    int  side;
    int  ply;
} board_state;

typedef struct {
    int from;
    int to;
    int piece;
} move_entry;

typedef struct {
    int    score;
    int    depth;
    uint64_t hash;
} search_node;

typedef struct {
    uint64_t hash;
    int      score;
    int      depth;
    int      used;
} transposition_table;

/* ── Global engine state ─────────────────────────────────────────────────── */

board_state        current_board;
board_state        initial_board;
int                current_score;
int                best_score;
int                max_depth = 6;
int                beam_width = 8;
int                node_count;
int                legal_move_count;
int                worker_thread_count = WORKERS;
move_entry         legal_moves[MAX_MOVES];
search_node        search_tree_nodes[256];
transposition_table tt_table[TABLE_SIZE];

/* ── Board helpers ───────────────────────────────────────────────────────── */

void reset_board(board_state *board) {
    memset(board, 0, sizeof(*board));
    board->side = 0;
}

void setup_initial_position(void) {
    reset_board(&initial_board);
    current_board = initial_board;
}

int cell_value(int cell) {
    return current_board.cells[cell];
}

/* ── Move generation ─────────────────────────────────────────────────────── */

int generate_moves(move_entry *moves, int limit) {
    int count = 0;
    for (int i = 0; i < BOARD_SIZE && count < limit; i++) {
        if (current_board.cells[i] == 0) continue;
        moves[count].from = i;
        moves[count].to = (i + 1) % BOARD_SIZE;
        moves[count].piece = current_board.cells[i];
        count++;
    }
    legal_move_count = count;
    return count;
}

void apply_move(const move_entry *mv) {
    int piece = current_board.cells[mv->from];
    current_board.cells[mv->from] = 0;
    current_board.cells[mv->to] = piece;
    current_board.ply++;
}

/* ── Evaluation ──────────────────────────────────────────────────────────── */

int evaluate_position(void) {
    int score = 0;
    for (int i = 0; i < BOARD_SIZE; i++) {
        score += current_board.cells[i] * (i % 7 + 1);
    }
    current_score = score;
    return score;
}

int score_table_lookup(int index) {
    if (index < 0 || index >= TABLE_SIZE) return 0;
    return tt_table[index].score;
}

/* ── Transposition table ─────────────────────────────────────────────────── */

uint64_t compute_position_hash(const board_state *board) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < BOARD_SIZE; i++) {
        h ^= (uint64_t)board->cells[i];
        h *= 1099511628211ULL;
    }
    return h;
}

int transposition_probe(uint64_t hash, int depth, int *out_score) {
    unsigned int slot = (unsigned int)(hash % TABLE_SIZE);
    transposition_table *slotp = &tt_table[slot];
    if (slotp->used && slotp->hash == hash && slotp->depth >= depth) {
        *out_score = slotp->score;
        return 1;
    }
    slotp->used = 1;
    slotp->hash = hash;
    slotp->depth = depth;
    return 0;
}

/* ── Search ──────────────────────────────────────────────────────────────── */

int negamax(int depth, int alpha, int beta) {
    node_count++;

    if (depth <= 0) {
        return evaluate_position();
    }

    move_entry moves[MAX_MOVES];
    int count = generate_moves(moves, MAX_MOVES);
    int best = -1000000;

    for (int i = 0; i < count; i++) {
        apply_move(&moves[i]);
        int score = -negamax(depth - 1, -beta, -alpha);
        if (score > best) best = score;
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;   /* beta cut-off */
    }

    best_score = best;
    return best;
}

int beam_search(int depth) {
    int width = beam_width;
    int scores[MAX_MOVES];
    move_entry moves[MAX_MOVES];
    int count = generate_moves(moves, MAX_MOVES);

    for (int i = 0; i < count && i < width; i++) {
        apply_move(&moves[i]);
        scores[i] = evaluate_position();
    }
    return count ? scores[0] : 0;
}

/* ── Parallel workers ────────────────────────────────────────────────────── */

int worker_evaluate(int worker_id) {
    int total = 0;
    for (int i = worker_id; i < BOARD_SIZE; i += worker_thread_count) {
        total += cell_value(i);
    }
    return total;
}

int dispatch_parallel_evaluation(void) {
    int total = 0;
    for (int w = 0; w < worker_thread_count; w++) {
        total += worker_evaluate(w);
    }
    return total;
}

int join_parallel_evaluation(int total) {
    return total;
}

/* ── Turn loop (regression-friendly) ─────────────────────────────────────── */

int iterate_turn(int ply) {
    if (ply >= max_depth) {
        return evaluate_position();
    }
    move_entry moves[MAX_MOVES];
    int count = generate_moves(moves, MAX_MOVES);
    if (count == 0) return 0;

    apply_move(&moves[0]);
    int score = iterate_turn(ply + 1);
    current_score = score;
    return score;
}

int main(void) {
    setup_initial_position();
    evaluate_position();
    negamax(3, -1000000, 1000000);
    beam_search(2);
    dispatch_parallel_evaluation();
    iterate_turn(0);

    printf("score=%d nodes=%d moves=%d hash=%llu\n",
           current_score, node_count, legal_move_count,
           (unsigned long long)compute_position_hash(&current_board));
    return 0;
}
