// nx_case_ae.cpp — After Effects の「起動→サインイン要求→終了」案件
//
// 静的に確認した依存の鎖:
//   MainMain                          (戻り値を使わない)
//     -> aelicensing::DoSetup         (大枠。ここを外す)
//          -> licensingglue::LicenseSetup
//               -> NglController 構築
//                    -> NglService::RequestAccessToken   <- サインイン要求
//
// 観測は 3 層に分ける:
//   1. 痕跡  (ファイル / NGL ログ)      … 事実
//   2. 機構  (バイナリ内の関数の有無)   … 事実
//   3. 状態  (NGL コンテナの中身)       … 事実
// 問題の判定は 1-3 を突き合わせて行い、根拠の signal id を必ず添える。

#include "nx_case.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace nx {
namespace {

std::string home() {
    const char *h = std::getenv("HOME");
    return h ? h : "";
}

std::string join(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

bool exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

bool read_small(const std::string &p, std::string &out) {
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    out.assign(buf, n);
    return true;
}

std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// team 付き NGL コンテナを探す。
std::string find_ngl() {
    std::string root = join(home(), "Library/Group Containers");
    DIR *d = opendir(root.c_str());
    if (!d) return "";
    std::string found;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n.find("com.adobe.NGL") != std::string::npos) { found = join(root, n); break; }
    }
    closedir(d);
    return found;
}

// NGL のログから「サインインを要求した根拠」を拾う。
// 推測ではなく、NGL 自身が書いた行だけを見る。
struct NglLogFacts {
    bool access_token_failed = false;   // GetAccessToken : Failed
    bool principal_failed = false;      // GetUserPrincipal failed
    bool kc_item_missing = false;       // lOsStatus: -25300
    int  lines_scanned = 0;
};

NglLogFacts scan_ngl_log() {
    NglLogFacts r;
    const char *names[] = {
        "NGLClient_AfterEffects126.3.log",
        "NGLClient_AfterEffects126.3 2026-11-08 17-53-26-954.log",
    };
    for (const char *nm : names) {
        std::string p = join(join(home(), "Library/Logs/NGL"), nm);
        FILE *f = fopen(p.c_str(), "rb");
        if (!f) continue;
        char line[8192];
        while (fgets(line, sizeof line, f)) {
            r.lines_scanned++;
            if (strstr(line, "GetAccessToken : Failed")) r.access_token_failed = true;
            if (strstr(line, "GetUserPrincipal failed")) r.principal_failed = true;
            if (strstr(line, "lOsStatus: -25300")) r.kc_item_missing = true;
        }
        fclose(f);
    }
    return r;
}

// AE の app-update 対象かどうか: Group Containers に NGL があり、
// Adobe の状態ファイルがあるなら AE 案件とみなす。
bool looks_like_ae(const CaseTarget &t) {
    if (t.kind != "app" && t.kind != "state" && t.kind != "dir") return false;
    if (!find_ngl().empty()) return true;
    return exists(join(home(), "Library/Application Support/Adobe"));
}

} // namespace

class AeLicenseCase : public Case {
public:
    const char *name() const override { return "ae-license"; }

    bool applicable(const CaseTarget &t) const override { return looks_like_ae(t); }

    void collect(const CaseTarget &t, std::vector<Signal> &out) const override {
        const std::string adobe = join(home(), "Library/Application Support/Adobe");
        const std::string ngl = find_ngl();

        // ---- 1. 痕跡 (Adobe の状態ファイル) ----
        std::string st, lic;
        bool have_st = read_small(join(adobe, ".adobestatusnotification"), st);
        bool have_lic = read_small(join(adobe, ".adobelicnotification"), lic);
        st = trim(st); lic = trim(lic);
        out.push_back({"ae.status_notification", "Adobe/.adobestatusnotification",
                       have_st ? st : "(none)", Persistence::Durable, have_st, true});
        out.push_back({"ae.lic_notification", "Adobe/.adobelicnotification",
                       have_lic ? lic : "(none)", Persistence::Durable, have_lic, true});

        // ---- 2. NGL コンテナ (V2 マーカー) ----
        std::string v2;
        bool have_v2 = false;
        if (!ngl.empty()) {
            have_v2 = read_small(join(ngl, "licflags/.adobelicnotificationV2"), v2);
            v2 = trim(v2);
        }
        out.push_back({"ae.ngl_v2", "NGL/licflags/.adobelicnotificationV2",
                       have_v2 ? v2 : "(none)", Persistence::Durable, have_v2, true});

        // ---- 3. NGL ログ: サインインを要求した直接の根拠 ----
        NglLogFacts lf = scan_ngl_log();
        out.push_back({"ae.ngl_access_token_failed", "Library/Logs/NGL",
                       lf.access_token_failed ? "failed" : "not seen",
                       Persistence::Transient, lf.access_token_failed, true});
        out.push_back({"ae.ngl_principal_failed", "Library/Logs/NGL",
                       lf.principal_failed ? "failed" : "not seen",
                       Persistence::Transient, lf.principal_failed, true});
        out.push_back({"ae.ngl_kc_missing", "Library/Logs/NGL",
                       lf.kc_item_missing ? "absent" : "not seen",
                       Persistence::Transient, lf.kc_item_missing, true});

        // ---- 4. 機構: バイナリ内の関数の有無 (静的事実) ----
        // app の場合、AfterFXLib に aelicensing::DoSetup が存在すれば
        // 「ライセンス初期化が焼き込まれている」と言える。
        if (!t.path.empty()) {
            std::string lib = join(t.path,
                "Contents/Frameworks/AfterFXLib.framework/Versions/A/AfterFXLib");
            out.push_back({"ae.afterfxlib", "AfterFXLib.framework",
                           exists(lib) ? "present" : "absent",
                           Persistence::Durable, exists(lib), true});
            out.push_back({"ae.target_app", "app bundle",
                           exists(t.path) ? "present" : "absent",
                           Persistence::Durable, exists(t.path), false});
        }
        (void)ngl;
    }

