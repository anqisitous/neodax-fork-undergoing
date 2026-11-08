// advise.cpp — 生成的アプリ解析 / IT アドバイザの実装

#include "advise.hpp"

#include <cctype>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include <dirent.h>
#include <sys/stat.h>

namespace nx {
namespace {

std::string join(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool read_small(const std::string &p, std::string &out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

bool exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

std::string find_suffix_in(const std::string &dir, const std::string &suffix) {
    DIR *d = opendir(dir.c_str());
    if (!d) return {};
    std::string found;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() >= suffix.size() &&
            n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) {
            found = join(dir, n);
            break;
        }
    }
    closedir(d);
    return found;
}

std::string clip(const std::string &s, size_t n) {
    if (s.size() <= n) return s;
    return s.substr(0, n) + "…";
}

std::string xml(const std::string &s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default: o += c;
        }
    }
    return o;
}

const char *verdict_name(Verdict v) {
    switch (v) {
        case Verdict::Yes: return "YES";
        case Verdict::No: return "NO";
        default: return "UNKNOWN";
    }
}

const char *verdict_color(Verdict v) {
    switch (v) {
        case Verdict::Yes: return "#f87171";
        case Verdict::No: return "#4ade80";
        default: return "#fbbf24";
    }
}

} // namespace

LoginModel::LoginModel(std::string home) : home_(std::move(home)) {
    char ts[32] = {0};
    std::time_t now = std::time(NULL);
    std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    observed_at_ = ts;
}

void LoginModel::collect() {
    signals_.clear();
    const std::string adobe = join(home_, "Library/Application Support/Adobe");
    const std::string oobe = join(adobe, "OOBE");

    std::string prefs;
    bool have_prefs = read_small(join(oobe, "com.adobe.acc.container.default.prefs"), prefs);
    logged_out_ = have_prefs && prefs.find("LoggedOutModeEnabled\">true") != std::string::npos;
    logged_in_flag_ = have_prefs && prefs.find("LoggedOutModeEnabled\">false") != std::string::npos;
    signals_.push_back({"OOBE.LoggedOutModeEnabled",
                        "OOBE/com.adobe.acc.container.default.prefs",
                        !have_prefs ? "(無し)" : logged_out_ ? "true" : logged_in_flag_ ? "false" : "(不明)",
                        Persistence::Durable, have_prefs, true});

    std::string st, lic;
    bool have_st = read_small(join(adobe, ".adobestatusnotification"), st);
    bool have_lic = read_small(join(adobe, ".adobelicnotification"), lic);
    st = trim(st); lic = trim(lic);
    signals_.push_back({".adobestatusnotification", "Adobe/.adobestatusnotification",
                        have_st ? st : "(無し)", Persistence::Durable, have_st, true});
    signals_.push_back({".adobelicnotification", "Adobe/.adobelicnotification",
                        have_lic ? lic : "(無し)", Persistence::Durable, have_lic, true});

    std::string gc = find_suffix_in(join(home_, "Library/Group Containers"), "com.adobe.NGL");
    std::string v2;
    bool have_v2 = !gc.empty() && read_small(join(gc, "licflags/.adobelicnotificationV2"), v2);
    v2 = trim(v2);
    signals_.push_back({"licflags/.adobelicnotificationV2",
                        "Group Containers/<TEAM>.com.adobe.NGL/licflags",
                        have_v2 ? v2 : "(無し)", Persistence::Durable, have_v2, true});

    // これは on-demand に作り直されるので判定に使わない (表示だけ)。
    signals_.push_back({"NGL.container", "Group Containers/<TEAM>.com.adobe.NGL",
                        exists(gc) ? "present" : "absent now",
                        Persistence::Transient, !gc.empty(), false});

    sso_ = (st == "IMS_ACTIVE") && (lic == "SIGNIN") && (v2 == "SIGNIN");
    build_advice();
    build_capabilities();
}

Verdict LoginModel::evaluate() const {
    if (logged_out_) return Verdict::Yes;
    if (logged_in_flag_ && sso_) return Verdict::No;
    return Verdict::Unknown;
}

void LoginModel::build_advice() {
    advice_.summary.clear();
    advice_.steps.clear();
    switch (evaluate()) {
        case Verdict::Yes:
            advice_.summary = "ログイン/ライセンス UI が起動時に出る可能性が高い (持続マーカーが logged-out)。";
            advice_.steps = {
                "AE を一度サインインさせるか、サインイン済みの状態を復元する。",
                "OOBE LoggedOutModeEnabled=false と SSO/V2 の 3 つを揃える。",
                "NGL コンテナの有無ではなく、この持続マーカーで再判定する。",
            };
            break;
        case Verdict::No:
            advice_.summary = "持続マーカー上はサインイン済み。ログイン UI は予測されない。";
            advice_.steps = {
                "ディスク容量だけ別途確認する (Disk Cache 警告は独立)。",
                "起動時に AXWebArea が出ても、それは NGL UI ではない可能性を切り分ける。",
            };
            break;
        default:
            advice_.summary = "判定不能。持続マーカーが無い (NGL の現在の不在は根拠にならない)。";
            advice_.steps = {
                "Keychain の IMS トークン、または OOBE のログイン状態を観測する。",
                "NGL は起動時に生成されるため、コンテナの現在の有無を判定に使わない。",
                "observed_at 付きのスナップショットとして扱い、未来を断定しない。",
            };
            break;
    }
}

