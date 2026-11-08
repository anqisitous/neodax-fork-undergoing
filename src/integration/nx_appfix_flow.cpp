// nx_appfix_flow.cpp — AE の fork を作り、サインイン経路を無効化して保存する
//
// 1 コマンドで 複製 -> パッチ -> 差分検証 -> 再署名 -> 検証 まで通す。
// 途中で失敗したら止め、**元のアプリには触らない**。
//
// なぜ「最新版のみ保存」か:
//   以前は /tmp に複数のコピーが散らばり (ae-emu, ae-fix.app, clonetest)、
//   どれが最新か分からなくなった。保存先を 1 つに固定し、成功時だけ置き換える。
//
// パッチ内容 (静的に確定済み):
//   aelicensing::ShouldCheckLicense (0x5e9b74) を「常に false」にする。
//   呼び出し元は cbz の分岐のみで、戻り値に副作用はない。

#include "nx_appfix_flow.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace {

const char *kAfterFxLib =
    "/Contents/Frameworks/AfterFXLib.framework/Versions/A/AfterFXLib";

// ShouldCheckLicense の範囲 (次の関数 DoSetup の手前まで)
const uint64_t kShouldCheckLicense = 0x5e9b74;
const uint64_t kNextFunction       = 0x5e9c60;

bool exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

} // namespace

extern "C" int nx_appfix_make(const char *src_app, const char *dst_app,
                              int force, FILE *out) {
    if (!out) out = stdout;
    if (!src_app || !dst_app) return 2;

    if (!exists(src_app)) {
        std::fprintf(out, "  [fix] 元アプリが見つかりません: %s\n", src_app);
        return 2;
    }
    std::string src_lib = std::string(src_app) + kAfterFxLib;
    std::string lib     = std::string(dst_app) + kAfterFxLib;
    if (!exists(src_lib)) {
        std::fprintf(out, "  [fix] AfterFXLib がありません: %s\n", src_lib.c_str());
        return 2;
    }

    if (exists(dst_app) && !force) {
        std::fprintf(out, "  [fix] 既にあります: %s (置き換えるには --force)\n", dst_app);
        return 1;
    }

    // 1. 複製 (APFS クローン。元は触らない)
    {
        std::string cmd = "rm -rf '" + std::string(dst_app) + "' && cp -c -R '"
                        + std::string(src_app) + "' '" + std::string(dst_app) + "'";
        std::fprintf(out, "  [fix] clone -> %s\n", dst_app);
        if (std::system(cmd.c_str()) != 0) {
            std::fprintf(out, "  [fix] 複製に失敗\n");
            return 1;
        }
    }

    // 2. パッチ: mov w0,#0 ; ret ; 以降 nop
    {
        extern int nx_patch_bytes(const char *, uint64_t, const char *, FILE *);
        std::string hex = "00008052 c0035fd6";
        for (uint64_t v = kShouldCheckLicense + 8; v < kNextFunction; v += 4)
            hex += " 1f2003d5";
        if (nx_patch_bytes(lib.c_str(), kShouldCheckLicense, hex.c_str(), out) != 0) {
            std::fprintf(out, "  [fix] パッチに失敗\n");
            return 1;
        }
        std::fprintf(out, "  [fix] patch: ShouldCheckLicense -> ret 0 (%llu bytes)\n",
                     (unsigned long long)(kNextFunction - kShouldCheckLicense));
    }

    // 3. 差分検証: 変わったのが連続した 1 区間だけか
    {
        std::ifstream a(src_lib, std::ios::binary), b(lib, std::ios::binary);
        if (!a || !b) { std::fprintf(out, "  [fix] 比較用に開けません\n"); return 1; }
        std::vector<char> va((std::istreambuf_iterator<char>(a)), {});
        std::vector<char> vb((std::istreambuf_iterator<char>(b)), {});
        if (va.size() != vb.size()) {
            std::fprintf(out, "  [fix] サイズが変わりました (同長のはず)\n");
            return 1;
        }
        size_t first = va.size(), last = 0, ndiff = 0;
        for (size_t i = 0; i < va.size(); i++) {
            if (va[i] == vb[i]) continue;
            ndiff++;
            if (first == va.size()) first = i;
            last = i;
        }
        std::fprintf(out, "  [fix] diff: %zu bytes, file 0x%zx..0x%zx\n",
                     ndiff, first, last);
        if (ndiff == 0) {
            std::fprintf(out, "  [fix] 差分がありません (パッチが効いていない)\n");
            return 1;
        }
        // 期待: 変更が ShouldCheckLicense の範囲内に収まっていること。
        // 元のバイトが偶然 nop と同じだと差分は飛び飛びになるので、
        // 「連続か」ではなく「範囲内か」で判定する。
        // 範囲: パッチ時に得た先頭 offset から関数サイズ分。
        {
            extern int nx_vaddr_to_off(const char *, uint64_t, uint64_t *);
            uint64_t lo = 0, hi = 0;
            int ok = (nx_vaddr_to_off(lib.c_str(), kShouldCheckLicense, &lo) == 0 &&
                      nx_vaddr_to_off(lib.c_str(), kNextFunction, &hi) == 0);
            if (!ok) {
                // 変換できなければ、分かっている値をそのまま使う
                lo = 0x5edb74;
                hi = lo + (kNextFunction - kShouldCheckLicense);
            }
            std::fprintf(out, "  [fix] 期待範囲: file 0x%llx..0x%llx\n",
                         (unsigned long long)lo, (unsigned long long)hi);
            if (first < (size_t)lo || last >= (size_t)hi) {
                std::fprintf(out, "  [fix] 想定外の範囲が変わりました。中止\n");
                return 1;
            }
        }
    }

    // 4. 再署名 (ad-hoc)
    {
        std::fprintf(out, "  [fix] resign (adhoc)\n");
        std::string cmd = "codesign --force --deep --sign - '" + std::string(dst_app) + "'";
        if (std::system(cmd.c_str()) != 0) {
            std::fprintf(out, "  [fix] 再署名に失敗\n");
            return 1;
        }
    }

    // 5. 検証
    {
        std::fprintf(out, "  [fix] verify\n");
        std::string cmd = "codesign --verify --strict '" + std::string(dst_app) + "' 2>&1";
        if (std::system(cmd.c_str()) != 0) {
            std::fprintf(out, "  [fix] 署名検証に失敗\n");
            return 1;
        }
    }

    std::fprintf(out, "  [fix] done: %s\n", dst_app);
    return 0;
}
