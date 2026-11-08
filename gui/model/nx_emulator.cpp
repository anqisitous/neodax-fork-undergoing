#include "nx_emulator.hpp"

#include <algorithm>
#include <cstdio>

struct DVNode {
    std::string role, sub, title, desc;
    double x = 0, y = 0, w = 0, h = 0;
    std::vector<DVNode> kids;
};

namespace nxemu {
namespace {

void walk(const DVNode &n, int depth, std::vector<Widget> &out) {
    Widget w;
    w.node = &n;
    w.x = n.x; w.y = n.y; w.w = n.w; w.h = n.h;
    w.depth = depth;
    w.role = n.role;
    out.push_back(w);
    for (const auto &k : n.kids) walk(k, depth + 1, out);
}

} // namespace

const char *mode_name(Mode m) {
    return m == Mode::Operate ? "operate" : "inspect";
}

std::vector<Widget> widgets(const DVNode &root) {
    std::vector<Widget> out;
    walk(root, 0, out);
    return out;
}

std::vector<Widget> select_in_rect(const std::vector<Widget> &ws,
                                   double x0, double y0, double x1, double y1) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    std::vector<Widget> out;
    for (const auto &w : ws) {
        // 矩形と重なるものを選ぶ (完全内包は要求しない)
        double wx1 = w.x + w.w, wy1 = w.y + w.h;
        if (w.x < x1 && wx1 > x0 && w.y < y1 && wy1 > y0) out.push_back(w);
    }
    return out;
}

const Widget *pick(const std::vector<Widget> &ws, double px, double py) {
    const Widget *best = nullptr;
    for (const auto &w : ws) {
        if (!w.hit(px, py)) continue;
        // 最も内側 (depth が大きい) を優先。同じなら面積が小さい方。
        if (!best || w.depth > best->depth ||
            (w.depth == best->depth && w.w * w.h < best->w * best->h))
            best = &w;
    }
    return best;
}

std::string describe(const Widget &w) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.0f,%.0f %.0fx%.0f", w.x, w.y, w.w, w.h);
    std::string s = w.role + " @" + buf;
    if (w.node) {
        if (!w.node->sub.empty())   s += " sub=" + w.node->sub;
        if (!w.node->title.empty()) s += " title=" + w.node->title;
        if (!w.node->desc.empty())  s += " desc=" + w.node->desc;
        s += " kids=" + std::to_string(w.node->kids.size());
    }
    return s;
}

} // namespace nxemu