void LoginModel::build_capabilities() {
    // 承認ダイアログを出さずに進められるのは、既に許可済みの OS 権限だけ。
    // requires_approval=true の行は「実行しない」— 計画として表示するだけ。
    caps_ = {
        {"Read OOBE / SSO durable signals", "file-read", false, true},
        {"Read NGL container metadata",      "file-read", false, true},
        {"Render advisory SVG",             "file-write (own artifact)", false, true},
        {"Read Keychain IMS token",         "keychain", true, false},
        {"Launch AE and read AX tree",      "AX + process launch", true, false},
        {"Rewrite Adobe auth state",        "file-write (Adobe state)", true, false},
        {"Request TCC permission",          "TCC prompt", true, false},
    };
}

bool AdvisorySvg::write(const LoginModel &m, const std::string &path, std::string &err) {
    std::ofstream f(path);
    if (!f) { err = "書けません: " + path; return false; }

    Verdict v = m.evaluate();
    const int W = 1200, H = 1020;
    f << "<?xml version='1.0' encoding='UTF-8'?>\n";
    f << "<svg xmlns='http://www.w3.org/2000/svg' width='" << W << "' height='" << H
      << "' viewBox='0 0 " << W << " " << H << "'>\n";
    f << "<rect width='100%' height='100%' fill='#0f1319'/>\n";
    f << "<style>"
         "text{font-family:Menlo,monospace;font-size:12px;fill:#c9d4e3}"
         ".h{font-size:20px;font-weight:bold}"
         ".s{font-size:12px;fill:#7d8da3}"
         ".k{font-size:12px;fill:#93a4bb}"
         ".v{font-size:13px;font-weight:bold}"
         ".box{fill:#171d26;stroke:#2a3444;stroke-width:1}"
         "</style>\n";

    // ヘッダ
    f << "<text class='h' x='36' y='52'>NeoDAX advisory — After Effects launch / login</text>\n";
    f << "<text class='s' x='36' y='74'>observed_at=" << xml(m.observed_at())
      << "   snapshot, not a live probe</text>\n";

    // Verdict バッジ
    f << "<rect x='840' y='30' width='224' height='58' rx='10' class='box' stroke='"
      << verdict_color(v) << "' stroke-width='2'/>\n";
    f << "<text class='s' x='856' y='52'>VERDICT</text>\n";
    f << "<text x='856' y='76' font-size='24' font-weight='bold' fill='" << verdict_color(v)
      << "'>" << verdict_name(v) << "</text>\n";

    // Signal 表
    int y = 120;
    f << "<text class='k' x='36' y='" << y << "'>SIGNALS (durable のみ判定 / transient は表示のみ)</text>\n";
    y += 14;
    f << "<rect class='box' x='36' y='" << y << "' width='1128' height='24'/>\n";
    f << "<text class='s' x='48' y='" << y + 16 << "'>signal</text>\n";
    f << "<text class='s' x='360' y='" << y + 16 << "'>persistence</text>\n";
    f << "<text class='s' x='500' y='" << y + 16 << "'>value</text>\n";
    f << "<text class='s' x='680' y='" << y + 16 << "'>source</text>\n";
    y += 24;
    for (const auto &s : m.signals()) {
        bool used = s.persistence == Persistence::Durable && s.informs_verdict;
        f << "<rect x='36' y='" << y << "' width='1128' height='30' fill='#131922'/>\n";
        f << "<text x='48' y='" << y + 20 << "' fill='" << (used ? "#c9d4e3" : "#65758b")
          << "'>" << xml(s.id) << "</text>\n";
        f << "<text x='330' y='" << y + 20 << "' fill='"
          << (s.persistence == Persistence::Durable ? "#60a5fa" : "#7d8da3") << "'>"
          << (s.persistence == Persistence::Durable ? "durable" : "transient") << "</text>\n";
        f << "<text class='v' x='500' y='" << y + 20 << "' fill='"
          << (s.observed ? "#e6edf7" : "#ff9f9f") << "'>" << xml(clip(s.value, 22)) << "</text>\n";
        f << "<text class='s' x='680' y='" << y + 20 << "'>" << xml(clip(s.source, 62)) << "</text>\n";
        y += 30;
    }

    // Truth table
    y += 28;
    f << "<text class='k' x='36' y='" << y << "'>DECISION MODEL</text>\n";
    y += 16;
    f << "<rect class='box' x='36' y='" << y << "' width='560' height='84'/>\n";
    f << "<text x='50' y='" << y + 24 << "'>LoggedOutMode=true                       -> YES</text>\n";
    f << "<text x='50' y='" << y + 46 << "'>LoggedOutMode=false &amp;&amp; SSO/V2 all SIGNIN -> NO</text>\n";
    f << "<text x='50' y='" << y + 68 << "'>otherwise                                -> UNKNOWN</text>\n";

    // Evidence chain
    f << "<text class='k' x='610' y='" << y - 16 << "'>BACK-TRACK EVIDENCE</text>\n";
    f << "<rect class='box' x='610' y='" << y << "' width='554' height='84'/>\n";
    f << "<text class='s' x='624' y='" << y + 22 << "'>NGLWMessageManager::_getSUSIURL 0x14874</text>\n";
    f << "<text class='s' x='624' y='" << y + 40 << "'> -> SignInWithoutSpinner 0x2bcb5c</text>\n";
    f << "<text class='s' x='624' y='" << y + 58 << "'>aelicensing::HaveUser 0x5eaf74</text>\n";
    f << "<text class='s' x='624' y='" << y + 76 << "'> -> GetIMSUserID 0x5eae24</text>\n";

    // Advice
    y += 120;
    f << "<text class='k' x='36' y='" << y << "'>ADVICE</text>\n";
    y += 14;
    f << "<rect class='box' x='36' y='" << y << "' width='1128' height='"
      << (40 + (int)m.advice().steps.size() * 22) << "'/>\n";
    f << "<text x='52' y='" << y + 26 << "' fill='" << verdict_color(v) << "'>"
      << xml(m.advice().summary) << "</text>\n";
    int yy = y + 50;
    for (const auto &s : m.advice().steps) {
        f << "<text class='s' x='52' y='" << yy << "'>- " << xml(s) << "</text>\n";
        yy += 22;
    }

    // Promote / develop: 承認なしで進められる範囲と、OS 権限の対応。
    y = yy + 40;
    f << "<text class='k' x='36' y='" << y << "'>PROMOTE / DEVELOP (approval-free)</text>\n";
    y += 14;
    f << "<rect class='box' x='36' y='" << y << "' width='1128' height='24'/>\n";
    f << "<text class='s' x='48' y='" << y + 16 << "'>action</text>\n";
    f << "<text class='s' x='420' y='" << y + 16 << "'>OS permission</text>\n";
    f << "<text class='s' x='720' y='" << y + 16 << "'>approval</text>\n";
    f << "<text class='s' x='900' y='" << y + 16 << "'>status</text>\n";
    y += 24;
    for (const auto &c : m.capabilities()) {
        f << "<rect x='36' y='" << y << "' width='1128' height='28' fill='#131922'/>\n";
        f << "<text x='48' y='" << y + 19 << "'>" << xml(clip(c.action, 48)) << "</text>\n";
        f << "<text class='s' x='420' y='" << y + 19 << "'>" << xml(clip(c.permission, 38)) << "</text>\n";
        f << "<text class='s' x='720' y='" << y + 19 << "' fill='"
          << (c.requires_approval ? "#fbbf24" : "#4ade80") << "'>"
          << (c.requires_approval ? "required" : "none") << "</text>\n";
        f << "<text class='s' x='900' y='" << y + 19 << "' fill='"
          << (c.available ? "#4ade80" : "#f87171") << "'>"
          << (c.available ? "available" : "blocked") << "</text>\n";
        y += 28;
    }

    f << "</svg>\n";
    return true;
}

} // namespace nx

