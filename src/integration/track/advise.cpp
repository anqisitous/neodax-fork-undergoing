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

// ------------------------------------------------------------------
// TabModel
// ------------------------------------------------------------------

TabModel::TabModel(std::string state_json, std::string source)
    : json_(std::move(state_json)), source_(std::move(source)) {
    char ts[32] = {0};
    std::time_t now = std::time(NULL);
    std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    observed_at_ = ts;
}

namespace {

// 最小限の JSON フィールド取り出し。オブジェクト境界を自前で括るので、
// 外部パーサに依存しない (この助言器は単体でリンクできる必要がある)。
std::string field(const std::string &o, const std::string &key) {
    std::string pat = "\"" + key + "\"";
    size_t p = o.find(pat);
    if (p == std::string::npos) return {};
    p = o.find(':', p + pat.size());
    if (p == std::string::npos) return {};
    p++;
    while (p < o.size() && std::isspace((unsigned char)o[p])) p++;
    if (p >= o.size() || o[p] != '"') return {};
    p++;
    std::string out;
    bool esc = false;
    for (; p < o.size(); p++) {
        char c = o[p];
        if (esc) { out += c; esc = false; continue; }
        if (c == '\\') { esc = true; continue; }
        if (c == '"') break;
        out += c;
    }
    return out;
}

// 指定位置の '{' に対応する '}' の次を返す。
size_t obj_end(const std::string &s, size_t open) {
    int depth = 0, in_str = 0, esc = 0;
    for (size_t i = open; i < s.size(); i++) {
        char c = s[i];
        if (in_str) {
            if (esc) esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"') in_str = 0;
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == '{') depth++;
        else if (c == '}') { depth--; if (depth == 0) return i + 1; }
    }
    return s.size();
}

} // namespace

void TabModel::collect() {
    tabs_.clear();
    orphans_ = live_surfaces_ = 0;

    // 1) tabs 配列の範囲を決める
    size_t tp = json_.find("\"tabs\"");
    if (tp == std::string::npos) { build_advice(); build_capabilities(); return; }
    size_t lb = json_.find('[', tp);
    if (lb == std::string::npos) { build_advice(); build_capabilities(); return; }

    // 2) 各タブのオブジェクトを切り出す。
    //    tabs 配列の直下にある要素だけを対象にするため、
    //    配列の深さ (arr) とオブジェクトの深さ (obj) を別々に数える。
    std::vector<std::string> objs;
    int arr = 0, obj = 0, in_str = 0, esc = 0;
    size_t start = 0;
    for (size_t i = lb; i < json_.size(); i++) {
        char c = json_[i];
        if (in_str) {
            if (esc) esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"') in_str = 0;
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == '[') { arr++; continue; }
        if (c == ']') { arr--; if (arr == 0) break; continue; }
        if (c == '{') {
            if (arr == 1 && obj == 0) start = i;   /* 配列直下の要素の開始 */
            obj++;
            continue;
        }
        if (c == '}') {
            obj--;
            if (arr == 1 && obj == 0 && start) {
                objs.push_back(json_.substr(start, i - start + 1));
                start = 0;
            }
            continue;
        }
    }

    // 3) 生存スレッド (chat タブ) を集める
    std::vector<std::pair<std::string, std::string>> live;
    for (const auto &o : objs) {
        if (field(o, "kind") != "chat") continue;
        size_t tr = o.find("\"threadRef\"");
        if (tr == std::string::npos) continue;
        std::string tref = o.substr(tr, obj_end(o, o.find('{', tr)) - tr);
        live.emplace_back(field(tref, "environmentId"), field(tref, "threadId"));
    }

    // 4) 分類
    for (const auto &o : objs) {
        TabFact f;
        f.id = field(o, "id");
        f.surface = field(o, "surfaceId");
        std::string kind = field(o, "kind");
        std::string env, th;
        size_t tr = o.find("\"threadRef\"");
        if (tr != std::string::npos) {
            std::string tref = o.substr(tr, obj_end(o, o.find('{', tr)) - tr);
            env = field(tref, "environmentId");
            th = field(tref, "threadId");
        }
        f.owner = env + "/" + th;

        if (kind == "chat") {
            f.kind = TabKind::Chat;
        } else if (kind == "surface") {
            bool alive = false;
            for (const auto &l : live)
                if (l.first == env && l.second == th) { alive = true; break; }
            f.kind = alive ? TabKind::SurfaceLive : TabKind::SurfaceOrphan;
            if (alive) live_surfaces_++; else orphans_++;
        } else {
            f.kind = TabKind::Unknown;
        }
        tabs_.push_back(f);
    }
    build_advice();
    build_capabilities();
}

Verdict TabModel::evaluate() const {
    if (orphans_ > 0) return Verdict::Yes;      // 閉じられないタブがある
    return Verdict::No;
}

