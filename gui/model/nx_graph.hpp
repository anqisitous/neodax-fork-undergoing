// nx_graph.hpp — viewer のグラフモデル (多相設計 / AppKit 非依存)
//
// 設計方針:
//   以前は GNode 構造体 1 個と switch の寄せ集めで、
//     - ノード種別 (実関数 / 仮想ノード)
//     - 所属 (role / ring)
//     - 配置方法 (円周 / 単体)
//   がすべて「フラグと分岐」で表現されていた。ここを多相に直す。
//
//   ・Node        … 描画対象となる要素。実体は FunctionNode / VirtualGroupNode
//   ・Group       … ノードのまとまり。RoleGroup / RingGroup / VirtualRingGroup
//   ・Layout      … 配置戦略。RadialLayout / SingleLayout
//   ・Graph       … 上記を組み立て、階層を構築し、配置する門番
//
//   このヘッダは AppKit を一切知らない。座標は独自の Rect/Point で扱い、
//   view 側が NSRect へ変換して描く。これで多相なロジックを .mm の外に置ける。

#ifndef NX_GRAPH_HPP
#define NX_GRAPH_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nx {

struct Point { float x = 0, y = 0; };
struct Rect  { float x = 0, y = 0, w = 0, h = 0; };

// ------------------------------------------------------------------ Node

class Node {
public:
    virtual ~Node() = default;
    virtual const char *kind() const = 0;      // "function" / "virtual"
    virtual bool is_virtual() const { return false; }
    virtual std::string label() const = 0;

    uint64_t    addr = 0;
    int         role = -1;
    int         ring = -1;
    Point       pos;
    std::string name;
};

// 実関数。viewer 側の関数配列と同じ並びを保つため、生成順が意味を持つ。
class FunctionNode : public Node {
public:
    const char *kind() const override { return "function"; }
    std::string label() const override { return name; }
    bool recursive = false;
    int  nargs = 0;
};

// 仮想ノード。状態依存の環をまとめた見出しとして 2 階層目に出す。
class VirtualGroupNode : public Node {
public:
    const char *kind() const override { return "virtual"; }
    bool is_virtual() const override { return true; }
    std::vector<int> grouped_rings;
    std::string label() const override {
        return "state-dependent x" + std::to_string(grouped_rings.size());
    }
};

// ------------------------------------------------------------------ Layout

class Layout {
public:
    virtual ~Layout() = default;
    virtual const char *name() const = 0;
    // members を center 中心に配置し、外接半径を返す。
    virtual float arrange(std::vector<Node *> &members, Point center) const = 0;
};

// 環: メンバーを円周上に等間隔で置く
class RadialLayout : public Layout {
public:
    const char *name() const override { return "radial"; }
    float arrange(std::vector<Node *> &members, Point center) const override;
};

// 単体: 中心に置く
class SingleLayout : public Layout {
public:
    const char *name() const override { return "single"; }
    float arrange(std::vector<Node *> &members, Point center) const override;
};

// 戦略の選択も多相で行う (呼び出し側に分岐を書かない)
const Layout *layout_for(size_t member_count);

// ------------------------------------------------------------------ Group

class Group {
public:
    virtual ~Group() = default;
    virtual const char *kind() const = 0;
    virtual bool is_virtual() const { return false; }
    virtual std::string label() const = 0;

    std::vector<Node *> members;
    Rect box;
    int  role = -1;
};

class RingGroup : public Group {
public:
    const char *kind() const override { return "ring"; }
    int ring = -1;
    std::string label() const override {
        if (ring < 0) return "seq";
        return "ring " + std::to_string(ring) + (members.size() > 1 ? " *" : "");
    }
};

// 状態依存の環を束ねた仮想の環。仮想ノードを 1 つだけ抱える。
class VirtualRingGroup : public RingGroup {
public:
    VirtualRingGroup() { ring = -1000; }
    const char *kind() const override { return "virtual-ring"; }
    bool is_virtual() const override { return true; }
    std::string label() const override {
        std::string vn = "state-dependent";
        for (auto *m : members)
            if (m->is_virtual()) { vn = m->label(); break; }
        return vn;
    }
};

class RoleGroup : public Group {
public:
    const char *kind() const override { return "role"; }
    std::vector<RingGroup *> rings;
    std::string label() const override { return "role " + std::to_string(role); }
};

// ------------------------------------------------------------------ Graph

class Graph {
public:
    Graph();
    ~Graph();

    // 実関数を追加 (呼び出し順がそのまま関数インデックスになる)
    FunctionNode *add_function(uint64_t addr, const std::string &name, int role, int ring);

    // role -> ring -> node を組み立て、状態依存の環が同 role に複数あれば仮想環を作る
    void build_hierarchy();

    // 配置。wrap は role を折り返す幅。
    void layout(float wrap = 34.0f);

    const std::vector<FunctionNode *> &functions() const { return funcs_; }
    const std::vector<RingGroup *>    &rings()     const { return ring_ptrs_; }
    const std::vector<RoleGroup *>    &roles()     const { return role_ptrs_; }
    // 仮想ノードだけ (描画用に実関数の後ろへ足す)
    std::vector<Node *> virtual_nodes() const;

    static bool is_state_dependent_name(const std::string &s);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    std::vector<FunctionNode *> funcs_;
    std::vector<RingGroup *>    ring_ptrs_;   // build_hierarchy 後に確定
    std::vector<RoleGroup *>    role_ptrs_;
};

} // namespace nx

#endif // NX_GRAPH_HPP
