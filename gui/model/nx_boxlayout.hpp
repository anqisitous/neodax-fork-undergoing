// nx_boxlayout.hpp — 矩形 (AX ノード) を「隙間を埋める」形で配置し直す
//
// なぜ要るか:
//   AX ツリーの矩形は、空ノードが 0x0 だったり、親と同じ大きさだったりして
//   隙間と重なりが多い。そのまま描くと「どこが空か」が読めない。
//   ここでは各ノードに property (空 / 子持ち / テキスト有り / サイズ異常) を
//   持ちたせ、property ごとに決めた寸法で**隙間なく**並べ直す。
//
// nx::Layout との関係:
//   nx::Layout はグラフのノード (点) を環状に置く抽象。こちらは矩形を
//   詰める別の抽象で、同じ「arrange が配置を決める」形を保つ。
//   neodax 自身は元の座標を書き換えない。ここで新しい座標を計算するだけ。

#ifndef NX_BOXLAYOUT_HPP
#define NX_BOXLAYOUT_HPP

#include <string>
#include <vector>

namespace nx {

// AX ノードの性質。描画と一覧の両方で使う。
enum class Prop {
    Empty,        // 子も title も desc も無い = 中身が無い
    Container,    // 子を持つ
    Text,         // title か desc を持つ
    Sized,        // 子もテキストも無いがサイズはある (本当に空の可能性)
    Opaque,       // 面積はあるが中身が AX に出ていない (Web 面など)。空ではない
    Degenerate,   // 幅か高さが 0
    Unknown,
};

const char *prop_name(Prop p);
Prop prop_of(bool has_kids, bool has_text, double w, double h);

// 役割も見て判定する。AXWebArea のように「中身を AX に見せない」面を
// 空と誤認しないため。
Prop prop_of_role(const char *role, bool has_kids, bool has_text,
                  double w, double h);

// 1 ノード。元の矩形 (ox,oy,ow,oh) と、割り当て後の矩形 (x,y,w,h) を持つ。
struct BoxItem {
    std::string role, sub, title, desc;
    double ox = 0, oy = 0, ow = 0, oh = 0;   // 元の矩形 (変更しない)
    double x = 0, y = 0, w = 0, h = 0;       // 配置後
    Prop prop = Prop::Unknown;
    int depth = 0;
    int parent = -1;
    std::vector<int> kids;
};

class BoxLayout {
public:
    virtual ~BoxLayout() = default;
    virtual const char *name() const = 0;
    // items を (x0,y0,W,H) の領域に配置し直す。戻り値は使った高さ。
    virtual double arrange(std::vector<BoxItem> &items,
                           double x0, double y0, double W, double H) const = 0;
};

// 隙間を埋める再配置。
//   - 元の入れ子 (親子) を保ちつつ、葉を property ごとの固定寸法に揃える
//   - 子の合計が親より小さければ引き伸ばし、大きければ折り返す
//   - 空ノードも 1 マス確保する (「無い」ことが見えるように)
class PackLayout : public BoxLayout {
public:
    const char *name() const override { return "pack"; }
    double arrange(std::vector<BoxItem> &items,
                   double x0, double y0, double W, double H) const override;
};

// property ごとの固定寸法 (葉に使う)
void prop_size(Prop p, double *w, double *h);

// 木の関係から property を導き直す (ロール名を使わない)。
// 子の合計面積が自分に対して小さければ「AX に見えない中身」があると判断する。
void refine_by_tree(std::vector<BoxItem> &items);

// 配置結果を SVG にする。property ごとに色を変え、空が目立つようにする。
std::string boxes_to_svg(const std::vector<BoxItem> &items,
                         const std::string &caption, int width, int height);

} // namespace nx

#endif
