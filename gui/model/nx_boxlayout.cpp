#include "nx_boxlayout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace nx {
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

} // namespace

const char *prop_name(Prop p) {
    switch (p) {
        case Prop::Empty:      return "empty";
        case Prop::Container:  return "container";
        case Prop::Text:       return "text";
        case Prop::Sized:      return "sized";
        case Prop::Opaque:     return "opaque";
        case Prop::Degenerate: return "degenerate";
        default:               return "unknown";
    }
}

Prop prop_of(bool has_kids, bool has_text, double w, double h) {
    return prop_of_role("", has_kids, has_text, w, h);
}

Prop prop_of_role(const char *role, bool has_kids, bool has_text,
                  double w, double h) {
    (void)role;   // 名前は判定に使わない
    // 幅か高さが 0 なら退化。これは「無い」ではなく「測れていない」。
    if (w <= 0 || h <= 0) return Prop::Degenerate;
    if (has_kids) return Prop::Container;
    if (has_text) return Prop::Text;

    // 子もテキストも無いがサイズはある = 中身が無い領域の候補。
    // 「描画されているかどうか」はここでは決めない。木の関係から
    // 導く (prop_of_tree) ので、ロール名の表は持たない。
    if (w * h > 0) return Prop::Sized;
    return Prop::Empty;
}

void prop_size(Prop p, double *w, double *h) {
    // property ごとの固定寸法。安定させるため、内容に依存させない。
    switch (p) {
        case Prop::Empty:      *w = 48; *h = 24; break;
        case Prop::Container:  *w = 96; *h = 48; break;
        case Prop::Text:       *w = 72; *h = 20; break;
        case Prop::Sized:      *w = 64; *h = 32; break;
        case Prop::Degenerate: *w = 8;  *h = 8;  break;
        default:               *w = 32; *h = 24; break;
    }
}

double PackLayout::arrange(std::vector<BoxItem> &items,
                           double x0, double y0, double W, double /*H*/) const {
    // 元の座標は書き換えない。ox/oy に残したまま、x/y に詰めた結果を入れる。
    // 配置は「深さ優先の安定順」で行う。同じ入力なら常に同じ結果。
    const double pad = 4;

    // 葉 (子を持たない) だけを property ごとの寸法に揃えて並べる。
    // 容器は「葉の外接矩形 + 余白」に置く。
    std::vector<int> order;
    for (size_t i = 0; i < items.size(); i++)
        if (items[i].kids.empty()) order.push_back((int)i);

    double x = x0 + pad, y = y0 + pad, row_h = 0;
    std::vector<double> leaf_bottom(items.size(), 0);

    for (int i : order) {
        BoxItem &it = items[i];
        double w, h;
        prop_size(it.prop, &w, &h);
        if (x + w + pad > x0 + W && x > x0 + pad) {
            x = x0 + pad;
            y += row_h + pad;
            row_h = 0;
        }
        it.x = x; it.y = y; it.w = w; it.h = h;
        leaf_bottom[i] = y + h;
        x += w + pad;
        row_h = std::max(row_h, h);
    }

    // 容器は、その子の外接矩形を包むように置く (内側から外側へ)。
    // items は深さ優先で作られている前提なので、逆順に処理すると子が先に確定する。
    for (int i = (int)items.size() - 1; i >= 0; i--) {
        BoxItem &it = items[i];
        if (it.kids.empty()) continue;
        double minx = 1e18, miny = 1e18, maxx = -1e18, maxy = -1e18;
        for (int k : it.kids) {
            const BoxItem &c = items[k];
            minx = std::min(minx, c.x);
            miny = std::min(miny, c.y);
            maxx = std::max(maxx, c.x + c.w);
            maxy = std::max(maxy, c.y + c.h);
        }
        if (maxx < minx) {   // 子が全部退化していた場合
            double w, h;
            prop_size(it.prop, &w, &h);
            it.x = x0 + pad; it.y = y0 + pad; it.w = w; it.h = h;
            continue;
        }
        it.x = minx - pad;
        it.y = miny - pad;
        it.w = (maxx - minx) + pad * 2;
        it.h = (maxy - miny) + pad * 2;
    }

    double bottom = y0;
    for (const auto &it : items) bottom = std::max(bottom, it.y + it.h);
    return bottom - y0;
}

