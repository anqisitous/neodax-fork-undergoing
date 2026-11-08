// nx_uilayout.hpp — アプリ画面の「状態から再構築したレイアウト」を SVG にする
//
// なぜ要るか:
//   display は実キャプチャと状態再構築の両方を扱うが、結果はテキスト/PNG だけで、
//   GUI (viewer) では見られなかった。撮れない環境 (起動していないアプリ、
//   画面収録の権限が無い場合) でも、状態から組んだレイアウトを目で確認したい。
//
// 入力は nx_ui_layout_t (ノード + エッジ + タブ情報)。ここでは描画だけを行う。

#ifndef NX_UILAYOUT_HPP
#define NX_UILAYOUT_HPP

#include <string>

#include "integration/nx_deepai.h"

namespace nxui {

// layout を SVG 文字列にする。caption はタイトル行。
std::string to_svg(const nx_ui_layout_t &l, const std::string &caption);

// ファイルへ書く。成功で true。
bool write_svg(const nx_ui_layout_t &l, const std::string &caption,
               const std::string &path, std::string &err);

} // namespace nxui

#endif
