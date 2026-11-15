// nx_case.cpp — 案件の登録簿と、組み込みテンプレートの登録
//
// 方針:
//   案件は「名前」で管理しない。テンプレートのリストとして並べ、
//   対象に合うものを登録順に試す (最初に applicable を返したものが動く)。
//   識別子が要るところは、その場の仮番号 (tpl[i]) だけで足りる。

#include "nx_case.hpp"
#include "nx_recipe.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace nx {

// ============================================================ 登録簿

CaseRegistry &CaseRegistry::instance() {
    static CaseRegistry r;
    return r;
}

void CaseRegistry::add(std::shared_ptr<Case> c) {
    if (!c) return;
    cases_.push_back(c);   // 名前で置き換えない。リストに積むだけ。
}

std::vector<std::shared_ptr<Case>> CaseRegistry::match(const CaseTarget &t) const {
    std::vector<std::shared_ptr<Case>> out;
    for (const auto &c : cases_) {
        if (!c) continue;
        if (c->applicable(t)) out.push_back(c);   // 登録順を保つ (逐次に試す)
    }
    return out;
}

CaseRegistry::CaseRegistry() = default;
CaseRegistry::~CaseRegistry() = default;

int CaseRegistry::index_of(const std::shared_ptr<Case> &c) const {
    for (size_t i = 0; i < cases_.size(); i++)
        if (cases_[i] == c) return (int)i;
    return -1;
}

// ============================================================ 共通の小道具

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

/* team 付きの共有コンテナを 1 つ探す (名前は呼び出し側が与える)。 */
std::string find_container(const char *marker) {
    std::string root = join(home(), "Library/Group Containers");
    DIR *d = opendir(root.c_str());
    if (!d) return "";
    std::string found;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n.find(marker) != std::string::npos) { found = join(root, n); break; }
    }
    closedir(d);
    return found;
}

// サインインを要求した根拠を、そのログ自身が書いた行だけから拾う。
struct LogFacts {
    bool access_token_failed = false;
    bool principal_failed = false;
    bool kc_item_missing = false;
    int  lines_scanned = 0;
};

LogFacts scan_log_dir(const std::string &dir) {
    LogFacts r;
    DIR *d = opendir(dir.c_str());
    if (!d) return r;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string p = join(dir, e->d_name);
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
    closedir(d);
    return r;
}

} // namespace

// ============================================================ テンプレート 0
// ライセンス起因で「起動 → サインイン要求 → 終了」になる案件。
//
// 静的に確認できる依存の鎖:
//   MainMain                          (戻り値を使わない)
//     -> aelicensing::DoSetup         (大枠。ここを外す)
//          -> licensingglue::LicenseSetup
//               -> NglController 構築
//                    -> NglService::RequestAccessToken   <- サインイン要求
//
// 観測は 3 層:
//   1. 痕跡  (状態ファイル / ログ)      … 事実
//   2. 機構  (バイナリ内の関数の有無)   … 事実
//   3. 大元  (コード経路で起こす外部)   … 静的追跡の事実
// 判定は 1-3 を突き合わせ、根拠の signal id を必ず添える。
namespace {

class LicenseTemplate : public Case {
public:
    const char *name() const override { return "license"; }

    bool applicable(const CaseTarget &t) const override {
        if (t.kind != "app" && t.kind != "state" && t.kind != "dir") return false;
        if (!find_container("com.adobe.NGL").empty()) return true;
        return exists(join(home(), "Library/Application Support/Adobe"));
    }

