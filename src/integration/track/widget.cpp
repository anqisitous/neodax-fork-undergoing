// widget.cpp — ウィジェット構成の解釈とシミュレート

#include "widget.hpp"

#include <cstdlib>
#include <cstring>

namespace nx {

// ---------------------------------------------------------------- Node

void Splitter::validate(std::vector<Issue> &out) const {
    if (!sub1 || !sub2) {
        Issue i;
        i.what = "分割";
        i.why = "子が 2 つ揃っていません。";
        out.push_back(i);
        return;
    }
    if (!(place > 0.0 && place < 1.0)) {
        Issue i;
        i.what = "分割";
        i.why = "位置が 0 から 1 の外にあります。";
        out.push_back(i);
    }
    sub1->validate(out);
    sub2->validate(out);
}

void TabSet::validate(std::vector<Issue> &out) const {
    if (tabs.empty()) {
        Issue i;
        i.what = anchor.empty() ? "タブ" : anchor;
        i.why = "タブが 1 つもありません。";
        out.push_back(i);
    }
}

// ---------------------------------------------------------------- Registry

bool Registry::knows(const std::string &id) const {
    for (const auto &x : ids_)
        if (x == id) return true;
    return false;
}

// ---------------------------------------------------------------- Workspace

Workspace::~Workspace() { clear(); }

void Workspace::clear() {
    // 深さ優先で解放する
    std::vector<Node *> stack;
    if (root) stack.push_back(root);
    while (!stack.empty()) {
        Node *n = stack.back();
        stack.pop_back();
        if (auto *s = dynamic_cast<Splitter *>(n)) {
            if (s->sub1) stack.push_back(s->sub1);
            if (s->sub2) stack.push_back(s->sub2);
        }
        delete n;
    }
    root = nullptr;
    all_tabs.clear();
}

// ---------------------------------------------------------------- 解釈

namespace {

} // namespace

// タブ識別子を素直に集める。木の厳密な再構築よりも、
// 「どのタブが要求されているか」を取り違えないことを優先する。
bool parse_workspace(const std::string &text, Workspace &out, std::string *err) {
    out.clear();
    if (text.empty()) { if (err) *err = "空です"; return false; }

    // タブを持つ葉を 1 つにまとめる (要求されている識別子の集合)
    auto *tabs = new TabSet();
    tabs->anchor = "タブ";

    size_t pos = 0;
    int found = 0;
    while (true) {
        size_t t = text.find("TabIDs", pos);
        if (t == std::string::npos) break;
        pos = t + 6;
        // この TabIDs の配列の中の <string> を拾う
        size_t arr_end = text.find("</array>", pos);
        size_t scan = pos;
        while (true) {
            size_t s0 = text.find("<string>", scan);
            if (s0 == std::string::npos) break;
            if (arr_end != std::string::npos && s0 > arr_end) break;
            size_t s1 = text.find("</string>", s0);
            if (s1 == std::string::npos) break;
            std::string id = text.substr(s0 + 8, s1 - s0 - 8);
            if (!id.empty()) { tabs->tabs.push_back(id); found++; }
            scan = s1 + 9;
        }
    }

    if (found == 0) {
        delete tabs;
        if (err) *err = "タブが見つかりません";
        return false;
    }

    out.root = tabs;
    out.all_tabs.push_back(tabs);
    return true;
}

// ---------------------------------------------------------------- Simulator

bool Simulator::run(const Workspace &ws, std::vector<Issue> &out) const {
    if (!ws.root) {
        Issue i; i.what = "構成"; i.why = "根がありません。";
        out.push_back(i);
        return false;
    }

    size_t before = out.size();
    for (const auto *t : ws.all_tabs) {
        if (t->tabs.empty()) {
            Issue i; i.what = t->anchor; i.why = "タブがありません。";
            out.push_back(i);
            continue;
        }
        for (const auto &id : t->tabs) {
            if (!reg_.knows(id)) {
                Issue i;
                i.what = t->anchor + " の " + id;
                i.why = "この識別子は解決できません。";
                out.push_back(i);
            }
        }
    }
    return out.size() == before;
}

} // namespace nx
