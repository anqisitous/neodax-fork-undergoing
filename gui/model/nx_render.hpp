// nx_render.hpp — 描画の多相設計 (AppKit / Objective-C 非依存)
//
// Renderer が抽象。SvgRenderer / AsciiRenderer が実装。
// Graph 側の Node/Group も多相なので、描画側に種別分岐を書かずに済む。

#ifndef NX_RENDER_HPP
#define NX_RENDER_HPP

#include <iosfwd>

#include "nx_graph.hpp"

namespace nx {

class Renderer {
public:
    virtual ~Renderer() = default;
    virtual const char *name() const = 0;
    virtual void render(const Graph &g, std::ostream &out) const = 0;
};

// 自己完結の SVG。依存ゼロで、ブラウザ/Preview でそのまま見られる。
class SvgRenderer : public Renderer {
public:
    const char *name() const override { return "svg"; }
    void render(const Graph &g, std::ostream &out) const override;

    float margin = 24.0f;
    float scale = 12.0f;   // ワールド単位 → px
};

// 端末向けの簡易表示 (デバッグ・CI 用)
class AsciiRenderer : public Renderer {
public:
    const char *name() const override { return "ascii"; }
    void render(const Graph &g, std::ostream &out) const override;
};

} // namespace nx

#endif // NX_RENDER_HPP