extern "C" int nx_advise_ae(const char *save_svg, FILE *out) {
    if (!out) out = stdout;
    const char *h = std::getenv("HOME");
    nx::LoginModel m(h ? h : "");
    m.collect();
    nx::Verdict v = m.evaluate();

    std::fprintf(out, "=== NeoDAX advisory (AE launch/login) ===\n");
    std::fprintf(out, "observed_at=%s\n", m.observed_at().c_str());
    for (const auto &s : m.signals())
        std::fprintf(out, "  %-34s %-9s %-14s %s\n", s.id.c_str(),
                     s.persistence == nx::Persistence::Durable ? "durable" : "transient",
                     s.observed ? "observed" : "missing", s.value.c_str());
    std::fprintf(out, "VERDICT=%s\n", v == nx::Verdict::Yes ? "YES" : v == nx::Verdict::No ? "NO" : "UNKNOWN");
    std::fprintf(out, "  %s\n", m.advice().summary.c_str());
    for (const auto &s : m.advice().steps) std::fprintf(out, "  - %s\n", s.c_str());
    std::fprintf(out, "capabilities (approval-free):\n");
    for (const auto &c : m.capabilities())
        std::fprintf(out, "  %-38s %-24s approval=%-8s %s\n", c.action.c_str(),
                     c.permission.c_str(), c.requires_approval ? "required" : "none",
                     c.available ? "available" : "blocked");

    if (save_svg && *save_svg) {
        std::string err;
        if (!nx::AdvisorySvg::write(m, save_svg, err)) {
            std::fprintf(out, "SVG: %s\n", err.c_str());
            return 1;
        }
        std::fprintf(out, "SVG: %s\n", save_svg);
    }
    return v == nx::Verdict::Yes ? 1 : v == nx::Verdict::No ? 0 : 2;
}