    void analyze(const std::vector<Signal> &signals,
                 std::vector<Finding> &out) const override {
        auto val = [&](const char *id) -> std::string {
            for (const auto &s : signals)
                if (s.observed && s.id == id) return s.value;
            return "";
        };
        auto seen = [&](const char *id) {
            for (const auto &s : signals) if (s.observed && s.id == id) return true;
            return false;
        };

        // (a) トークンが取れずサインインを要求している
        {
            std::vector<std::string> ev;
            if (seen("ae.ngl_access_token_failed")) ev.push_back("ae.ngl_access_token_failed");
            if (seen("ae.ngl_principal_failed")) ev.push_back("ae.ngl_principal_failed");
            if (seen("ae.ngl_kc_missing")) ev.push_back("ae.ngl_kc_missing");
            if (!ev.empty())
                out.push_back({"ae.token-unavailable",
                               "NGL がアクセストークンを取得できず、サインインを要求している",
                               ev, 0.95, true});
        }

        // (b) サインインを要求された結果として終了する (戻り値を使わない構造)
        {
            bool token = false;
            for (const auto &f : out)
                if (f.id == "ae.token-unavailable") token = true;
            if (token && seen("ae.afterfxlib"))
                out.push_back({"ae.exit-on-signin",
                               "ライセンス初期化が起動列に焼き込まれており、失敗が終了に直結する",
                               {"ae.afterfxlib", "ae.ngl_access_token_failed"}, 0.9, true});
        }

        // (c) 状態ファイルは揃っているのに直らない
        {
            bool markers_ok = val("ae.status_notification") == "IMS_ACTIVE" &&
                              val("ae.lic_notification") == "SIGNIN" &&
                              val("ae.ngl_v2") == "SIGNIN";
            bool token = false;
            for (const auto &f : out)
                if (f.id == "ae.token-unavailable") token = true;
            if (markers_ok && token)
                out.push_back({"ae.markers-insufficient",
                               "状態ファイルは揃っているが直らない (トークンはファイルではない)",
                               {"ae.status_notification", "ae.lic_notification", "ae.ngl_v2"},
                               0.85, true});
        }
    }

    void plan(const std::vector<Finding> &findings,
              std::vector<Step> &out) const override {
        bool token = false, exit_on = false, markers = false;
        for (const auto &f : findings) {
            if (f.id == "ae.token-unavailable") token = true;
            if (f.id == "ae.exit-on-signin") exit_on = true;
            if (f.id == "ae.markers-insufficient") markers = true;
        }

        // 順序の理由:
        //   1) まず「状態ファイルで直らない」ことを確定させる (現状の観測)
        //   2) 次にライセンス初期化を起動列から外す (大枠)
        //   3) そのうえで、初期化に依存する関数を定数返却に置き換える
        //   2 を先にしないと 3 の副作用評価ができない。
        if (markers) {
            Step s;
            s.id = "ae.confirm-markers";
            s.action = "状態ファイルが揃っていても直らないことを記録する (これ以上のファイル操作は無効)";
            s.mutates = false;
            s.cost = 1;
            out.push_back(s);
        }
        if (exit_on) {
            Step s;
            s.id = "ae.detach-license-setup";
            s.action = "MainMain から aelicensing::DoSetup の呼び出しを外す (大枠ごと切る)";
            s.mutates = true;
            s.approved = true;
            s.cost = 3;
            if (markers) s.requires.push_back("ae.confirm-markers");
            out.push_back(s);

            Step d;
            d.id = "ae.detach-dependent-fns";
            d.action = "初期化に依存する関数を定数返却へ置換 (必要変数 s_calledB だけ残す)";
            d.requires = {"ae.detach-license-setup"};
            d.mutates = true;
            d.approved = true;
            d.cost = 4;
            out.push_back(d);

            Step r;
            r.id = "ae.resign";
            r.action = "書き換えたバンドルを ad-hoc で再署名して検証する";
            r.requires = {"ae.detach-dependent-fns"};
            r.mutates = true;
            r.approved = true;
            r.cost = 1;
            out.push_back(r);

            // 代替: 大枠を切らずに NGL 構築だけ外す (副作用を抑えた版)
            Step alt;
            alt.id = "ae.detach-ngl-only";
            alt.action = "代替: NglController 構築だけを外す (ライセンス層は残す)";
            alt.mutates = true;
            alt.approved = true;
            alt.cost = 2;
            out.push_back(alt);

            for (auto &s : out) {
                if (s.id == "ae.detach-license-setup") s.alternatives = {"ae.detach-ngl-only"};
                if (s.id == "ae.detach-ngl-only") s.alternatives = {"ae.detach-license-setup"};
            }
        }
        if (token && !exit_on) {
            Step s;
            s.id = "ae.report-only";
            s.action = "トークンはファイルで作れないため、修復は実行せず報告に留める";
            s.mutates = false;
            s.cost = 1;
            out.push_back(s);
        }
    }
};

void register_builtin_cases() {
    static bool done = false;
    if (done) return;
    done = true;
    CaseRegistry::instance().add(std::make_shared<AeLicenseCase>());
}

} // namespace nx
