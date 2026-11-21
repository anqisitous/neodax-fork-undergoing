// ===== NX TEMPLATE =====
// id: rotation.linear
// kind: motion
// target: css-transform / keyframes
// learn: graph=rotation-v1
//
// 回転分析の思想
// ==============
//
// 回転は「状態」ではない。
//
// 画面が連続して回っているとき、アプリの状態は止まっていることが多い。
// 毎フレーム変わっているのは合成層の transform であって、モデルの値ではない。
// だから回転を「値の更新」として追うと、必ず取りこぼす。
// 追うべきなのは次の段。
//
//   source      … どの規則が回転を宣言しているか
//   axis        … どの軸か (z / x / y / 任意の 3d ベクトル)
//   keyframes   … 角度の離散点 (from/to、必要なら中間点)
//   linear      … 点の間をどう埋めるか
//   timing      … 時間方向の埋め方 (linear / steps / ease)
//   iteration   … 同じ区間を繰り返すか (1 / infinite)
//   compositor  … transform を合成層へ渡す
//   render      … 実際に画面へ出る結果
//
// 「回転している」を 1 ノードで表さない。1 ノードにすると、
// 角度の発生源と補間と合成が同じ箱に入り、どこを直せばいいか分からなくなる。
//
// このファイルは mechanism (仕組み) だけを持つ。アプリ名は書かない。
// 実在する selector / file / axis / from / to は data (ui.tsv) 側に置く。
//
// node: motion.source    回転を宣言している規則 (selector / JS / keyframes)
// node: motion.axis      回転軸。z は画面内、x/y は 3D、3d は軸ベクトル
// node: motion.keyframes 角度の離散点 (from/to、必要なら中間点)
// node: motion.linear    点の間を線形補間する
// node: motion.timing    時間方向の埋め方 (linear / steps / ease)
// node: motion.iteration 同じ区間を繰り返す (1 / infinite)
// node: motion.compositor transform を合成層へ渡す
// node: motion.render    合成結果 (transform / will-change / compositor)
//
// link: motion.axis requires motion.source
// link: motion.keyframes requires motion.source
// link: motion.linear requires motion.keyframes
// link: motion.timing requires motion.linear
// link: motion.iteration requires motion.timing
// link: motion.compositor requires motion.linear
// link: motion.compositor requires motion.axis
// link: motion.render requires motion.compositor
// link: motion.linear loops motion.compositor
// ===== /NX TEMPLATE =====

/*
 * 実装の場所
 * ----------
 * この .cpp はビルド対象ではない。tpl-cpp / trace が構成を読むための model。
 * 実際に回転を抽出して cpp を出すのは:
 *
 *   src/integration/plan/nx_ui.cpp
 *     read_rotation()       … CSS/JS の transform から axis/vec/from/to を読む
 *     rotation_value()      … periodic/turns を計算して ui.tsv の 1 行にする
 *     emit_rotation_cpp()   … 観測行から actual cpp を出す
 *
 * このファイルに実行ロジックを書かない。ここは「形」と「禁止事項」
 *
 * データと仕組みを混ぜない。
 *   mechanism … このファイル。アプリ名を書かない。
 *   data      … ui-scan が吐く ui.tsv の rotation 行。
 *               実在する selector / file / axis / vec / from / to /
 *               periodic / turns だけ。
 *
 * 生成される actual cpp (ui-flow --format rotcpp) は data から作る。
 * このファイルは、その actual cpp が従うべき形を定義する。
 */

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace nx_rotation_model {

/* 軸は必ずベクトルで持つ。z/x/y も 3d も同じ表現にする。
   rotate3d(2,0,0,90deg) は正規化して (1,0,0) になる。 */
struct Vec3 { double x, y, z; };

struct Rotation {
    const char  *keyframes;   /* @keyframes 名 / JS の selector */
    const char  *file;        /* 定義があるファイル */
    const char  *axis_name;   /* z / x / y / 3d (表示用) */
    Vec3         axis;        /* 実際の軸。3d でも一意 */
    double       from_deg;    /* 開始角 (度に正規化済み) */
    double       to_deg;      /* 終了角 (度に正規化済み) */
    bool         periodic;    /* to-from が 360 の整数倍のときだけ true */
    int          turns;       /* periodic のときの周回数 (0→720 なら 2) */
};

inline double angle_at(const Rotation &r, double t, double duration) {
    if (duration <= 0.0) return r.from_deg;
    double u = t / duration;
    if (r.periodic) {
        u = std::fmod(u, 1.0);
        if (u < 0.0) u += 1.0;
    } else {
        u = std::min(1.0, std::max(0.0, u));
    }
    return r.from_deg + (r.to_deg - r.from_deg) * u;
}

/* 任意軸回転 (Rodrigues)。軸を正規化してから使う。
   z のときは 2D に縮むが、同じ関数で扱う。
   m は row-major の 3x3。t は 1-c で、時間ではない。 */
inline void matrix(const Vec3 &a, double deg, double m[9]) {
    for (int i = 0; i < 9; i++) m[i] = (i % 4 == 0) ? 1.0 : 0.0;
    const double n = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    if (n < 1e-12) return;
    const double x = a.x / n, y = a.y / n, z = a.z / n;
    const double rad = deg * 3.14159265358979323846 / 180.0;
    const double c = std::cos(rad), s = std::sin(rad), t = 1.0 - c;
    m[0] = t*x*x + c;   m[1] = t*x*y - s*z; m[2] = t*x*z + s*y;
    m[3] = t*x*y + s*z; m[4] = t*y*y + c;   m[5] = t*y*z - s*x;
    m[6] = t*x*z - s*y; m[7] = t*y*z + s*x; m[8] = t*z*z + c;
}

} // namespace nx_rotation_model

