// nx_emulator.hpp — AE 画面のエミュレータ (別フレーム)
//
// なぜ別フレームか:
//   neodax のコード/プロパティパネルと、AE 画面のエミュレータは
//   見る対象が違う。混ぜると「どちらの数値か」が分からなくなる。
//   よって独立したウィンドウに置き、データだけを共有する。
//
// 操作モードは 2 つだけ。トグルもドロップダウンも作らない:
//   Inspect  … 範囲選択してウィジェットを解析する (画面は動かさない)
//   Operate  … クリックで実際に操作する
// モードはボタン 2 つで切り替える (押されている方だけが有効)。

#ifndef NX_EMULATOR_HPP
#define NX_EMULATOR_HPP

#include <string>
#include <vector>

struct DVNode;

namespace nxemu {

enum class Mode { Inspect, Operate };

const char *mode_name(Mode m);

// 画面上の 1 ウィジェット。エミュレータが「押せる四角」として扱う。
struct Widget {
    const DVNode *node = nullptr;
    double x = 0, y = 0, w = 0, h = 0;
    int    depth = 0;
    std::string role;
    bool   hit(double px, double py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// モデルからウィジェット一覧を作る (深さ優先・安定順)。
std::vector<Widget> widgets(const DVNode &root);

// 矩形で範囲選択する。Inspect モードで使う。
std::vector<Widget> select_in_rect(const std::vector<Widget> &ws,
                                   double x0, double y0, double x1, double y1);

// 1 点をクリックしたときの最も内側のウィジェット。Operate モードで使う。
const Widget *pick(const std::vector<Widget> &ws, double px, double py);

// 選択結果の説明 (プロパティパネルへ渡す文字列)。
std::string describe(const Widget &w);

} // namespace nxemu

#endif
