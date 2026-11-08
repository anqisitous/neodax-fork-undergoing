// nx_plan.hpp — 自発修復のための「分析 → 計画 → 検証」器
//
// 目的:
//   特定のタブ (chat など) に限らず、観測した事実から
//   「何が問題か」「いつ・どの順で直すか」「その結果にどれだけ自信があるか」
//   を論理的に組み立てる。実行そのものは別 (CLI が approve する)。
//
// なぜ順序と並列が要るか:
//   最終出力は「関数を呼ぶ順番」に依存する。同じ集合でも
//     ・逐次で呼ばれる (A→B→C)
//     ・並列で呼ばれ、片方が失敗したら他方で代替する (A ∥ B, fallback)
//   では結果が変わる。だから計画は順序を持ち、並列群は
//   「両方取れたか / 片方だけか」を記録して、その下での結論だけを出す。
//
// 設計:
//   Signal   観測した事実 (durable / transient, 出所つき)
//   Finding  信号から導いた「問題」の候補 (根拠の信号を列挙)
//   Step     修復の 1 手 (前提・効果・並列可否・順序制約)
//   Plan     トポロジカル順序 + 並列群 + 代替経路
//   Verdict  計画を実行したときの結論と自信 (regression で裏取り)

#ifndef NX_PLAN_HPP
#define NX_PLAN_HPP

#include <cstdio>
#include <string>
#include <vector>

namespace nx {

// ---- 事実 ----
enum class Persistence { Durable, Transient };

struct Signal {
    std::string id;         // "tabs.orphan[0]"
    std::string source;     // 観測した出所 (store / binary / probe)
    std::string value;      // 観測値
    Persistence persistence = Persistence::Durable;
    bool observed = false;
    bool informs_verdict = true;
};

// ---- 問題 ----
struct Finding {
    std::string id;             // "orphan-surface"
    std::string summary;        // 何が問題か
    std::vector<std::string> evidence;   // 根拠の signal id
    double severity = 0.0;      // 0..1
    bool fixable = false;       // 修復手があるか
};

// ---- 修復手 ----
// 並列可否と「代替」を持つ。片方が取れなければ他方で取る、を表現する。
struct Step {
    std::string id;             // "sync-live-store"
    std::string action;         // 何をするか
    std::vector<std::string> requires;   // 前提となる step id
    std::vector<std::string> alternatives; // 代替 step (並列で試す)
    bool can_parallel = false;  // requires が満たされていれば並列可
    bool mutates = false;       // 状態を書き換えるか
    bool approved = false;      // 実行に承認が要るか
    int  cost = 1;              // 相対コスト (順序の決定に使う)
};

// ---- 計画 ----
enum class OrderKind { Sequential, Parallel, Fallback };

struct Group {
    std::vector<std::string> steps;  // 同じ段で走る step (1 つなら逐次)
    OrderKind kind = OrderKind::Sequential;
    // 並列群の取りこぼし。両方取れなければ fallback 側だけが残る。
    int got = 0;
    int attempted = 0;
};

struct Plan {
    std::vector<Step> steps;
    std::vector<Group> order;        // 実行順 (段)
    std::string error;
    bool ok() const { return error.empty(); }
};

// ---- 結論 ----
enum class Verdict { Healthy, NeedsFix, Unknown };

struct Confidence {
    // 順序を変えたときに結論が揺れないか (regression)。
    int  orderings_tried = 0;
    int  orderings_agreeing = 0;
    double stability = 0.0;          // agreeing / tried
    double evidence_coverage = 0.0;  // finding の根拠が観測済みか
    double score = 0.0;              // 総合 (0..1)
    std::string basis;               // 何を根拠にしたか
};

struct Advice {
    std::string summary;
    std::vector<std::string> steps;
};

class Model {
public:
    explicit Model(std::string subject);
    void collect();

    // 信号を外から足す (CLI が store / バイナリから集めて渡す)。
    void add_signal(Signal s);

    void analyze();          // signals -> findings
    void plan();             // findings -> plan (order + parallel/fallback)
    void verify();           // plan -> confidence (regression over orders)

    Verdict evaluate() const;
    const std::vector<Signal> &signals() const { return signals_; }
    const std::vector<Finding> &findings() const { return findings_; }
    const Plan &make_plan() const { return plan_; }
    const Confidence &confidence() const { return conf_; }
    const Advice &advice() const { return advice_; }
    const std::string &subject() const { return subject_; }

private:
    std::string subject_;
    std::vector<Signal> signals_;
    std::vector<Finding> findings_;
    Plan plan_;
    Confidence conf_;
    Advice advice_;

    void build_advice();
    void topo_sort();
    int simulate(const std::vector<std::string> &order, bool parallel_ok) const;
};

} // namespace nx

#endif
