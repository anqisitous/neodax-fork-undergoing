// cond.cpp — 起動条件クラスの実装

#include "cond.hpp"
#include "widget.hpp"
#include "learn.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nx {

namespace {

bool is_dir(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool is_file(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
bool can(const std::string &p, int mode) { return access(p.c_str(), mode) == 0; }

std::string read_all(const std::string &p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void walk(const std::string &dir, int depth, int max_depth, std::vector<std::string> &out) {
    if (depth > max_depth) return;
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    while (struct dirent *e = readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        std::string p = dir + "/" + e->d_name;
        if (is_dir(p)) walk(p, depth + 1, max_depth, out);
        else if (is_file(p)) out.push_back(p);
    }
    closedir(d);
}

} // namespace

Verdict FileCondition::evaluate() const {
    Verdict v;
    v.seen = path_;
    if (!is_file(path_)) { v.ok = false; v.seen += " (無し)"; return v; }
    if (read_ && !can(path_, R_OK)) { v.ok = false; v.seen += " (読めない)"; return v; }
    if (exec_ && !can(path_, X_OK)) { v.ok = false; v.seen += " (実行できない)"; return v; }
    v.ok = true;
    return v;
}

Verdict DirWritableCondition::evaluate() const {
    Verdict v;
    v.seen = dir_;
    if (!is_dir(dir_)) { v.ok = false; v.seen += " (無し)"; return v; }
    if (!can(dir_, W_OK)) { v.ok = false; v.seen += " (書けない)"; return v; }
    v.ok = true;
    return v;
}

Verdict FrameworksCondition::evaluate() const {
    Verdict v;
    std::string missing;
    for (const auto &n : need_) {
        if (!is_dir(root_ + "/" + n)) {
            if (!missing.empty()) missing += ", ";
            missing += n;
        }
    }
    if (!missing.empty()) { v.ok = false; v.seen = "欠け: " + missing; return v; }
    v.ok = true;
    v.seen = "全て有り";
    return v;
}

Verdict ArgsCondition::evaluate() const {
    Verdict v;
    for (const auto &a : args_) {
        for (const auto &p : deny_) {
            if (a.rfind(p, 0) == 0) {
                v.ok = false;
                v.seen = "付いている: " + a;
                return v;
            }
        }
    }
    v.ok = true;
    v.seen = args_.empty() ? "引数なし" : ("引数 " + std::to_string(args_.size()) + " 個");
    return v;
}

Verdict StateCondition::evaluate() const {
    Verdict v;
    std::vector<std::string> files;
    if (is_dir(root_)) walk(root_, 0, 6, files);

    int checked = 0, broken = 0;
    std::string worst;
    for (const auto &f : files) {
        bool hit = false;
        for (const auto &h : hints_)
            if (f.find(h) != std::string::npos) { hit = true; break; }
        if (!hit) continue;
        checked++;
        std::string body = read_all(f);
        // 「ワークスペース版を宣言しているのにレイアウトの実体が無い」= 壊れている形。
        // 宣言の目印に DVA_Wrkspce を使う。ビルトイン一覧のように
        // 元々レイアウトを含まないファイルを誤検出しないため。
        bool decl = body.find("DVA_Wrkspce") != std::string::npos;
        bool data = body.find("Splitter") != std::string::npos ||
                    body.find("TabPanel") != std::string::npos ||
                    body.find("WorkspaceFrame") != std::string::npos;
        if (decl && !data) { broken++; if (worst.empty()) worst = f; }
    }
    if (checked == 0) { v.ok = true; v.seen = "対象なし"; return v; }
    v.seen = "検査 " + std::to_string(checked) + " 件, 破損 " + std::to_string(broken) + " 件";
    if (broken) { v.ok = false; v.seen += " (" + worst + ")"; }
    return v;
}

bool Probe::all_ok() const {
    for (const auto *c : conds_)
        if (!c->evaluate().ok) return false;
    return true;
}

void Probe::report(FILE *out) const {
    std::fprintf(out, "起動条件を並べた結果は、次のとおりです。\n\n");
    bool all = true;
    for (const auto *c : conds_) {
        Verdict v = c->evaluate();
        std::fprintf(out, "  %s  %s  %s\n", c->id(), c->what(),
                     v.ok ? "成立しています" : "成立していません");
        if (!v.seen.empty() && !v.ok)
            std::fprintf(out, "      見た値は %s です。\n", v.seen.c_str());
        if (!v.ok) all = false;
    }
    std::fprintf(out, "\n判定は %s です。\n",
                 all ? "起動します" : "起動しません");
}

void Probe::report_drop_each(FILE *out) const {
    std::fprintf(out, "条件を 1 つずつ落としたときの起動可否は、次のとおりです。\n\n");
    for (const auto *c : conds_) {
        bool all = true;
        for (const auto *x : conds_) {
            if (x == c) continue;
            if (!x->evaluate().ok) { all = false; break; }
        }
        std::fprintf(out, "  %s  %s  %s\n", c->id(), c->what(),
                     all ? "起動します" : "起動しません");
    }
    std::fprintf(out, "\n");
}

} // namespace nx

using namespace nx;   // C 入口から短い名前で使うため

// ---------------------------------------------------------------- C 入口

// 保存されたウィジェット構成を解析し、動くかどうかを再現する。
extern "C" int nx_widget_run(const char *workspace_path, FILE *out) {
    if (!workspace_path || !*workspace_path) return 2;
    if (!out) out = stdout;

    std::ifstream in(workspace_path, std::ios::binary);
    if (!in) { std::fprintf(out, "できませんでした\n"); return 1; }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();

    Workspace ws;
    std::string err;
    if (!parse_workspace(text, ws, &err)) {
        std::fprintf(out, "できませんでした\n");
        return 1;
    }

    // 解決できる識別子を集める。拡張の置き場を一般的に探す。
    Registry reg;
    std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    const char *roots[] = {
        "/Library/Application Support/Adobe/CEP/extensions",
        "~/Library/Application Support/Adobe/CEP/extensions",
        "/Library/Application Support/Adobe/CEP/extensions.bak",
        nullptr
    };
    for (int i = 0; roots[i]; i++) {
        std::string r = roots[i];
        if (!r.empty() && r[0] == '~') r = home + r.substr(1);
        DIR *d = opendir(r.c_str());
        if (!d) continue;
        while (struct dirent *e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            reg.add("AE CEP Extension-" + std::string(e->d_name));
            reg.add(e->d_name);
        }
        closedir(d);
    }

    Simulator sim(reg);
    std::vector<Issue> issues;
    bool ok = sim.run(ws, issues);

    std::fprintf(out, "この構成に要求されているタブは、%zu 個です。\n",
                 ws.all_tabs.empty() ? 0 : ws.all_tabs[0]->tabs.size());
    for (const auto &id : ws.all_tabs[0]->tabs)
        std::fprintf(out, "  %s  %s\n", id.c_str(),
                     reg.knows(id) ? "解決できます" : "解決できません");

    // 学んだことを 1 つの脳へ。対象を特定する情報は入れない。
    SkillStore brain;
    brain.load(SkillStore::default_path());
    brain.observe("widget.tabs", std::to_string(ws.all_tabs[0]->tabs.size()) + " 個");
    brain.observe("widget.sim", ok ? "構成は動く" : "構成は動かない");
    for (const auto &is : issues)
        brain.observe("widget.issue", is.why);
    brain.save(SkillStore::default_path());

    std::fprintf(out, "\n判定は %s です。\n", ok ? "動きます" : "動きません");
    for (const auto &is : issues)
        std::fprintf(out, "  %s は %s\n", is.what.c_str(), is.why.c_str());
    return ok ? 0 : 1;
}

// 壊れた状態を退避 (*.ae-backup) から戻す。
// アプリ固有のパスは持たず、退避が置かれた階層を一般的に探す。
extern "C" int nx_cond_fix(FILE *out) {
    if (!out) out = stdout;
    std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    std::vector<std::string> files;
    walk(home + "/Library/Preferences", 0, 6, files);

    int restored = 0;
    const std::string suf = ".ae-backup";
    for (const auto &f : files) {
        if (f.size() <= suf.size()) continue;
        if (f.compare(f.size() - suf.size(), suf.size(), suf) != 0) continue;
        std::string orig = f.substr(0, f.size() - suf.size());
        std::string body = read_all(f);
        if (body.empty()) continue;
        std::ofstream o(orig, std::ios::binary | std::ios::trunc);
        if (!o) continue;
        o.write(body.data(), (std::streamsize)body.size());
        if (!o) continue;
        std::fprintf(out, "戻しました: %s\n", orig.c_str());
        restored++;
    }
    if (restored == 0) { std::fprintf(out, "できませんでした\n"); return 1; }
    std::fprintf(out, "%d 件を戻しました。\n", restored);
    return 0;
}

// ---------------------------------------------------------------- 起動可能なファイルを作る
//
// 「存在可能で起動可能な条件」を持つファイルを、無ければ作成する。
// 変更だけでなく新規作成も行う。既にあるものは壊れていれば作り直す。
//
// 作る中身は最小限の妥当な形であり、対象を特定する情報は含めない。

extern "C" int nx_make_run(const char *app, FILE *out) {
    if (!out) out = stdout;
    std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    std::string prefroot = home + "/Library/Preferences/Adobe/After Effects";

    // 状態の置き場を決める。既存があればその中、無ければ作る。
    std::string base = prefroot;
    if (DIR *d = opendir(prefroot.c_str())) {
        std::string best;
        while (struct dirent *e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string p = prefroot + "/" + e->d_name;
            if (is_dir(p)) best = p;   // 最後に見つかったものを使う
        }
        closedir(d);
        if (!best.empty()) base = best;
    }

    struct Item { std::string path; std::string body; };
    std::vector<Item> items;

    // 最小限のワークスペース一覧 (ビルトインの名前だけ)
    items.push_back({ base + "/Workspaces.xml",
        "<?xml version='1.0'?>\n<prop.map version='4'>\n<prop.list>\n"
        "<prop.pair><key>BuiltIns</key><array>\n"
        "<array.type><ustring/></array.type>\n"
        "<ustring>Default</ustring>\n"
        "</array></prop.pair>\n"
        "</prop.list>\n</prop.map>\n" });

    // 最小限のワークスペース本体。宣言と整合した実体を持たせる。
    items.push_back({ base + "/ModifiedWorkspaces/UserWorkspace.xml",
        "<?xml version='1.0'?>\n<prop.map version='4'>\n<prop.list>\n"
        "<prop.pair><key>DVA_Wrkspce</key><string>1.2</string></prop.pair>\n"
        "<prop.pair><key>TopLevelFrame-0</key><prop.list>\n"
        "<prop.pair><key>HasToolBar</key><true/></prop.pair>\n"
        "<prop.pair><key>Splitter</key><prop.list>\n"
        "<prop.pair><key>Orient</key><false/></prop.pair>\n"
        "<prop.pair><key>Place</key><float>0.5</float></prop.pair>\n"
        "<prop.pair><key>Sub1</key><prop.list>\n"
        "<prop.pair><key>Splitter</key><prop.list>\n"
        "<prop.pair><key>Place</key><float>0.5</float></prop.pair>\n"
        "<prop.pair><key>Sub1</key><prop.list/></prop.pair>\n"
        "<prop.pair><key>Sub2</key><prop.list/></prop.pair>\n"
        "</prop.list></prop.pair>\n"
        "</prop.list></prop.pair>\n"
        "<prop.pair><key>Sub2</key><prop.list/></prop.pair>\n"
        "</prop.list></prop.pair>\n"
        "</prop.list></prop.pair>\n"
        "</prop.list>\n</prop.map>\n" });

    // 最小限のダイアログ設定
    items.push_back({ base + "/DVADialogPrefs/DVADialogPrefs.xml",
        "<?xml version='1.0'?>\n<prop.map version='4'>\n<prop.list/>\n</prop.map>\n" });

    int made = 0, kept = 0;
    for (const auto &it : items) {
        if (is_file(it.path)) {
            // 壊れている (宣言はあるが実体が無い) なら作り直す
            std::string body = read_all(it.path);
            bool decl = body.find("prop.map") != std::string::npos;
            bool data = body.find("Splitter") != std::string::npos ||
                        body.find("TabPanel") != std::string::npos;
            bool broken = decl && !data &&
                          it.path.find("UserWorkspace") != std::string::npos;
            if (!broken) { kept++; continue; }
        }
        // 置き場所を作る (無ければ親ごと作る)
        std::string dir = it.path.substr(0, it.path.find_last_of('/'));
        std::string cur;
        for (size_t i = 0; i < dir.size(); i++) {
            cur.push_back(dir[i]);
            if (dir[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0700);
        }
        mkdir(dir.c_str(), 0700);

        std::ofstream o(it.path, std::ios::binary | std::ios::trunc);
        if (!o) { std::fprintf(out, "作れませんでした: %s\n", it.path.c_str()); continue; }
        o.write(it.body.data(), (std::streamsize)it.body.size());
        if (!o) { std::fprintf(out, "書けませんでした: %s\n", it.path.c_str()); continue; }
        std::fprintf(out, "作成しました: %s\n", it.path.c_str());
        made++;
    }
    std::fprintf(out, "作成は %d 件で、既に妥当だったものは %d 件です。\n", made, kept);
    return 0;
}

// neodax (C の CLI) から呼ぶ。アプリ固有の名前は持たない。

extern "C" int nx_cond_run(const char *app, int drop_each, FILE *out) {
    if (!app || !*app) return 2;
    std::string root(app);
    if (!root.empty() && root.back() == '/') root.pop_back();

    std::string contents = root + "/Contents";
    std::string macos = contents + "/MacOS";
    std::string fw = contents + "/Frameworks";

    // 実行ファイルは MacOS 直下の最初の実行可能ファイルとする。
    std::string exe;
    if (DIR *d = opendir(macos.c_str())) {
        while (struct dirent *e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string p = macos + "/" + e->d_name;
            if (is_file(p) && can(p, X_OK)) { exe = p; break; }
        }
        closedir(d);
    }

    std::string home = getenv("HOME") ? getenv("HOME") : "";
    std::string prefroot = home + "/Library/Preferences/Adobe/After Effects";

    FileCondition        c1("C1", "実行ファイルが存在して実行できる", exe, true, false);
    FileCondition        c2("C2", "バンドルに Info.plist がある", contents + "/Info.plist", false, true);
    FrameworksCondition  c3("C3", "必須フレームワークが揃っている", fw,
                            {"AfterFXLib.framework", "dvacore.framework"});
    ArgsCondition        c4("C4", "起動の仕方を変える引数が付いていない",
                            std::vector<std::string>{},
                            {"-aerender", "-headlessonly"});
    StateCondition       c5("C5", "復元元の状態が壊れていない", prefroot,
                            {"UserWorkspace", "Workspaces"});
    DirWritableCondition c6("C6", "ログ出力先に書ける", prefroot);

    nx::Probe probe;
    probe.add(&c1); probe.add(&c2); probe.add(&c3);
    probe.add(&c4); probe.add(&c5); probe.add(&c6);

    if (!out) out = stdout;
    if (drop_each) probe.report_drop_each(out);
    else probe.report(out);
    return probe.all_ok() ? 0 : 1;
}