/*
 * 次のエージェントへ。
 * ここを読まずに触ると、たぶんこれを壊す。
 *
 * 1. from != to を「周期」と混同するな。
 *    0→90 は 1 回で止まる。周期なのは to-from が 360 の整数倍のときだけ。
 *    0→720 は 2 周 (turns=2) で、1 周ではない。
 *    from==to でも、中間点が 0→360→0 なら回転している。
 *    first/last だけを見て「回転なし」と結論するな。
 *
 * 2. Axis::ThreeD だけで済ませるな。
 *    rotate3d(x,y,z,angle) の x,y,z を捨てると、どの軸の回転か決まらない。
 *    軸は Vec3 で持つ。rotate3d(2,0,0,90) は正規化して (1,0,0) になる。
 *
 * 3. @keyframes の中間点を first/last で潰すな。
 *    0% → 50% → 100% が 0 → 270 → 360 なら、0→360 とは違う。
 *    中間点が要るなら stops を配列で持つ。今の生成器は first/last だけ。
 *    この制限を消すときは read_rotation() と emit_rotation_cpp() の両方を直す。
 *
 * 4. transform は 1 つとは限らない。
 *    translate / scale / rotate が同じ宣言に同居する。角度だけ抜いても、
 *    合成順序を保存しないと見た目は再現できない。CSS の transform は
 *    右から左へ適用される。matrix の積の順序を勝手に変えるな。
 *
 * 5. transform-origin は角度ではない。
 *    回転中心が要素の中心でない場合、同じ角度でも見た目が変わる。
 *    matrix() は原点まわりの回転であり、origin の移動は別。
 *
 * 6. CSS の rotate 個別プロパティと transform: rotate() は別経路。
 *    どちらも拾うが、同じものとして重複カウントするな。
 *    rotate: 180deg は transform ではない。
 *
 * 7. animation-timeline は時間ではない。
 *    scroll() / view() のとき duration=1ms でもスクロールで進む。
 *    duration は「時間あたりの速度」ではなく「進行の正規化」として扱う。
 *
 * 8. JS の inline transform は CSS に出ない。
 *    ui-scan は文字列リテラルも見る。CSS だけ見て「回転なし」と結論するな。
 *
 * 9. 角度の単位を混ぜるな。
 *    deg / turn / rad を度に正規化してから from/to に入れる。
 *    1turn を 1 度として扱うと全部壊れる。
 *
 * 10. 負角・逆回転を落とすな。
 *     rotate(-90deg) は方向が逆なだけで、回転がないわけではない。
 *     負の turns もあり得る。
 *
 * 11. 静的解析であって実行時の computed style ではない。
 *     見つからないものを見つかったことにしない。binding が取れないなら
 *     取れないと書く。推測で link を張らない。
 *     calc() / CSS 変数 / 実行時の style 注入は解決できないことがある。
 *
 * 12. アプリ名をコードに書くな。
 *     Spotify / 特定の keyframes 名 / 特定の CSS をコードに埋めない。
 *     それらは data。コードは任意の ui.tsv に対して同じように動く。
 *
 * 13. loops は requires でも converges でもない。
 *     tpl-cpp の ring/order は requires と converges だけを見る。
 *     loops は「周期」を表示するための印で、解析の辺ではない。
 *     周期をグラフの辺として扱いたいなら tplcpp 側を拡張する。
 *
 * 14. @keyframes 名はファイルをまたぐと重複する。
 *     key は名前だけでなく file も含める。名前だけで dedupe するな。
 *
 * 15. rotate という語は rotate3d の中にも現れる。
 *     単純に "rotate" を探すと 1 つの宣言を二重に拾うことがある。
 *     read_rotation() は関数名と括弧を見て 1 回だけ数える。
 *
 * 16. 変更したら data を作り直す。
 *     rotation_value() の出力を変えたら ui-scan をやり直す。
 *
 * 17. 生成した cpp は必ず構文検査する。
 *     clang++ -std=c++17 -fsyntax-only <generated.cpp>
 *     actual cpp は data 次第で 0 件にもなる。0 件でも配列は壊さない。
 *
 * 18. 2D (z) は 2x2 で足りるので、2x2しかその回転で使わないなら z 軸を捨てる。
 */