    void collect(const CaseTarget &t, std::vector<Signal> &out) const override {
        const std::string adobe = join(home(), "Library/Application Support/Adobe");
        const std::string ngl = find_container("com.adobe.NGL");

        // ---- 1. 痕跡 ----
        std::string st, lic;
        bool have_st = read_small(join(adobe, ".adobestatusnotification"), st);
        bool have_lic = read_small(join(adobe, ".adobelicnotification"), lic);
        st = trim(st); lic = trim(lic);
        out.push_back({"license.status_notification", "Adobe/.adobestatusnotification",
                       have_st ? st : "(none)", Persistence::Durable, have_st, true});
        out.push_back({"license.lic_notification", "Adobe/.adobelicnotification",
                       have_lic ? lic : "(none)", Persistence::Durable, have_lic, true});

        // ---- 2. 共有コンテナのマーカー ----
        std::string v2;
        bool have_v2 = false;
        if (!ngl.empty()) {
            have_v2 = read_small(join(ngl, "licflags/.adobelicnotificationV2"), v2);
            v2 = trim(v2);
        }
        out.push_back({"license.ngl_v2", "NGL/licflags/.adobelicnotificationV2",
                       have_v2 ? v2 : "(none)", Persistence::Durable, have_v2, true});

        // ---- 3. ログ: サインインを要求した直接の根拠 ----
        LogFacts lf = scan_log_dir(join(home(), "Library/Logs/NGL"));
        out.push_back({"license.access_token_failed", "Library/Logs/NGL",
                       lf.access_token_failed ? "failed" : "not seen",
                       Persistence::Transient, lf.access_token_failed, true});
        out.push_back({"license.principal_failed", "Library/Logs/NGL",
                       lf.principal_failed ? "failed" : "not seen",
                       Persistence::Transient, lf.principal_failed, true});
        out.push_back({"license.kc_missing", "Library/Logs/NGL",
                       lf.kc_item_missing ? "absent" : "not seen",
                       Persistence::Transient, lf.kc_item_missing, true});

        // ---- 4. 機構 ----
        if (!t.path.empty()) {
            std::string lib = join(t.path,
                "Contents/Frameworks/AfterFXLib.framework/Versions/A/AfterFXLib");
            out.push_back({"license.lib", "license framework",
                           exists(lib) ? "present" : "absent",
                           Persistence::Durable, exists(lib), true});
            out.push_back({"license.target_app", "app bundle",
                           exists(t.path) ? "present" : "absent",
                           Persistence::Durable, exists(t.path), false});

            // ---- 5. 大元: コード経路で外部を起こしている (静的追跡) ----
            //   AdobeGCInvokeClient が agcinvokerutility を読み、
            //   licensingglue::InvokeGoCart がそれを起こす。
            if (exists(lib)) {
                nx_site_t gc{}, root{};
                bool have_gc =
                    nx_recipe_locate_symbol(lib.c_str(), "AdobeGCInvokeClient", &gc) == 0;
                bool have_root = nx_recipe_locate_symbol(
                    lib.c_str(), "licensingglue12_GLOBAL__N_112InvokeGoCart", &root) == 0;
                char gcv[32] = "(none)", rv[32] = "(none)", scv[32] = "(none)";
                if (have_gc) snprintf(gcv, sizeof gcv, "0x%llx", gc.vaddr);
                if (have_root) snprintf(rv, sizeof rv, "0x%llx", root.vaddr);
                nx_site_t sc{};
                bool have_sc =
                    nx_recipe_locate_symbol(lib.c_str(), "ShouldCheckLicense", &sc) == 0;
                if (have_sc) snprintf(scv, sizeof scv, "0x%llx", sc.vaddr);
                out.push_back({"license.should_check", "ShouldCheckLicense",
                               scv, Persistence::Durable, have_sc, true});
                out.push_back({"license.gc_invoke_client", "AdobeGCInvokeClient",
                               gcv, Persistence::Durable, have_gc, true});
                out.push_back({"license.gc_spawn_root", "InvokeGoCart",
                               rv, Persistence::Durable, have_root, true});
            }
        }
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
            if (seen("license.access_token_failed")) ev.push_back("license.access_token_failed");
            if (seen("license.principal_failed")) ev.push_back("license.principal_failed");
            if (seen("license.kc_missing")) ev.push_back("license.kc_missing");
            if (!ev.empty())
                out.push_back({"license.token-unavailable",
                               "トークンを取得できず、サインインを要求している",
                               ev, 0.95, true});
        }

        // (b) サインイン要求が終了に直結する (戻り値を使わない構造)
        {
            bool token = false;
            for (const auto &f : out)
                if (f.id == "license.token-unavailable") token = true;
            if (token && seen("license.lib"))
                out.push_back({"license.exit-on-signin",
                               "ライセンス初期化が起動列に焼き込まれており、失敗が終了に直結する",
                               {"license.lib", "license.access_token_failed"}, 0.9, true});
        }

        // (c) 状態ファイルは揃っているのに直らない
        {
            bool markers_ok = val("license.status_notification") == "IMS_ACTIVE" &&
                              val("license.lic_notification") == "SIGNIN" &&
                              val("license.ngl_v2") == "SIGNIN";
            bool token = false;
            for (const auto &f : out)
                if (f.id == "license.token-unavailable") token = true;
            if (markers_ok && token)
                out.push_back({"license.markers-insufficient",
                               "状態ファイルは揃っているが直らない (トークンはファイルではない)",
                               {"license.status_notification", "license.lic_notification",
                                "license.ngl_v2"}, 0.85, true});
        }

        // (d) 外部の大元をコード経路で起こしている
        {
            std::vector<std::string> ev;
            if (seen("license.gc_invoke_client")) ev.push_back("license.gc_invoke_client");
            if (seen("license.gc_spawn_root")) ev.push_back("license.gc_spawn_root");
            if (!ev.empty())
                out.push_back({"license.root-spawns",
                               "コード経路で agcinvokerutility を起こしている (静的追跡)",
                               ev, 0.9, true});
        }

