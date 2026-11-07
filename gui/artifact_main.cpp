// artifact_main.cpp — 「消しても復活する残骸」を検出する CLI
//
//   neodax-artifact [root] [--target PATH] [--json] [--commands]
//
//   root     走査するリポジトリ (既定はカレント)
//   --target この 1 ファイルを復活させうる残骸に絞る (例: gui/neodax-viewer.mm)
//   --json   機械可読
//   --commands 復活を止めるためのコマンド列を出す (実行はしない)
//
// 判定は gui/model/nx_artifact.{hpp,cpp} の多相に閉じている。
// ここには kind 分岐を書かない。

#include "model/nx_artifact.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using namespace nx;

static void print_human(const ArtifactScan &s, const std::string &root,
                        const std::string &target, bool commands) {
    std::printf("neodax-artifact: 復活する残骸の検出\n\n");
    std::printf("走査したのは %s です。\n", root.c_str());
    std::printf("見つかった残骸は %zu 件であり、最大の重大度は %s です。\n",
                s.count(), severity_name(s.max_severity()));
    if (s.count() == 0) {
        std::printf("残骸はありません。消したものが復活するなら、git の追跡か"
                    "外部ツールの再生成を疑ってください。\n");
        return;
    }

    std::printf("\n残骸の一覧は、次のとおりです。\n");
    for (auto &a : s.all())
        std::printf("  [%-8s] %s\n", severity_name(a->severity()), a->summary().c_str());

    if (!target.empty()) {
        std::printf("\n結論は、次のとおりです。\n%s", s.verdict(target).c_str());
        if (commands) {
            const auto cmds = s.removal_commands(target);
            std::printf("\n復活を止めるコマンドは、次のとおりです (実行はしていません)。\n");
            for (auto &c : cmds) std::printf("  %s\n", c.c_str());
        }
    }
}

static void print_json(const ArtifactScan &s, const std::string &root,
                       const std::string &target) {
    std::printf("{\n");
    std::printf("  \"root\": \"%s\",\n", root.c_str());
    std::printf("  \"count\": %zu,\n", s.count());
    std::printf("  \"max_severity\": \"%s\",\n", severity_name(s.max_severity()));
    std::printf("  \"artifacts\": [\n");
    size_t i = 0;
    for (auto &a : s.all()) {
        std::printf("    { \"kind\": \"%s\", \"path\": \"%s\", \"severity\": \"%s\", "
                    "\"mechanism\": \"%s\", \"size\": %llu }%s\n",
                    a->kind(), a->path.c_str(), severity_name(a->severity()),
                    a->restore_mechanism(), (unsigned long long)a->size,
                    ++i < s.count() ? "," : "");
    }
    std::printf("  ],\n");
    if (!target.empty()) {
        const auto hit = s.affecting(target);
        std::printf("  \"target\": \"%s\",\n", target.c_str());
        std::printf("  \"affecting_count\": %zu,\n", hit.size());
        std::printf("  \"revives\": %s\n", hit.empty() ? "false" : "true");
    } else {
        std::printf("  \"target\": null,\n  \"revives\": null\n");
    }
    std::printf("}\n");
}

int main(int argc, char **argv) {
    std::string root = ".";
    std::string target;
    bool json = false, commands = false;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--json") == 0) json = true;
        else if (std::strcmp(argv[i], "--commands") == 0) commands = true;
        else if (std::strcmp(argv[i], "--target") == 0 && i + 1 < argc) target = argv[++i];
        else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::printf("usage: %s [root] [--target PATH] [--json] [--commands]\n", argv[0]);
            std::printf("  消しても復活する残骸 (checkpoint / patch / archive / 手順書 / backup)"
                        " を検出する。\n");
            return 0;
        } else if (argv[i][0] != '-') root = argv[i];
    }

    ArtifactScan scan;
    scan.use_default_rules();
    scan.run(root);

    if (json) print_json(scan, root, target);
    else print_human(scan, root, target, commands);

    // target が指定され、復活させうる残骸がある場合は非 0 で返す (CI 用)
    if (!target.empty() && !scan.affecting(target).empty()) return 2;
    return 0;
}