void TabModel::build_advice() {
    advice_.summary.clear();
    advice_.steps.clear();
    if (orphans_ > 0) {
        advice_.summary = "閉じられない surface タブが " + std::to_string(orphans_) +
                          " 件。所有者スレッドがタブ帯に存在しない (削除は妥当)。";
        advice_.steps = {
            "これらのタブは×では閉じられない (親スレッドが無いため解決できない)。",
            "`delete --id <ID> --live` で永続層と稼働中ストアを同時に更新する。",
            "削除後に `tabs` を再読して、本当に消えたかを確かめる。",
        };
    } else if (live_surfaces_ > 0) {
        advice_.summary = "surface タブは " + std::to_string(live_surfaces_) +
                          " 件あるが、すべて所有者スレッドが生存。壊れていない。";
        advice_.steps = {
            "これらは普通に×で閉じられる。削除は不要。",
            "閉じられないように見える場合、原因は所有者ではなく別の層 (メモリ/描画) の可能性。",
            "勝手に削除しない。まず観測を増やす。",
        };
    } else {
        advice_.summary = "surface タブは無い。削除対象は無い。";
        advice_.steps = { "状態は健全。何もしない。" };
    }
}

void TabModel::build_capabilities() {
    caps_ = {
        {"Read workspace-tabs state",   "file-read", false, true},
        {"Classify tabs (live/orphan)",  "in-process", false, true},
        {"Delete tab from LevelDB",     "file-write (app state)", true, false},
        {"Sync live renderer store",    "inspector + JS", true, false},
    };
}

bool TabModel::removable(const std::string &id, std::string &why) const {
    for (const auto &t : tabs_) {
        if (t.id != id) continue;
        switch (t.kind) {
            case TabKind::SurfaceOrphan:
                why = "owner thread is gone (cannot be closed from the UI)";
                return true;
            case TabKind::SurfaceLive:
                why = "owner thread is alive; the app can close this itself";
                return false;
            case TabKind::Chat:
                why = "chat tab (never deleted automatically)";
                return false;
            default:
                why = "unknown kind";
                return false;
        }
    }
    why = "id not present in state";
    return false;
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

/*
 * タブ状態のアドバイザ (C エントリ)。
 *   state_json: workspace-tabs の JSON (NULL なら何もしない)
 *   check_id:   指定すると、その id を削除してよいかを判定して出力する
 * 戻り値: 0 = 健全 (削除不要), 1 = 閉じられないタブがある, 2 = 判定不能
 */
extern "C" int nx_advise_tabs(const char *source, const char *state_json,
                              const char *check_id, FILE *out) {
    if (!out) out = stdout;
    if (!state_json) {
        std::fprintf(out, "  [advise] no state to evaluate\n");
        return 2;
    }
    nx::TabModel m(state_json ? state_json : "", source ? source : "");
    m.collect();
    nx::Verdict v = m.evaluate();

    std::fprintf(out, "=== NeoDAX advisory (workspace tabs) ===\n");
    std::fprintf(out, "observed_at=%s\n", m.observed_at().c_str());
    std::fprintf(out, "source=%s\n", m.source().c_str());
    for (const auto &t : m.tabs()) {
        const char *k = t.kind == nx::TabKind::Chat           ? "chat"
                        : t.kind == nx::TabKind::SurfaceLive  ? "surface(live)"
                        : t.kind == nx::TabKind::SurfaceOrphan ? "surface(orphan)"
                                                              : "unknown";
        std::fprintf(out, "  %-17s %-28s owner=%s\n", k,
                     t.surface.empty() ? "(chat)" : t.surface.c_str(),
                     t.owner.c_str());
    }
    std::fprintf(out, "VERDICT=%s\n",
                 v == nx::Verdict::Yes ? "YES" : v == nx::Verdict::No ? "NO" : "UNKNOWN");
    std::fprintf(out, "  %s\n", m.advice().summary.c_str());
    for (const auto &s : m.advice().steps) std::fprintf(out, "  - %s\n", s.c_str());

    if (check_id && *check_id) {
        std::string why;
        bool ok = m.removable(check_id, why);
        std::fprintf(out, "ID=%s removable=%s (%s)\n", check_id,
                     ok ? "yes" : "no", why.c_str());
        if (!ok) return 1;
    }
    return v == nx::Verdict::Yes ? 1 : v == nx::Verdict::No ? 0 : 2;
}

/* 1 つの id を削除してよいかだけを問う (実行はしない)。 */
extern "C" int nx_advise_tab_can_delete(const char *state_json, const char *id,
                                        char *why_out, size_t why_cap) {
    if (!state_json || !id) return 0;
    nx::TabModel m(state_json, "");
    m.collect();
    std::string why;
    bool ok = m.removable(id, why);
    if (why_out && why_cap) {
        std::snprintf(why_out, why_cap, "%s", why.c_str());
    }
    return ok ? 1 : 0;
}
