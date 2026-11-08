#include "nx_uilayout.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace nxui {
namespace {

std::string esc(const std::string &s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

const char *node_fill(int kind) {
    switch (kind) {
        case 0: return "#3a3f4b";   // input
        case 1: return "#4a7fd0";   // button
        case 2: return "#4a9f6a";   // tab active
        case 3: return "#55606e";   // tab other
        case 4: return "#c04040";   // tab stuck
        case 5: return "#5a5f48";   // table
        default: return "#3a3d44";  // text
    }
}

} // namespace

std::string to_svg(const nx_ui_layout_t &l, const std::string &caption) {
    const int W = l.width  > 0 ? l.width  : 1280;
    const int H = l.height > 0 ? l.height : 800;
    std::ostringstream o;
    o << "<?xml version='1.0' encoding='UTF-8'?>\n";
    o << "<svg xmlns='http://www.w3.org/2000/svg' width='" << W
      << "' height='" << (H + 48) << "' font-family='monospace'>\n";
    o << "<rect width='100%' height='100%' fill='#14161a'/>\n";
    o << "<text x='16' y='26' fill='#d0d4dc' font-size='15'>"
      << esc(caption) << "</text>\n";
    o << "<text x='16' y='42' fill='#7a828e' font-size='11'>"
      << l.n_nodes << " nodes, " << l.n_edges << " edges, "
      << l.n_tabs << " tabs (" << l.n_stuck << " stuck)</text>\n";
    o << "<g transform='translate(0,48)'>\n";

    // エッジ
    for (int i = 0; i < l.n_edges; i++) {
        const nx_ui_edge_t &e = l.edges[i];
        o << "<line x1='" << e.x1 << "' y1='" << (H - e.y1) << "' x2='" << e.x2
          << "' y2='" << (H - e.y2) << "' stroke='#8a7a3a' stroke-width='2'/>\n";
    }

    // ノード
    for (int i = 0; i < l.n_nodes; i++) {
        const nx_ui_node_t &nd = l.nodes[i];
        o << "<rect x='" << nd.x << "' y='" << (H - nd.y - nd.h)
          << "' width='" << nd.w << "' height='" << nd.h
          << "' fill='" << node_fill(nd.kind) << "' stroke='#6c7482'/>\n";
        if (nd.label[0]) {
            o << "<text x='" << (nd.x + 4) << "' y='" << (H - nd.y - 4)
              << "' fill='#e6e9ef' font-size='11'>" << esc(nd.label) << "</text>\n";
        }
    }
    o << "</g>\n</svg>\n";
    return o.str();
}

bool write_svg(const nx_ui_layout_t &l, const std::string &caption,
               const std::string &path, std::string &err) {
    std::ofstream f(path);
    if (!f) { err = "cannot open " + path; return false; }
    f << to_svg(l, caption);
    if (!f) { err = "write failed: " + path; return false; }
    return true;
}

} // namespace nxui

// ==================================================================
// DVNode モデル (AX ツリー) を「中身の有無」で可視化する
// ==================================================================
//
// 既存の --display-graph は AX ツリーをそのまま描く。それだと
// 「どのノードが空か」が分からない (全部同じ枠で描かれる)。
// ここでは各ノードを
//   ・実体あり (子あり / タイトル・説明あり) → 通常色
//   ・空 (子もテキストも無い)                → 赤枠 + EMPTY 表示
// に分け、空のノードを一覧でも出す。サインイン画面のように
// 「なぜか中身が無い」を探す用途に向ける。

namespace nxui {

int render_dvnodes(const char *path, const std::string &out_path,
                   std::string &err, std::string &report);

} // namespace nxui
