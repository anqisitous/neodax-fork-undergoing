// viewer_main.cpp — NeoDAX viewer (純 C++ / Objective-C なし)
//
// 旧 gui/neodax-viewer.mm を置き換える。
//   ・モデルと配置は gui/model/nx_graph.{hpp,cpp}  (多相)
//   ・描画は gui/model/nx_render.{hpp,cpp}         (多相)
//   ・ここは組み立てと入出力だけ
//
// ビルド:
//   clang++ -std=c++17 -O2 -I src -I include -I gui \
//       gui/viewer_main.cpp gui/model/nx_graph.cpp gui/model/nx_render.cpp <解析オブジェクト> \
//       -lpthread -lm -o bin/neodax-viewer
//
// 使い方:
//   neodax-viewer <mach-o> [-o out.svg] [--ascii]

#include "model/nx_graph.hpp"
#include "model/nx_render.hpp"

extern "C" {
#include "integration/neodax_bridge.h"
}

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr int kPaletteN = 12;

// 役割名 → パレット番号。安定したハッシュで、同じ役割は常に同じ色になる。
int rolePal(const std::string &r) {
    if (r.empty()) return kPaletteN - 1;
    unsigned h = 2166136261u;
    for (unsigned char c : r) { h ^= c; h *= 16777619u; }
    return (int)(h % (unsigned)kPaletteN);
}

std::vector<uint8_t> read_file(const char *path, bool *ok) {
    *ok = false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    if (b.empty()) return {};
    *ok = true;
    return b;
}

} // namespace

int main(int argc, char **argv) {
    const char *path = nullptr;
    const char *out_path = nullptr;
    bool ascii = false;

    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!std::strcmp(argv[i], "--ascii")) ascii = true;
        else if (argv[i][0] != '-') path = argv[i];
    }
    if (!path) {
        std::fprintf(stderr,
            "usage: %s <mach-o> [-o out.svg] [--ascii]\n"
            "  role(1) -> ring(2) -> function(3) の 3 階層を描く。\n"
            "  状態依存の環が同じ role に複数あるときは仮想ノードを 2 階層目に出す。\n",
            argv[0]);
        return 2;
    }

    bool ok = false;
    std::vector<uint8_t> data = read_file(path, &ok);
    if (!ok) { std::fprintf(stderr, "cannot read: %s\n", path); return 1; }

    integrated_binary_t ibin;
    if (neodax_bridge_init(&ibin, data.data(), data.size()) != 0) {
        std::fprintf(stderr, "not a Mach-O binary: %s\n", path);
        return 1;
    }

    static_analysis_opts_t opts;
    std::memset(&opts, 0, sizeof(opts));
    opts.num_threads = 4;
    FILE *devnull = std::fopen("/dev/null", "w");
    neodax_run_static_analysis(&ibin, &opts, devnull ? devnull : stderr);
    if (devnull) std::fclose(devnull);

    macho_binary_t *bin = &ibin.macho_bin;
    depflow_graph_t *dg = ibin.dep_graph;
    if (!dg) {
        std::fprintf(stderr, "no dependency graph for: %s\n", path);
        neodax_bridge_free(&ibin);
        return 1;
    }

    // ---- モデル構築 (多相) ----
    nx::Graph g;
    const int nf = bin->nfunctions;
    for (int i = 0; i < nf; i++) {
        char role[64];
        if (!depflow_function_role(bin, dg, i, role, sizeof(role)))
            std::snprintf(role, sizeof(role), "misc");
        const int ring = (dg->summaries && i < dg->summary_count)
                             ? dg->summaries[i].ring_id : -1;
        g.add_function(bin->functions[i].start,
                       bin->functions[i].name,
                       rolePal(role), ring);
    }
    g.build_hierarchy();
    g.layout();

    // ---- 描画 (多相) ----
    std::unique_ptr<nx::Renderer> renderer;
    if (ascii) renderer.reset(new nx::AsciiRenderer());
    else       renderer.reset(new nx::SvgRenderer());

    if (out_path) {
        std::ofstream out(out_path, std::ios::binary);
        if (!out) { std::fprintf(stderr, "cannot write: %s\n", out_path); neodax_bridge_free(&ibin); return 1; }
        renderer->render(g, out);
        std::fprintf(stderr, "wrote %s (%s)\n", out_path, renderer->name());
    } else {
        renderer->render(g, std::cout);
    }

    neodax_bridge_free(&ibin);
    return 0;
}
