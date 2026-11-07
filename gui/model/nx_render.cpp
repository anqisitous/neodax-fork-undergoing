// nx_render.cpp — 描画の実装 (AppKit 非依存)
//
// Graph 側が多相なので、ここには「種別ごとの分岐」をほとんど書かない。
// 環が仮想かどうかも is_virtual() で判定する。

#include "nx_render.hpp"

#include <algorithm>
#include <cmath>
#include <ostream>
#include <sstream>
#include <string>

namespace nx {

namespace {

std::string esc(const std::string &s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&#39;"; break;
            default: o += c;
        }
    }
    return o;
}

// role ごとの色 (hue を回す)
std::string role_fill(int role, double alpha) {
    double hue = std::fmod((double)(role >= 0 ? role : 11) * 47.0, 360.0);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "hsla(%.0f,55%%,45%%,%.2f)", hue, alpha);
    return buf;
}

std::string role_stroke(int role) {
    double hue = std::fmod((double)(role >= 0 ? role : 11) * 47.0, 360.0);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "hsl(%.0f,60%%,60%%)", hue);
    return buf;
}

} // namespace

void SvgRenderer::render(const Graph &g, std::ostream &out) const {
    // ワールド範囲を求める
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    auto grow = [&](float x, float y) {
        minx = std::min(minx, x); maxx = std::max(maxx, x);
        miny = std::min(miny, y); maxy = std::max(maxy, y);
    };
    for (const auto *f : g.functions()) grow(f->pos.x, f->pos.y);
    for (const auto *r : g.rings()) {
        grow(r->box.x, r->box.y);
        grow(r->box.x + r->box.w, r->box.y + r->box.h);
    }
    for (const auto *rg : g.roles()) {
        grow(rg->box.x, rg->box.y);
        grow(rg->box.x + rg->box.w, rg->box.y + rg->box.h);
    }
    if (minx > maxx) { out << "<svg xmlns=\"http://www.w3.org/2000/svg\"/>\n"; return; }

    const float w = (maxx - minx) * scale + margin * 2;
    const float h = (maxy - miny) * scale + margin * 2;

    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << (int)w
        << "\" height=\"" << (int)h << "\" viewBox=\"0 0 " << (int)w << " " << (int)h
        << "\" font-family=\"Menlo,monospace\">\n";
    out << "<rect width=\"100%\" height=\"100%\" fill=\"#0d1117\"/>\n";

    // ワールド → SVG 座標 (y 軸は下向きに反転)
    auto X = [&](float wx) { return margin + (wx - minx) * scale; };
    auto Y = [&](float wy) { return margin + (maxy - wy) * scale; };

    // ---- 1 階層目: role ----
    for (const auto *rg : g.roles()) {
        out << "<rect x=\"" << X(rg->box.x) << "\" y=\"" << Y(rg->box.y + rg->box.h)
            << "\" width=\"" << rg->box.w * scale << "\" height=\"" << rg->box.h * scale
            << "\" rx=\"10\" fill=\"" << role_fill(rg->role, 0.06)
            << "\" stroke=\"" << role_stroke(rg->role)
            << "\" stroke-opacity=\"0.35\" stroke-width=\"1.5\"/>\n";
    }

    // ---- 2 階層目: ring (仮想環は破線で区別) ----
    for (const auto *r : g.rings()) {
        const bool virt = r->is_virtual();
        out << "<circle cx=\"" << X(r->box.x + r->box.w / 2)
            << "\" cy=\"" << Y(r->box.y + r->box.h / 2)
            << "\" r=\"" << (std::max(r->box.w, r->box.h) / 2) * scale << "\""
            << " fill=\"none\" stroke=\"" << (virt ? "#e3b341" : role_stroke(r->role)) << "\""
            << " stroke-opacity=\"" << (virt ? 0.95 : 0.55) << "\""
            << " stroke-width=\"" << (virt ? 2.5 : 1.2) << "\""
            << (virt ? " stroke-dasharray=\"6 4\"" : "") << "/>\n";
    }

    // ---- 3 階層目: ノード ----
    for (const auto *f : g.functions()) {
        out << "<circle cx=\"" << X(f->pos.x) << "\" cy=\"" << Y(f->pos.y)
            << "\" r=\"" << 0.42f * scale << "\" fill=\"" << role_fill(f->role, 0.9)
            << "\" stroke=\"#0d1117\" stroke-width=\"1\"/>\n";
    }
    // 仮想ノード
    for (const auto *v : g.virtual_nodes()) {
        out << "<rect x=\"" << X(v->pos.x) - 0.55f * scale
            << "\" y=\"" << Y(v->pos.y) - 0.35f * scale
            << "\" width=\"" << 1.1f * scale << "\" height=\"" << 0.7f * scale
            << "\" rx=\"3\" fill=\"#e3b341\" fill-opacity=\"0.85\"/>\n";
        out << "<text x=\"" << X(v->pos.x) + 0.7f * scale << "\" y=\"" << Y(v->pos.y) + 4
            << "\" fill=\"#e3b341\" font-size=\"" << (11.0f)
            << "\" font-weight=\"bold\">" << esc(v->label()) << "</text>\n";
    }

    // 凡例 (role の色)
    out << "<text x=\"12\" y=\"16\" fill=\"#8b949e\" font-size=\"11\">"
        << "nx graph — roles(1) / rings(2, dashed=virtual) / functions(3) — "
        << g.functions().size() << " functions, " << g.rings().size() << " rings, "
        << g.roles().size() << " roles</text>\n";
    out << "</svg>\n";
}

void AsciiRenderer::render(const Graph &g, std::ostream &out) const {
    out << "roles=" << g.roles().size() << "  rings=" << g.rings().size()
        << "  functions=" << g.functions().size() << "\n";
    for (const auto *rg : g.roles()) {
        out << "role " << rg->role << "  box=(" << rg->box.x << "," << rg->box.y << ","
            << rg->box.w << "," << rg->box.h << ")\n";
        for (const auto *r : rg->rings) {
            out << "  " << (r->is_virtual() ? "[virtual] " : "[ring]    ")
                << r->label() << "  members=" << r->members.size() << "\n";
        }
    }
    for (const auto *v : g.virtual_nodes())
        out << "virtual node: " << v->label() << " at (" << v->pos.x << "," << v->pos.y << ")\n";
}

} // namespace nx
