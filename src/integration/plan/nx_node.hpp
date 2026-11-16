#ifndef NX_NODE_HPP
#define NX_NODE_HPP

/*
 * nx_node — neodax のコマンドを「ノードから呼べる」ようにする橋。
 *
 * なぜ要るか:
 *   判断ノード (clean / resolve / splice) は、状態を見るのに
 *   すでに完成したコマンド (tabs / keys / advise) と同じことを
 *   もう一度書いていた。実装が 2 つあると、片方だけ直して食い違う。
 *
 *   neodax 自身がコマンドを持っているのだから、ノードはそれを
 *   呼べばよい。コマンドの中身は 1 つだけにする。
 *
 * 呼び方:
 *     nx_node::Result r = nx_node::run("tabs");
 *     nx_node::Result r = nx_node::run({"keys", "--state", path});
 *     if (r.ok()) printf("%s", r.out.c_str());
 *
 * 仕組み:
 *   nx_cli_dispatch (CLI と GUI が共有している唯一の入口) を呼び、
 *   stdout を一時ファイルへ逃がして回収する。GUI のボタンが使っている
 *   経路と同じなので、挙動がずれない。
 *
 * 安定性:
 *   コマンドが増えても、この橋は変えない。名前と引数を渡すだけ。
 *   「どのコマンドがあるか」は nx_cli_registry が持っている。
 */

#include <string>
#include <vector>

namespace nx {

struct NodeResult {
    int         rc;     /* コマンドの終了コード */
    std::string out;    /* 標準出力 (そのまま) */
    std::string err;    /* 失敗理由 (rc != 0 のとき) */

    bool ok() const { return rc == 0; }
};

namespace node {

/* argv を直接渡す。argv[0] はコマンド名 ("tabs" など)。 */
NodeResult run(const std::vector<std::string> &argv);

/* 空白区切りの 1 行を渡す。引用はしない (パスに空白があるなら vector 版)。 */
NodeResult run(const char *line);

/* --state を自動で足して呼ぶ (省略可)。 */
NodeResult run_with_state(const std::vector<std::string> &argv, const char *state_path);

/* 出力を 1 行ずつに分ける (TSV の読み取り用)。 */
std::vector<std::string> lines(const std::string &s);

/* "KEY\tVALUE" から KEY を探して VALUE を返す。無ければ def。 */
std::string field(const std::string &out, const char *key, const char *def = "");

} // namespace node
} // namespace nx

#endif /* NX_NODE_HPP */