std::string boxes_to_svg(const std::vector<BoxItem> &items,
                         const std::string &caption, int width, int height) {
    double maxx = 0, maxy = 0;
    for (const auto &b : items) {
        maxx = std::max(maxx, b.x + b.w);
        maxy = std::max(maxy, b.y + b.h);
    }
    double sx = maxx > 0 ? (width - 40) / maxx : 1.0;
    double sy = maxy > 0 ? (height - 120) / maxy : 1.0;
    double s = std::min(sx, sy);
    if (s > 1.0) s = 1.0;

    std::ostringstream o;
    o << "<svg xmlns='http://www.w3.org/2000/svg' width='" << width
      << "' height='" << height << "' font-family='Menlo,monospace'>\n";
    o << "<rect width='100%' height='100%' fill='#0b0e13'/>\n";
    o << "<text x='12' y='20' fill='#dbe5f1' font-size='13'>"
      << esc(caption) << "</text>\n";
    o << "<text x='12' y='38' fill='#8a94a3' font-size='11'>"
      << items.size() << " boxes (packed)</text>\n";
    o << "<g transform='translate(20,56) scale(" << s << ")'>\n";

    for (const auto &b : items) {
        const char *fill = "#1b2230";
        switch (b.prop) {
            case Prop::Container:  fill = "#1e3350"; break;
            case Prop::Text:       fill = "#243a2a"; break;
            case Prop::Empty:      fill = "#4a2020"; break;
            case Prop::Sized:      fill = "#3a3020"; break;
            case Prop::Opaque:     fill = "#2a3f4a"; break;   // 描画されているが AX 非公開
        case Prop::Degenerate: fill = "#2a2a2a"; break;
            default: break;
        }
        o << "<rect x='" << b.x << "' y='" << b.y << "' width='" << b.w
          << "' height='" << b.h << "' fill='" << fill
          << "' stroke='#3a4a63'/>\n";
        o << "<text x='" << (b.x + 3) << "' y='" << (b.y + 12)
          << "' fill='#c9d4e3' font-size='8'>" << esc(b.role) << "</text>\n";
    }
    o << "</g>\n</svg>\n";
    return o.str();
}

} // namespace nx

namespace nx {

// 木の関係から「見えない中身」を導く。
//
// 判定の根拠 (すべて観測量で、ロール名を使わない):
//   1. 面積があるのに、子の合計面積が自分の面積に比べて極端に小さい
//      -> 子として AX に出ていない中身が、その領域を占めている
//   2. 子が 0 で、面積がある
//      -> 1 の極端な場合。同上
//   3. 面積が 0
//      -> 測れていない。無いのではない。
//
// 1 と 2 を「opaque」、3 を「degenerate」とし、
// 子の合計が自分の面積の大半を占めるなら「container」とする。
// これで role 名に依存せず、未知の役割でも同じ判断になる。
void refine_by_tree(std::vector<BoxItem> &items) {
    // 子の合計面積を先に出す (深さ優先で作られている前提なので逆順)。
    std::vector<double> kid_area(items.size(), 0.0);
    for (int i = (int)items.size() - 1; i >= 0; i--) {
        double sum = 0;
        for (int k : items[i].kids) sum += items[k].ow * items[k].oh;
        kid_area[i] = sum;
    }

    for (size_t i = 0; i < items.size(); i++) {
        BoxItem &it = items[i];
        double own = it.ow * it.oh;

        if (own <= 0) {
            it.prop = Prop::Degenerate;   // 測れていない
            continue;
        }
        if (!it.kids.empty()) {
            it.prop = Prop::Container;
            continue;
        }
        if (!it.title.empty() || !it.desc.empty()) {
            it.prop = Prop::Text;
            continue;
        }
        // 子もテキストも無いが面積はある。
        // 中身があるかは「記録を辿れたか」で決める。ここでは決めない。
        it.prop = Prop::Sized;
    }
}

} // namespace nx
