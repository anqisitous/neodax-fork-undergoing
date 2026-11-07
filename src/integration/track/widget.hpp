// widget.hpp — 保存されたウィジェット構成を静的にシミュレートする
//
// 目的:
//   アプリを起動せずに「そのウィジェット構成が動くか」を判定する。
//   保存状態は スプリッタの入れ子で、葉は タブ（パネル/拡張の識別子）を持つ。
//   識別子が解決できないとその葉は作れない。ここを再現する。
//
// クラス:
//   Node        構成の抽象。検証を持つ
//   Splitter    分割。向き・位置・可視・子 2 つ
//   TabSet      葉。タブ識別子の集合
//   Registry    解決できる識別子の集合 (拡張の一覧など)
//   Simulator   木をたどって作れるかどうかを判定する
//
// 形式の解釈はこのファイルに閉じる。呼び出し側は木と結果だけを見る。

#ifndef NX_WIDGET_HPP
#define NX_WIDGET_HPP

#include <cstdio>
#include <string>
#include <vector>

namespace nx {

// 作れる / 作れない の理由つき結果
struct Issue {
    std::string what;     // どの要素か
    std::string why;      // なぜ作れないか
};

class Node {
public:
    virtual ~Node() = default;
    virtual const char *kind() const = 0;
    virtual void validate(std::vector<Issue> &out) const = 0;
};

// 分割。子を 2 つ持ち、向きと位置で配置される。
class Splitter : public Node {
public:
    const char *kind() const override { return "splitter"; }
    void validate(std::vector<Issue> &out) const override;

    bool   vertical = false;    // Orient
    double place = 0.5;         // Place
    bool   sub1_vis = true, sub2_vis = true;
    Node  *sub1 = nullptr;      // 所有しない
    Node  *sub2 = nullptr;
};

// 葉。タブ識別子が 1 つ以上あり、すべて解決できる必要がある。
class TabSet : public Node {
public:
    const char *kind() const override { return "tabs"; }
    void validate(std::vector<Issue> &out) const override;

    std::vector<std::string> tabs;   // TabIDs
    std::string anchor;              // 記録用の位置 (TabPanel-N など)
};

// 解決できる識別子の集合
class Registry {
public:
    void add(const std::string &id) { ids_.push_back(id); }
    bool knows(const std::string &id) const;
    size_t size() const { return ids_.size(); }

private:
    std::vector<std::string> ids_;
};

// 構成全体
class Workspace {
public:
    ~Workspace();
    Node *root = nullptr;                     // 所有
    std::vector<TabSet *> all_tabs;           // 便宜のため
    void clear();
};

// 保存状態のテキストから構成を組み立てる。解釈はここに閉じる。
bool parse_workspace(const std::string &text, Workspace &out, std::string *err);

// 木をたどって判定する
class Simulator {
public:
    explicit Simulator(const Registry &r) : reg_(r) {}
    // 作れるなら true。作れない場合は理由を out に積む。
    bool run(const Workspace &ws, std::vector<Issue> &out) const;

private:
    const Registry &reg_;
};

} // namespace nx

#endif // NX_WIDGET_HPP