        // (e) サインイン要求の入口 (ライセンス確認) がコアに焼き込まれている
        if (seen("license.should_check"))
            out.push_back({"license.should-check-gate",
                           "ShouldCheckLicense がサインイン要求の入口になっている",
                           {"license.should_check"}, 0.9, true});
    }

    void plan(const std::vector<Finding> &findings,
              std::vector<Step> &out) const override {
        bool token = false, exit_on = false, markers = false, root = false, gate = false;
        for (const auto &f : findings) {
            if (f.id == "license.token-unavailable") token = true;
            if (f.id == "license.exit-on-signin") exit_on = true;
            if (f.id == "license.markers-insufficient") markers = true;
            if (f.id == "license.root-spawns") root = true;
            if (f.id == "license.should-check-gate") gate = true;
        }

        // 順序の理由:
        //   1) まず「状態ファイルで直らない」ことを確定させる
        //   2) 次にライセンス初期化を起動列から外す (大枠)
        //   3) そのうえで、初期化に依存する関数を定数返却に置き換える
        //   2 を先にしないと 3 の副作用評価ができない。
        if (markers) {
            Step s;
            s.id = "confirm-markers";
            s.action = "状態ファイルが揃っていても直らないことを記録する (これ以上のファイル操作は無効)";
            s.mutates = false;
            s.cost = 1;
            out.push_back(s);
        }
        if (exit_on) {
            Step s;
            s.id = "detach-license-setup";
            s.action = "起動列からライセンス初期化 (DoSetup) の呼び出しを外す (大枠ごと切る)";
            s.mutates = true;
            s.approved = true;
            s.cost = 3;
            if (markers) s.requires.push_back("confirm-markers");
            out.push_back(s);

            Step d;
            d.id = "detach-dependent-fns";
            d.action = "初期化に依存する関数を定数返却へ置換 (必要変数だけ残す)";
            d.requires = {"detach-license-setup"};
            d.mutates = true;
            d.approved = true;
            d.cost = 4;
            out.push_back(d);

            Step r;
            r.id = "resign";
            r.action = "書き換えたバンドルを ad-hoc で再署名して検証する";
            r.requires = {"detach-dependent-fns"};
            r.mutates = true;
            r.approved = true;
            r.cost = 1;
            out.push_back(r);

            // 代替: 大枠を切らずに NGL 構築だけ外す (副作用を抑えた版)
            Step alt;
            alt.id = "detach-ngl-only";
            alt.action = "代替: NglController 構築だけを外す (ライセンス層は残す)";
            alt.mutates = true;
            alt.approved = true;
            alt.cost = 2;
            out.push_back(alt);

            for (auto &s2 : out) {
                if (s2.id == "detach-license-setup") s2.alternatives = {"detach-ngl-only"};
                if (s2.id == "detach-ngl-only") s2.alternatives = {"detach-license-setup"};
            }
        }
        if (token && !exit_on) {
            Step s;
            s.id = "report-only";
            s.action = "トークンはファイルで作れないため、修復は実行せず報告に留める";
            s.mutates = false;
            s.cost = 1;
            out.push_back(s);
        }
        if (root) {
            // 静的に確認した根 (AdobeGCInvokeClient ← InvokeGoCart) を切る。
            Step s;
            s.id = "cut-gc-invoker";
            s.action = "AdobeGCInvokeClient を ret に潰し、agcinvokerutility の起動を止める";
            s.mutates = true;
            s.approved = true;
            s.cost = 2;
            out.push_back(s);

            Step a;
            a.id = "cut-gocart";
            a.action = "代替: InvokeGoCart を ret に潰す (呼び出し元ごと止める)";
            a.mutates = true;
            a.approved = true;
            a.cost = 3;
            out.push_back(a);

            for (auto &st : out) {
                if (st.id == "cut-gc-invoker") st.alternatives = {"cut-gocart"};
                if (st.id == "cut-gocart") st.alternatives = {"cut-gc-invoker"};
            }
        }
        if (gate) {
            // サインイン要求の入口そのものを外す (最優先・最小侵襲)。
            Step s;
            s.id = "cut-should-check";
            s.action = "ShouldCheckLicense を mov w0,#0; ret に潰し、"
                       "サインイン要求と終了の入口を外す";
            s.mutates = true;
            s.approved = true;
            s.cost = 1;
            out.push_back(s);
        }
    }
};

// ============================================================ 登録
// ここがテンプレートのリスト。名前で引かない。上から順に試す。
std::vector<std::shared_ptr<Case>> &templates() {
    static std::vector<std::shared_ptr<Case>> t = {
        std::make_shared<LicenseTemplate>(),
    };
    return t;
}

} // namespace

void register_builtin_cases() {
    static bool done = false;
    if (done) return;
    done = true;
    for (auto &c : templates()) CaseRegistry::instance().add(c);
}

} // namespace nx
