// nx_pile.hpp — 状態を derive して「積む」ための最小モデル
//
// 目的:
//   解析結果を後から突き合わせるのではなく、状態を derive した時点で
//   そのスナップショットを積み、**その先だけ**を解析する。
//
// なぜ pile か:
//   derive は「ある時点の状態から新しい状態を作る」操作。作った瞬間の
//   状態を保存しておかないと、後から「その時点で何だったか」を復元できない。
//   積み上がった各段は不変 (immutable) で、後続の解析はその段だけを見る。
//
// 不変条件:
//   - 各 Frame は derive 時点のスナップショット。後から書き換えない。
//   - Frame は parent を 1 つ持つ (木)。同じ親から複数 derive してよい。
//   - 「一致するか」の判定は Frame 同士の比較で行い、
//     解析側は差分 (delta) だけを辿る。

#ifndef NX_PILE_HPP
#define NX_PILE_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <set>
#include <vector>

namespace nx {

// 1 つの観測点での状態。値は「キー → 値」の平坦な写像。
// キー例: "x0", "sp", "0x1000(sp)", "flag:ZF"
struct State {
    std::map<std::string, uint64_t> vals;

    bool operator==(const State &o) const { return vals == o.vals; }
    bool operator!=(const State &o) const { return !(*this == o); }

    // 差分。a にあって b に無い/違うキーだけを返す。
    static std::vector<std::string> delta(const State &a, const State &b);
};

// 積まれた 1 段。
struct Frame {
    uint64_t id = 0;
    uint64_t at = 0;                 // 観測したアドレス (命令)
    std::string why;                 // 何で derive したか ("after-branch" 等)
    State state;                     // derive 時点のスナップショット (不変)
    uint64_t parent = 0;             // 親 frame (0 = 根)
    std::vector<uint64_t> children;
    uint64_t hash = 0;               // state の指紋 (比較を安くする)
};

// 命令のオペランドから書き込み先を取り出す。戻り値は書き込み先の数。
// 例: ("str", "x3, [x0, #16]") -> out[0] = "[x0, #16]"
//     ("mov", "x0, x1")        -> out[0] = "x0"
//     ("cmp", "x0, x1")        -> 0 (書かない)
int writes_of(const char *mnemonic, const char *operands,
              char out[][16], int cap);

class Pile {
public:
    // 根を積む。id は 1 から。
    uint64_t push_root(uint64_t at, State s, const std::string &why);

    // parent から derive して積む。中身が同じなら積まない (重複排除)。
    // 戻り値は積んだ frame の id。既存と同一なら既存の id。
    uint64_t derive(uint64_t parent, uint64_t at, State s, const std::string &why);

    const Frame *get(uint64_t id) const;
    const std::vector<Frame> &frames() const { return frames_; }

    // 2 つの frame が同じ状態か (積んだ時点の指紋で比較)。
    bool same_state(uint64_t a, uint64_t b) const;

    // ある frame から先で、状態が実際に変わった frame だけを返す。
    // 「変わっていない区間」は解析対象から外せる。
    std::vector<uint64_t> changed_descendants(uint64_t from) const;

    size_t size() const { return frames_.size(); }

    // ---- 完全一致検索 (derive した先だけを対象にする) ----
    //
    // 方針:
    //   1. 状態の指紋 (hash) で候補を絞る。これが「アタリ」付け。
    //   2. 指紋が一致したものだけ state を厳密比較する。
    //   走査範囲は from の子孫に限定するので、積んだ数に対して線形。
    //
    // why: 空でなければその理由で derive された frame だけに絞る。
    // 戻り値: 一致した frame の id (積んだ順)。
    std::vector<uint64_t> find_exact(uint64_t from, const State &want,
                                     const std::string &why = "") const;

    // 指紋だけで候補を出す (厳密比較はしない)。当たり付けの確認用。
    std::vector<uint64_t> find_by_hash(uint64_t from, uint64_t hash) const;

    // from の子孫集合 (from 自身は含めない)。参照範囲の判定に使う。
    std::set<uint64_t> descendants_of(uint64_t from) const;

private:
    std::vector<Frame> frames_;
    uint64_t next_id_ = 1;

    // 参照を絞るための索引。木をまたいで引けるが、
    // 「どこを舐めるか」は呼び出し側の範囲指定で決まる。
    //   hash -> frame id 群   (完全一致のあたり付け。O(1) で引く)
    std::map<uint64_t, std::vector<uint64_t>> by_hash_;
    //   frame id -> 積んだ順序 (範囲を「以降」で切るため)
    std::map<uint64_t, uint64_t> seq_;

    uint64_t hash_state(const State &s) const;
};

} // namespace nx

#endif
