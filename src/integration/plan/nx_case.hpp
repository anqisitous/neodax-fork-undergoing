// nx_case.hpp — 「案件」を差し込むための汎用インタフェース
//
// なぜ要るか:
//   nx_plan の Model は「観測 → 問題 → 計画 → 検証」の骨格を持つが、
//   何を問題とみなすかは案件ごとに違う (タブ状態 / AE のライセンス /
//   署名 / 更新 …)。骨格に案件の知識を直書きすると、新しい案件のたびに
//   Model を書き換えることになり、個別対応ができなくなる。
//
//   そこで「事実を集める」「事実から問題を導く」「問題から修復手を組む」
//   の 3 点だけを Case として外に出し、Model は順序と検証だけを受け持つ。
//
// 個別対応の作法:
//   1 案件 = 1 つの Case 実装。他の案件に影響しない。
//   同じ案件でも、対象 (アプリ / パス) ごとに観測結果が変わる。
//   Case は「自分が扱えるか (applicable)」を必ず答える。

#ifndef NX_CASE_HPP
#define NX_CASE_HPP

#include <memory>
#include <string>
#include <vector>

#include "nx_plan.hpp"

namespace nx {

// 案件が扱う対象。パスやアプリ名など、観測の入口。
struct CaseTarget {
    std::string kind;     // "state" / "app" / "binary" / "dir"
    std::string path;     // 実パス
    std::string note;     // 補足 (アプリ名など)
};

class Case {
public:
    virtual ~Case() = default;

    // この案件の名前 (表示・選択に使う)。例 "tabs", "ae-license"
    virtual const char *name() const = 0;

    // この対象を扱えるか。扱えないなら false を返し、Model は次を試す。
    // 副作用を持ってはいけない (観測だけで判断する)。
    virtual bool applicable(const CaseTarget &t) const = 0;

    // 事実を集める。collect は「観測」だけを行い、判断は analyze に残す。
    virtual void collect(const CaseTarget &t, std::vector<Signal> &out) const = 0;

    // 事実から問題を導く。根拠の signal id を必ず添える。
    virtual void analyze(const std::vector<Signal> &signals,
                         std::vector<Finding> &out) const = 0;

    // 問題から修復手を組む。順序制約 (requires) と代替をここで宣言する。
    virtual void plan(const std::vector<Finding> &findings,
                      std::vector<Step> &out) const = 0;
};

// 案件の登録簿。CLI はここから対象に合う案件を選ぶ。
class CaseRegistry {
public:
    static CaseRegistry &instance();

    void add(std::shared_ptr<Case> c);

    // 対象を扱える案件をすべて返す (逐次に試すため、登録順を保つ)。
    std::vector<std::shared_ptr<Case>> match(const CaseTarget &t) const;

    // 名前で取り出す。
    std::shared_ptr<Case> by_name(const std::string &n) const;

    const std::vector<std::shared_ptr<Case>> &all() const { return cases_; }

private:
    std::vector<std::shared_ptr<Case>> cases_;
};

// 組み込み案件を登録する (起動時に 1 回)。
void register_builtin_cases();

} // namespace nx

#endif
