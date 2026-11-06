#ifndef NX_PIPELINE_H
#define NX_PIPELINE_H

/*
 * nx_pipeline — 「デコード(x86)」と「C++エンコード(生成)」の実行戦略を選ぶ。
 *
 *   - sequential   : 全命令をデコードしてから、まとめて C++ 生成
 *   - simultaneous : デコードと C++ 生成を並行 (producer/consumer, pthread)
 *   - auto         : 両方を実測し、合計時間の短い方を採用
 *
 * 同じ入力・同じ出力を、戦略だけ変えて時間計測する。
 */

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NX_STRAT_AUTO = 0,
    NX_STRAT_SEQUENTIAL,
    NX_STRAT_SIMULTANEOUS
} nx_strategy_t;

typedef struct {
    double seq_ms;      /* sequential の合計時間 (auto 時のみ計測) */
    double sim_ms;      /* simultaneous の合計時間 (auto 時のみ計測) */
    int    chosen;      /* 採用した nx_strategy_t */
    int    n_insns;     /* デコードした命令数 */
} nx_pipeline_result_t;

/* PE をデコードし C++ を生成。strat に従い戦略を選ぶ。
   cpp_out が非 NULL なら、選んだ戦略で C++ を書き出す。 */
int nx_pipeline_run(const char *pe_path, nx_strategy_t strat, int limit,
                    FILE *cpp_out, nx_pipeline_result_t *res, FILE *log);

#ifdef __cplusplus
}
#endif

#endif /* NX_PIPELINE_H */
