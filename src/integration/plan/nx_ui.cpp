// nx_ui.cpp — UI 挙動の静的抽出とアルゴリズム表示
//
// アプリは起動しない。asar / JS / JSON を読み、文字列・prop・関数領域から
//   action -> handler -> command -> state -> persist
// の関係を組み立てる。minify 済みでも読める範囲だけを扱い、解決できない
// ものは推測で埋めない。

#include "nx_ui.hpp"
#include "nx_deepai.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

struct Row { std::string kind, key, value; };
struct Link { std::string from, to, kind; };
struct Lit { std::string value, file; size_t off; size_t abs; };

struct Scan {
    std::string app;
    std::string root;
    std::vector<Row> rows;
    std::vector<Link> links;
    std::vector<Lit> lits;
    std::set<std::string> keys;
    std::set<std::string> commands;
    size_t bytes = 0;
    int files = 0;

    void add(const std::string &kind, const std::string &key, const std::string &value) {
        if (key.empty()) return;
        std::string id = kind + "\t" + key;
        if (!keys.insert(id).second) return;
        rows.push_back({kind, key, value});
    }
    void link(const std::string &a, const std::string &b, const std::string &kind) {
        if (a.empty() || b.empty() || a == b) return;
        links.push_back({a, b, kind});
    }
};

std::string home() {
    const char *h = getenv("HOME");
    return h && *h ? h : ".";
}
std::string join(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    return a.back() == '/' ? a + b : a + "/" + b;
}
bool exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}
bool is_dir(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
void mkdirs(const std::string &p) {
    std::string cur;
    for (size_t i = 0; i < p.size(); i++) {
        cur.push_back(p[i]);
        if (p[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0700);
    }
    mkdir(p.c_str(), 0700);
}
std::string safe(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '_' || c == '-' || c == '.') o.push_back((char)c);
        else o.push_back('_');
    }
    if (o.empty()) o = "x";
    if (o.size() > 120) o.resize(120);
    return o;
}
std::string hash8(const std::string &s) {
    unsigned long long h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    char b[16];
    snprintf(b, sizeof b, "%08llx", h & 0xffffffffull);
    return b;
}
std::string now_iso() {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", &tmv);
    return b;
}
bool ends_with(const std::string &s, const char *p) {
    size_t n = strlen(p);
    return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}
std::string trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b])) b++;
    while (e > b && isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}
void one_line(std::string &s) {
    for (char &c : s) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (s.size() > 600) s.resize(600);
}

/* 回転の軸と角度。transform / rotate / @keyframes の本体から
   rotate() / rotateX() / rotateY() / rotateZ() / rotate3d() / rotate: を読む。
   連続値は補間側の性質なので、ここでは両端 (from/to) だけを残す。 */
void read_rotation(const std::string &body, std::string &axis,
                   std::string &from, std::string &to, std::string &vec) {
    std::vector<std::string> axes, angles, vecs;
    size_t p = 0;
    while ((p = body.find("rotate", p)) != std::string::npos) {
        size_t q = p + 6;
        std::string ax = "z";
        if (q < body.size() && (body[q] == 'X' || body[q] == 'Y' || body[q] == 'Z')) {
            ax.assign(1, (char)tolower((unsigned char)body[q]));
            q++;
        } else if (q + 2 <= body.size() && body.compare(q, 2, "3d") == 0) {
            ax = "3d";
            q += 2;
        }
        while (q < body.size() && (body[q] == ' ' || body[q] == '\t')) q++;
        std::string ang;
        std::string axis_vec = ax == "x" ? "1,0,0" : ax == "y" ? "0,1,0" : "0,0,1";
        if (q < body.size() && body[q] == '(') {
            size_t e = body.find(')', q);
            if (e == std::string::npos) break;
            std::string inside = body.substr(q + 1, e - q - 1);
            if (ax == "3d") {
                /* rotate3d(x,y,z,angle): 軸は最後の成分だけでは決まらない。
                   4 成分のうち前 3 つをそのまま軸として保存する。 */
                double vx = 0, vy = 0, vz = 1;
                std::sscanf(inside.c_str(), "%lf,%lf,%lf", &vx, &vy, &vz);
                char b[96];
                std::snprintf(b, sizeof b, "%.10g,%.10g,%.10g", vx, vy, vz);
                axis_vec = b;
                size_t c = inside.rfind(',');
                ang = trim(c == std::string::npos ? inside : inside.substr(c + 1));
            } else {
                ang = trim(inside);
            }
            p = e + 1;
        } else if (q < body.size() && body[q] == ':') {
            size_t e = body.find_first_of(";}", q);
            ang = trim(body.substr(q + 1, (e == std::string::npos ? body.size() : e) - q - 1));
            p = e == std::string::npos ? body.size() : e + 1;
        } else {
            p = q;
            continue;
        }
        if (!ang.empty()) {
            axes.push_back(ax);
            angles.push_back(ang);
            vecs.push_back(axis_vec);
        }
    }
    if (angles.empty()) return;
    axis = axes.front();
    from = angles.front();
    to = angles.back();
    vec = vecs.front();
}

/* "0" / "360deg" / "1turn" / "-4deg" を度に直す。 */
double deg_value(std::string s) {
    if (s.empty()) return 0.0;
    double mul = 1.0;
    auto ends = [&](const char *x) {
        size_t n = std::strlen(x);
        return s.size() >= n && s.compare(s.size() - n, n, x) == 0;
    };
    if (ends("deg")) s.resize(s.size() - 3);
    else if (ends("turn")) { s.resize(s.size() - 4); mul = 360.0; }
    else if (ends("rad")) { s.resize(s.size() - 3); mul = 57.29577951308232; }
    return std::atof(s.c_str()) * mul;
}

std::string rotation_value(const std::string &sel, const std::string &file,
                           const std::string &body, const std::string &axis,
                           const std::string &from, const std::string &to,
                           const std::string &vec) {
    /* from != to は「周期」ではない。0→90 は 1 回で止まる。
       周期なのは to-from が 360 の整数倍のときだけ。 */
    const double f = deg_value(from), t = deg_value(to);
    bool periodic = false;
    int turns = 0;
    const double d = t - f;
    if (std::fabs(d) > 1e-9) {
        const double q = d / 360.0;
        const double r = std::round(q);
        if (std::fabs(q - r) < 1e-6) {
            periodic = true;
            turns = (int)std::llround(r);
        }
    }
    std::string v = "selector=" + sel + " file=" + file + " body=" + body +
                    " axis=" + axis + " vec=" + vec + " from=" + from + " to=" + to +
                    " periodic=" + (periodic ? "1" : "0") +
                    " turns=" + std::to_string(turns);
    one_line(v);
    return v;
}

std::string app_asar(const std::string &path) {
    if (ends_with(path, ".asar")) return path;
    if (is_dir(path)) {
        const char *cand[] = {
            "/Contents/Resources/app.asar",
            "/Resources/app.asar",
            "/app.asar",
            "/Contents/Resources/app/app.asar",
        };
        for (const char *c : cand) if (exists(path + c)) return path + c;
        return "";
    }
    return "";
}

std::string default_dir(const std::string &app) {
    std::string base = app;
    size_t sl = base.find_last_of('/');
    if (sl != std::string::npos) base = base.substr(sl + 1);
    return join(join(home(), ".neodax/apps"), safe(base) + "_" + hash8(app));
}

bool is_js(const std::string &p) {
    return ends_with(p, ".js") || ends_with(p, ".mjs") || ends_with(p, ".cjs") || ends_with(p, ".json");
}
bool is_css(const std::string &p) { return ends_with(p, ".css"); }
bool is_asset(const std::string &p) { return is_js(p) || is_css(p); }

/* CSS の transition / transform / animation / @keyframes を構造として拾う。
   文字列を読むのではなく、宣言ブロックの形だけを見る。 */
void scan_css(const std::string &file, const std::string &src, size_t base, Scan &s) {
    (void)base;
    s.files++;
    s.bytes += src.size();
    std::string mod = "mod." + safe(file);
    s.add("module", mod, "file=" + file + " bytes=" + std::to_string(src.size()));

    std::string t;
    for (size_t i = 0; i < src.size();) {
        if (i + 1 < src.size() && src[i] == '/' && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        t.push_back(src[i++]);
    }

    size_t i = 0;
    while (i < t.size()) {
        size_t open = t.find('{', i);
        if (open == std::string::npos) break;
        std::string sel = trim(t.substr(i, open - i));
        size_t lb = sel.find_last_of(";}");
        if (lb != std::string::npos) sel = trim(sel.substr(lb + 1));

        bool kf = sel.rfind("@keyframes", 0) == 0 || sel.rfind("@-webkit-keyframes", 0) == 0;
        size_t close;
        std::string body;
        if (kf) {
            /* @keyframes は内側にも {} を持つ。外形の閉じまでを 1 つの本体として
               読むことで、0% と to を別ノードに割らず 1 回転として扱える。 */
            int depth = 1;
            close = open + 1;
            while (close < t.size() && depth > 0) {
                if (t[close] == '{') depth++;
                else if (t[close] == '}') depth--;
                close++;
            }
            if (depth != 0) break;
            body = t.substr(open + 1, close - 1 - (open + 1));
        } else {
            close = t.find('}', open);
            if (close == std::string::npos) break;
            body = t.substr(open + 1, close - open - 1);
        }

        std::string low = body;
        for (char &c : low) c = (char)tolower((unsigned char)c);

        bool motion = low.find("transition") != std::string::npos ||
                      low.find("transform") != std::string::npos ||
                      low.find("animation") != std::string::npos ||
                      low.find("rotate") != std::string::npos;

        if (kf) {
            std::string name = trim(sel.substr(sel.find(' ') == std::string::npos ? sel.size() : sel.find(' ') + 1));
            std::string id = "kf." + safe(name.empty() ? sel : name);
            std::string v = "file=" + file + " body=" + body;
            one_line(v);
            s.add("keyframes", id, v);
            s.link(mod, id, "declares");

            std::string axis, from, to, vec;
            read_rotation(body, axis, from, to, vec);
            if (!from.empty()) {
                std::string rid = "rot." + safe(name.empty() ? sel : name);
                s.add("rotation", rid, rotation_value(sel, file, body, axis, from, to, vec));
                s.link(mod, rid, "declares");
                s.link(id, rid, "rotates");
                s.link(rid, id, "requires");
            }
        }
        if (motion) {
            std::string id = "motion." + safe(sel.empty() ? ("rule" + std::to_string(i)) : sel);
            std::string v = "selector=" + sel + " file=" + file + " body=" + body;
            one_line(v);
            s.add("motion", id, v);
            s.link(mod, id, "declares");
            if (low.find("linear") != std::string::npos) s.link(id, "timing.linear", "timing");

            std::string axis, from, to, vec;
            if (!kf) read_rotation(body, axis, from, to, vec);
            if (!from.empty()) {
                std::string rid = "rot." + safe(sel) + "." + hash8(file + sel);
                s.add("rotation", rid, rotation_value(sel, file, body, axis, from, to, vec));
                s.link(mod, rid, "declares");
                s.link(id, rid, "rotates");
                s.link(rid, id, "requires");
            }
        }
        i = kf ? close : close + 1;
    }
}

/* --include は部分一致と簡易 glob (*) を受ける。 */
bool include_match(const std::string &pat, const std::string &name) {
    if (pat.empty()) return true;
    if (pat.find('*') == std::string::npos) return name.find(pat) != std::string::npos;
    size_t p = 0, n = 0, star = std::string::npos, mark = 0;
    while (n < name.size()) {
        if (p < pat.size() && pat[p] == name[n]) { p++; n++; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; mark = n; }
        else if (star != std::string::npos) { p = star + 1; n = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') p++;
    return p == pat.size();
}

const char *kProps[] = {
    "onClick", "onDoubleClick", "onKeyDown", "onKeyUp", "onSubmit", "onChange",
    "onInput", "onPointerDown", "onMouseDown", "onSelect", "addEventListener",
};

bool is_command(const std::string &s) {
    if (s.size() < 4 || s.size() > 140) return false;
    if (s.find_first_of(" \t\n\"'`") != std::string::npos) return false;
    size_t d = s.find('.');
    if (d == std::string::npos || d == 0) return false;
    static const char *prefix[] = {
        "thread", "composer", "workspace", "project", "tab", "diff", "browser",
        "terminal", "git", "app", "surface", "sidebar", "file", "editor",
        "keybinding", "provider", "auth", "settings", "mcp", "skill", "command",
        "run", "turn", "review", "search", "find", "model", "session"
    };
    std::string p = s.substr(0, d);
    for (const char *x : prefix) if (p == x) return true;
    return false;
}

bool is_storage_key(const std::string &s) {
    if (s.size() < 3 || s.size() > 140) return false;
    if (s.find_first_of(" \t\n\"'`") != std::string::npos) return false;
    if (s.find("://") != std::string::npos) return false;
    if (s.rfind("http", 0) == 0) return false;
    if (s.find(':') == std::string::npos && s.find("workspace-tabs") == std::string::npos)
        return false;
    return true;
}

bool is_keybinding(const std::string &s) {
    if (s.size() < 4 || s.size() > 80) return false;
    if (s.find('+') == std::string::npos) return false;
    std::string t = s;
    for (char &c : t) c = (char)tolower((unsigned char)c);
    if (t.find("mod+") == std::string::npos && t.find("cmd+") == std::string::npos &&
        t.find("meta+") == std::string::npos && t.find("ctrl+") == std::string::npos &&
        t.find("alt+") == std::string::npos && t.find("shift+") == std::string::npos)
        return false;
    return true;
}

/* JS の文字列リテラルだけを拾う。コメントは飛ばす。 */
void lex_strings(const std::string &file, const std::string &src, size_t base, Scan &s) {
    size_t i = 0, n = src.size();
    while (i < n) {
        char c = src[i];
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            i += 2;
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        if (c != '\'' && c != '"' && c != '`') { i++; continue; }
        char q = c;
        size_t start = ++i;
        std::string v;
        bool closed = false;
        while (i < n) {
            char d = src[i];
            if (d == '\\') {
                if (i + 1 < n) {
                    char e = src[i + 1];
                    if (e == 'n') v.push_back('\n');
                    else if (e == 't') v.push_back('\t');
                    else if (e == 'r') v.push_back('\r');
                    else v.push_back(e);
                    i += 2;
                    continue;
                }
                i++;
                continue;
            }
            if (d == q) { closed = true; i++; break; }
            if (q == '`' && d == '$' && i + 1 < n && src[i + 1] == '{') {
                /* 補間は式なので文字列を切る。閉じ括弧まで飛ばす。 */
                closed = true;
                i += 2;
                int depth = 1;
                while (i < n && depth > 0) {
                    if (src[i] == '{') depth++;
                    else if (src[i] == '}') depth--;
                    i++;
                }
                break;
            }
            v.push_back(d);
            i++;
        }
        (void)closed;
        if (v.size() <= 600) {
            Lit l{v, file, start, base + start};
            s.lits.push_back(l);
        }
    }
}

std::string nearest_command(const std::vector<Lit> &lits, size_t pos, size_t window) {
    std::string best;
    size_t bestd = (size_t)-1;
    for (const Lit &l : lits) {
        if (!is_command(l.value)) continue;
        size_t d = l.off >= pos ? l.off - pos : pos - l.off;
        if (d < bestd && d <= window) { bestd = d; best = l.value; }
    }
    return best;
}

std::string nearest_storage(const std::vector<Lit> &lits, size_t pos, size_t window) {
    std::string best;
    size_t bestd = (size_t)-1;
    for (const Lit &l : lits) {
        if (!is_storage_key(l.value)) continue;
        size_t d = l.off >= pos ? l.off - pos : pos - l.off;
        if (d < bestd && d <= window) { bestd = d; best = l.value; }
    }
    return best;
}

std::string nearest_label(const std::vector<Lit> &lits, size_t pos, size_t window) {
    std::string best;
    size_t bestd = (size_t)-1;
    for (const Lit &l : lits) {
        if (l.value.size() < 2 || l.value.size() > 80) continue;
        if (l.value.find_first_of("{}();=") != std::string::npos) continue;
        if (is_command(l.value) || is_storage_key(l.value) || is_keybinding(l.value)) continue;
        size_t d = l.off >= pos ? l.off - pos : pos - l.off;
        if (d < bestd && d <= window) { bestd = d; best = l.value; }
    }
    return best;
}

void scan_text(const std::string &file, const std::string &src, size_t base, Scan &s) {
    s.files++;
    s.bytes += src.size();
    size_t lit_start = s.lits.size();
    lex_strings(file, src, base, s);
    std::vector<Lit> local(s.lits.begin() + (std::ptrdiff_t)lit_start, s.lits.end());

    std::string mod = "mod." + safe(file);
    s.add("module", mod, "file=" + file + " bytes=" + std::to_string(src.size()));

    /* command / persist / keybinding / state */
    for (const Lit &l : local) {
        if (l.file != file) continue;
        if (is_command(l.value)) {
            std::string id = "cmd." + safe(l.value);
            s.add("command", id, "string=" + l.value + " file=" + file + " offset=" + std::to_string(l.off));
            s.commands.insert(l.value);
            s.link(mod, id, "declares");
        }
        if (is_storage_key(l.value)) {
            bool persist = l.value.find("workspace-tabs") != std::string::npos ||
                           l.value.find("t3code") != std::string::npos ||
                           l.value.find("deepai-code") != std::string::npos ||
                           l.value.find(":v") != std::string::npos;
            if (persist) {
                std::string id = "pst." + safe(l.value);
                s.add("persist", id, "key=" + l.value + " file=" + file);
                s.link(mod, id, "persists");
            }
        }
        if (is_keybinding(l.value)) {
            std::string id = "key." + safe(l.value);
            std::string cmd = nearest_command(local, l.off, 12000);
            s.add("keybinding", id, "keys=" + l.value + " file=" + file);
            if (!cmd.empty()) s.link(id, "cmd." + safe(cmd), "maps");
            s.link(mod, id, "declares");
        }
    }

    /* actions: prop / listener の位置から、近傍の文字列をラベルにする */
    int actn = 0;
    for (const char *prop : kProps) {
        size_t p = 0;
        while ((p = src.find(prop, p)) != std::string::npos) {
            size_t off = p;
            p += strlen(prop);
            if (off > 0 && (isalnum((unsigned char)src[off - 1]) || src[off - 1] == '_')) continue;
            std::string label = nearest_label(local, off, 2500);
            std::string cmd = nearest_command(local, off, 30000);
            std::string hnd = "hnd." + safe(file) + "." + std::to_string(actn);
            std::string id = "act." + safe(prop) + "." + std::to_string(actn);
            actn++;
            std::string val = "event=" + std::string(prop) +
                              " file=" + file + " offset=" + std::to_string(off);
            if (!label.empty()) val += " label=" + label;
            if (!cmd.empty()) val += " command=" + cmd;
            s.add("action", id, val);
            s.add("handler", hnd, "file=" + file + " offset=" + std::to_string(off) +
                                      (cmd.empty() ? "" : " command=" + cmd));
            s.link(id, hnd, "handled_by");
            s.link(hnd, mod, "in_module");
            if (!cmd.empty()) s.link(hnd, "cmd." + safe(cmd), "dispatches");
            if (actn >= 4000) break;
        }
        if (actn >= 4000) break;
    }

    /* JS の inline transform。CSS に現れない回転 (状態で角度を切り替える
       コンポーネント) はここで初めてノードになる。 */
    for (const Lit &l : local) {
        if (l.file != file || l.value.empty()) continue;
        std::string axis, from, to, vec;
        read_rotation(l.value, axis, from, to, vec);
        if (from.empty()) continue;
        std::string where = file + "|" + std::to_string(l.off) + "|" + l.value;
        std::string mid = "motion.js." + hash8(where);
        std::string rid = "rot.js." + hash8(where);
        std::string m = "selector=js:" + safe(l.value) + " file=" + file +
                        " offset=" + std::to_string(l.off) + " body=" + l.value +
                        " axis=" + axis + " vec=" + vec + " from=" + from + " to=" + to +
                        " source=js";
        one_line(m);
        s.add("motion", mid, m);
        s.add("rotation", rid, rotation_value("js:" + safe(l.value), file, l.value,
                                              axis, from, to, vec) +
                                              " offset=" + std::to_string(l.off) + " source=js");
        s.link(mod, mid, "declares");
        s.link(mid, rid, "rotates");
        s.link(rid, mid, "requires");
    }

    /* command -> persist / event の近傍リンク */
    for (const Lit &l : local) {
        if (l.file != file || !is_command(l.value)) continue;
        std::string pst = nearest_storage(local, l.off, 30000);
        if (!pst.empty()) s.link("cmd." + safe(l.value), "pst." + safe(pst), "writes");
        std::string lab = nearest_label(local, l.off, 1200);
        if (!lab.empty()) s.add("event", "evt." + safe(l.value), "label=" + lab + " file=" + file);
    }
}

void scan_dir(const std::string &dir, int depth, const std::string &include, Scan &s) {
    if (depth < 0) return;
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        std::string p = join(dir, e->d_name);
        if (is_dir(p)) { scan_dir(p, depth - 1, include, s); continue; }
        if (!is_asset(p)) continue;
        if (!include_match(include, e->d_name)) continue;
        FILE *f = fopen(p.c_str(), "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > (64L << 20)) { fclose(f); continue; }
        std::string text((size_t)sz, '\0');
        if (fread(&text[0], 1, (size_t)sz, f) != (size_t)sz) { fclose(f); continue; }
        fclose(f);
        if (is_css(p)) scan_css(p, text, 0, s);
        else scan_text(p, text, 0, s);
        if (s.files >= 400) break;
    }
    closedir(d);
}

bool write_all(const std::string &p, const std::string &s) {
    FILE *f = fopen(p.c_str(), "wb");
    if (!f) return false;
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
    return true;
}

std::string node_line(const Row &r) {
    std::string label = r.value;
    one_line(label);
    return "node " + r.key + " " + r.kind + " " + label +
           " element:" + r.key + " action:" + r.kind;
}

std::string emit_tsv(const Scan &s) {
    std::string o;
    for (const Row &r : s.rows) {
        std::string v = r.value;
        one_line(v);
        o += r.kind + "\t" + r.key + "\t" + v + "\n";
    }
    return o;
}

std::string emit_nodes(const Scan &s) {
    std::string o = "# generated by neodax ui-scan\n";
    for (const Row &r : s.rows) o += node_line(r) + "\n";
    for (const Link &l : s.links)
        o += "link " + l.from + " " + l.kind + " " + l.to + "\n";
    return o;
}

std::string emit_cpp(const Scan &s) {
    std::string o;
    o += "// generated by neodax ui-scan\n";
    o += "// target: " + s.app + "\n";
    o += "// ===== NX TEMPLATE =====\n";
    o += "// id: ui." + hash8(s.root) + "\n";
    o += "// kind: ui\n";
    o += "// target: " + s.app + "\n";
    for (const Row &r : s.rows) {
        std::string label = r.value;
        one_line(label);
        o += "// node: " + r.key + " " + r.kind + " " + label +
             " element:" + r.key + " action:" + r.kind + "\n";
    }
    for (const Link &l : s.links)
        o += "// link: " + l.from + " " + l.kind + " " + l.to + "\n";
    o += "// ===== /NX TEMPLATE =====\n\n";
    o += "#include <cstddef>\n\n";
    o += "namespace nx_ui {\n\n";
    o += "struct node { const char *id; const char *kind; const char *label; };\n";
    o += "static const node nodes[] = {\n";
    for (const Row &r : s.rows) {
        std::string label = r.value;
        one_line(label);
        /* C++ 文字列として安全にする */
        std::string esc;
        for (char c : label) {
            if (c == '"' || c == '\\') { esc.push_back('\\'); esc.push_back(c); }
            else if ((unsigned char)c < 32) esc.push_back(' ');
            else esc.push_back(c);
        }
        o += "    {\"" + r.key + "\", \"" + r.kind + "\", \"" + esc + "\"},\n";
    }
    o += "};\n\n";
    o += "static const std::size_t node_count = sizeof(nodes) / sizeof(nodes[0]);\n\n";
    o += "} // namespace nx_ui\n";
    return o;
}

std::string emit_hex(const Scan &s) {
    std::string o = "# generated by neodax ui-scan\n";
    char line[128];
    snprintf(line, sizeof line, "# app: %s\n", s.app.c_str());
    o += line;
    std::set<std::string> seen;
    for (const Lit &l : s.lits) {
        if (l.value.empty()) continue;
        if (!is_command(l.value) && !is_storage_key(l.value) && !is_keybinding(l.value)) continue;
        if (!seen.insert(l.value).second) continue;
        snprintf(line, sizeof line, "\n# string 0x%zx %s\n", l.abs, l.file.c_str());
        o += line;
        size_t k = 0;
        while (k < l.value.size()) {
            size_t n = std::min<size_t>(16, l.value.size() - k);
            char b[40];
            snprintf(b, sizeof b, "%016zx  ", l.abs + k);
            o += b;
            for (size_t i = 0; i < 16; i++) {
                if (i < n) {
                    snprintf(b, sizeof b, "%02x ", (unsigned char)l.value[k + i]);
                    o += b;
                } else o += "   ";
                if (i == 7) o += ' ';
            }
            o += " |";
            for (size_t i = 0; i < n; i++) {
                unsigned char c = (unsigned char)l.value[k + i];
                o += (c >= 32 && c < 127) ? (char)c : '.';
            }
            o += "|\n";
            k += n;
        }
    }
    return o;
}

/* ---- flow ---- */

std::vector<std::string> split_tsv(const std::string &line) {
    std::vector<std::string> v;
    size_t p = 0;
    for (;;) {
        size_t t = line.find('\t', p);
        if (t == std::string::npos) { v.push_back(line.substr(p)); break; }
        v.push_back(line.substr(p, t - p));
        p = t + 1;
    }
    return v;
}

bool load_rows(const std::string &dir, std::vector<Row> &rows) {
    std::string p = join(dir, "ui.tsv");
    std::ifstream f(p);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::vector<std::string> v = split_tsv(line);
        if (v.size() >= 3) rows.push_back({v[0], v[1], v[2]});
    }
    return !rows.empty();
}

std::string lower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

int score(const Row &r, const std::string &m) {
    std::string key = lower(r.key), val = lower(r.value), mm = lower(m);
    int s = 0;
    if (key == mm) s += 8;
    if (key.find(mm) != std::string::npos) s += 5;
    if (val.find(mm) != std::string::npos) s += 3;
    /* 空白区切りの語も見る */
    std::stringstream ss(mm);
    std::string w;
    while (ss >> w) {
        if (w.empty()) continue;
        if (key.find(w) != std::string::npos) s += 2;
        if (val.find(w) != std::string::npos) s += 1;
    }
    return s;
}

/* value=... の 1 フィールド。selector= のように空白を含む値は
   field_between で次のフィールドまでを切る。 */
std::string field_between(const std::string &v, const char *from, const char *to) {
    size_t p = v.find(from);
    if (p == std::string::npos) return "";
    p += std::strlen(from);
    size_t e = v.find(to, p);
    return v.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

/* "0" / "360deg" / "1turn" / "-4deg" を度に直す。 */

std::string cpp_escape(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
        else if ((unsigned char)c < 32) o.push_back(' ');
        else o.push_back(c);
    }
    return o;
}

/* 観測された rotation 行だけから C++ を出す。アプリ名には依存しない。
   --format rotcpp でも --format cpp でも同じ関数を通す。 */
void emit_rotation_cpp(const std::vector<const Row *> &rots, const std::string &ns,
                       FILE *out) {
    fprintf(out, "// generated from observed rotation rows\n");
    fprintf(out, "#include <algorithm>\n#include <cmath>\n#include <cstddef>\n\n");
    fprintf(out, "namespace %s {\n\n", ns.c_str());
    fprintf(out, "struct Vec3 { double x, y, z; };\n\n");
    fprintf(out, "struct Rotation {\n");
    fprintf(out, "    const char *keyframes;\n");
    fprintf(out, "    const char *file;\n");
    fprintf(out, "    const char *axis_name;   // z / x / y / 3d (表示用)\n");
    fprintf(out, "    Vec3 axis;               // 実際の軸。3d でもここで一意になる\n");
    fprintf(out, "    double from_deg;\n");
    fprintf(out, "    double to_deg;\n");
    fprintf(out, "    bool periodic;           // to-from が 360 の整数倍のときだけ true\n");
    fprintf(out, "    int turns;               // periodic のときの周回数 (0→720 なら 2)\n");
    fprintf(out, "};\n\n");

    int nr = 0;
    for (const Row *r : rots) {
        const std::string &v = r->value;
        std::string sel = field_between(v, "selector=", " file=");
        if (sel.rfind("@keyframes ", 0) == 0) sel = sel.substr(11);
        else if (sel.rfind("@-webkit-keyframes ", 0) == 0) sel = sel.substr(19);
        std::string file = field_between(v, "file=", " body=");
        std::string axis = field_between(v, "axis=", " vec=");
        std::string vec = field_between(v, "vec=", " from=");
        std::string from = field_between(v, "from=", " to=");
        std::string to = field_between(v, "to=", " periodic=");
        if (to.find(" spins=") != std::string::npos) to = to.substr(0, to.find(" spins="));
        bool periodic = v.find("periodic=1") != std::string::npos;
        if (v.find("periodic=") == std::string::npos)
            periodic = v.find("spins=1") != std::string::npos;  /* 旧データ */
        int turns = std::atoi(field_between(v, "turns=", " ").c_str());
        double ax = 0, ay = 0, az = 1;
        if (!vec.empty()) std::sscanf(vec.c_str(), "%lf,%lf,%lf", &ax, &ay, &az);
        else if (axis == "x") { ax = 1; az = 0; }
        else if (axis == "y") { ay = 1; az = 0; }
        fprintf(out, "static constexpr Rotation R%d = {\n", nr);
        fprintf(out, "    \"%s\",\n    \"%s\",\n    \"%s\",\n",
                cpp_escape(sel).c_str(), cpp_escape(file).c_str(), cpp_escape(axis).c_str());
        fprintf(out, "    { %.10g, %.10g, %.10g },\n", ax, ay, az);
        fprintf(out, "    %.10g, %.10g, %s, %d\n", deg_value(from), deg_value(to),
                periodic ? "true" : "false", turns);
        fprintf(out, "};\n");
        nr++;
    }
    if (nr == 0) {
        fprintf(out, "static constexpr Rotation rotations[] = { { \"\", \"\", \"z\", { 0, 0, 1 }, 0, 0, false, 0 } };\n");
    } else {
        fprintf(out, "static constexpr Rotation rotations[] = {\n");
        for (int i = 0; i < nr; i++) fprintf(out, "    R%d,\n", i);
        fprintf(out, "};\n");
    }
    fprintf(out, "static constexpr std::size_t rotation_count = %d;\n\n", nr);

    fprintf(out, "inline double angle_at(const Rotation &r, double t, double duration) {\n");
    fprintf(out, "    if (duration <= 0.0) return r.from_deg;\n");
    fprintf(out, "    double u = t / duration;\n");
    fprintf(out, "    if (r.periodic) { u = std::fmod(u, 1.0); if (u < 0.0) u += 1.0; }\n");
    fprintf(out, "    else u = std::min(1.0, std::max(0.0, u));\n");
    fprintf(out, "    return r.from_deg + (r.to_deg - r.from_deg) * u;\n");
    fprintf(out, "}\n\n");

    /* 任意軸回転 (Rodrigues)。z 専用の分岐をなくし、3d でも
       (x,y,z,angle) の情報を落とさない。 */
    fprintf(out, "inline void matrix(const Vec3 &a, double deg, double m[9]) {\n");
    fprintf(out, "    for (int i = 0; i < 9; i++) m[i] = (i %% 4 == 0) ? 1.0 : 0.0;\n");
    fprintf(out, "    const double n = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);\n");
    fprintf(out, "    if (n < 1e-12) return;\n");
    fprintf(out, "    const double x = a.x / n, y = a.y / n, z = a.z / n;\n");
    fprintf(out, "    const double rad = deg * 3.14159265358979323846 / 180.0;\n");
    fprintf(out, "    const double c = std::cos(rad), s = std::sin(rad), t = 1.0 - c;\n");
    fprintf(out, "    m[0] = t*x*x + c;   m[1] = t*x*y - s*z; m[2] = t*x*z + s*y;\n");
    fprintf(out, "    m[3] = t*x*y + s*z; m[4] = t*y*y + c;   m[5] = t*y*z - s*x;\n");
    fprintf(out, "    m[6] = t*x*z - s*y; m[7] = t*y*z + s*x; m[8] = t*z*z + c;\n");
    fprintf(out, "}\n\n");
    fprintf(out, "} // namespace %s\n", ns.c_str());
}

void print_inventory(const std::vector<Row> &rows, FILE *out) {
    std::map<std::string, int> c;
    for (const Row &r : rows) c[r.kind]++;
    fprintf(out, "UI_NODES\t%zu\n", rows.size());
    for (auto &kv : c) fprintf(out, "UI_%s\t%d\n", kv.first.c_str(), kv.second);
    fprintf(out, "UI_ACTIONS_BEGIN\n");
    int n = 0;
    for (const Row &r : rows) {
        if (r.kind != "action") continue;
        std::string v = r.value;
        one_line(v);
        fprintf(out, "ACTION\t%s\t%s\n", r.key.c_str(), v.c_str());
        if (++n >= 200) break;
    }
    fprintf(out, "UI_ACTIONS_END\n");
    fprintf(out, "UI_COMMANDS_BEGIN\n");
    n = 0;
    for (const Row &r : rows) {
        if (r.kind != "command") continue;
        fprintf(out, "COMMAND\t%s\t%s\n", r.key.c_str(), r.value.c_str());
        if (++n >= 200) break;
    }
    fprintf(out, "UI_COMMANDS_END\n");
}

int run_flow(const std::string &dir, const std::string &action, const std::string &match,
             const std::string &format, FILE *out) {
    std::vector<Row> rows;
    if (!load_rows(dir, rows)) {
        fprintf(out, "ui-flow: no ui.tsv in %s\n", dir.c_str());
        return 2;
    }
    std::string target = action;
    if (target.empty() && !match.empty()) {
        int best = 0;
        for (const Row &r : rows) {
            int s = score(r, match);
            if (s > best) { best = s; target = r.key; }
        }
        if (best == 0) target.clear();
    }
    if (format == "rotations") {
        std::string mm = lower(match);
        int n = 0, periodic = 0;
        std::map<std::string, int> axes;
        fprintf(out, "ROTATIONS\n");
        for (const Row &r : rows) {
            if (r.kind != "rotation") continue;
            if (!mm.empty() && lower(r.value).find(mm) == std::string::npos &&
                lower(r.key).find(mm) == std::string::npos) continue;
            fprintf(out, "ROTATION\t%s\t%s\n", r.key.c_str(), r.value.c_str());
            n++;
            size_t a = r.value.find("axis=");
            if (a != std::string::npos) {
                size_t e = r.value.find(' ', a + 5);
                axes[r.value.substr(a + 5, e == std::string::npos ? std::string::npos : e - (a + 5))]++;
            }
            if (r.value.find("periodic=1") != std::string::npos) periodic++;
        }
        fprintf(out, "ROT_SUMMARY\tnodes=%d\tperiodic=%d", n, periodic);
        for (auto &kv : axes) fprintf(out, "\taxis_%s=%d", kv.first.c_str(), kv.second);
        fprintf(out, "\n");
        return 0;
    }

    /* 回転ノードだけを C++ にする。--action も --app も要らない。
       出力はアプリ名に依存しない (namespace は nx_rotations)。 */
    if (format == "rotcpp") {
        std::string mm = lower(match);
        std::vector<const Row *> rots;
        for (const Row &r : rows) {
            if (r.kind != "rotation") continue;
            if (!mm.empty() && lower(r.value).find(mm) == std::string::npos &&
                lower(r.key).find(mm) == std::string::npos) continue;
            rots.push_back(&r);
        }
        emit_rotation_cpp(rots, "nx_rotations", out);
        return 0;
    }

    if (target.empty()) {
        print_inventory(rows, out);
        return 0;
    }

    /* 開始ノードを探す (完全一致 → 部分一致) */
    const Row *start = NULL;
    for (const Row &r : rows) if (r.key == target) { start = &r; break; }
    if (!start) for (const Row &r : rows) if (r.key.find(target) != std::string::npos) { start = &r; break; }
    if (!start) {
        fprintf(out, "ui-flow: target not found: %s\n", target.c_str());
        return 1;
    }

    /* 幅優先で action -> handler -> command -> state -> persist を辿る */
    std::map<std::string, const Row *> bykey;
    for (const Row &r : rows) bykey[r.key] = &r;
    std::vector<std::string> order;
    std::set<std::string> seen;
    std::vector<std::string> q;

    /* ui.node の link 行は ui.tsv には入らないので、ui.node も読む */
    std::vector<Link> links;
    {
        std::ifstream f(join(dir, "ui.node"));
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("link ", 0) != 0) continue;
            std::istringstream is(line.substr(5));
            Link l;
            if (is >> l.from >> l.kind >> l.to) links.push_back(l);
        }
    }
    q.assign(1, start->key);
    seen.clear();
    seen.insert(start->key);
    order.clear();
    for (size_t qi = 0; qi < q.size() && order.size() < 400; qi++) {
        std::string k = q[qi];
        order.push_back(k);
        for (const Link &l : links) {
            if (l.from != k) continue;
            if (seen.insert(l.to).second) q.push_back(l.to);
        }
    }

    if (format == "node") {
        fprintf(out, "# algorithm %s\n", start->key.c_str());
        for (const std::string &k : order) {
            auto it = bykey.find(k);
            if (it != bykey.end()) fprintf(out, "%s\n", node_line(*it->second).c_str());
        }
        for (const Link &l : links) {
            if (seen.count(l.from) && seen.count(l.to))
                fprintf(out, "link %s %s %s\n", l.from.c_str(), l.kind.c_str(), l.to.c_str());
        }
        return 0;
    }

    if (format == "cpp") {
        fprintf(out, "// generated by neodax ui-flow\n");
        fprintf(out, "// ===== NX TEMPLATE =====\n");
        fprintf(out, "// id: flow.%s\n// kind: ui-flow\n// target: %s\n",
                hash8(start->key).c_str(), start->key.c_str());
        for (const std::string &k : order) {
            auto it = bykey.find(k);
            if (it == bykey.end()) continue;
            std::string nl = node_line(*it->second);
            /* node_line は "node <key> ..." を返す。tpl-cpp が読む形は
               "// node: <key> ..." なので、接頭辞を落として colon を付ける。 */
            if (nl.rfind("node ", 0) == 0) nl = nl.substr(5);
            fprintf(out, "// node: %s\n", nl.c_str());
        }
        for (const Link &l : links)
            if (seen.count(l.from) && seen.count(l.to))
                fprintf(out, "// link: %s %s %s\n", l.from.c_str(), l.kind.c_str(), l.to.c_str());
        fprintf(out, "// ===== /NX TEMPLATE =====\n");

        /* 実際の cpp。アプリはこの部分をコンパイルして使う。
           抽象 (mechanism) は src/templates/rotation.cpp、ここは観測値。 */
        std::vector<const Row *> rots;
        for (const std::string &k : order) {
            auto it = bykey.find(k);
            if (it != bykey.end() && it->second->kind == "rotation") rots.push_back(it->second);
        }
        emit_rotation_cpp(rots, "nx_flow_" + hash8(start->key), out);
        return 0;
    }

    if (format == "hex") {
        fprintf(out, "# algorithm %s\n", start->key.c_str());
        for (const std::string &k : order) {
            auto it = bykey.find(k);
            if (it == bykey.end()) continue;
            const std::string &v = it->second->value;
            fprintf(out, "%s\t", k.c_str());
            for (unsigned char c : v) fprintf(out, "%02x", c);
            fprintf(out, "\n");
        }
        return 0;
    }

    fprintf(out, "ALGORITHM\t%s\n", start->key.c_str());
    int step = 1;
    for (const std::string &k : order) {
        auto it = bykey.find(k);
        if (it == bykey.end()) continue;
        std::string v = it->second->value;
        one_line(v);
        fprintf(out, "STEP\t%d\t%s\t%s\t%s\n", step++, it->second->kind.c_str(), k.c_str(), v.c_str());
    }
    fprintf(out, "LINKS\n");
    for (const Link &l : links)
        if (seen.count(l.from) && seen.count(l.to))
            fprintf(out, "LINK\t%s\t%s\t%s\n", l.from.c_str(), l.kind.c_str(), l.to.c_str());
    return 0;
}

} // namespace

extern "C" int nx_ui_scan(const char *app_path, const char *out_dir, int depth,
                          const char *include, FILE *out) {
    if (!out) out = stdout;
    if (!app_path || !*app_path) { fprintf(out, "ui-scan: --path required\n"); return 2; }
    if (!exists(app_path)) { fprintf(out, "ui-scan: not found: %s\n", app_path); return 2; }

    Scan s;
    s.app = app_path;
    std::string dir = out_dir && *out_dir ? out_dir : default_dir(app_path);
    s.root = dir;
    std::string inc = include ? include : "";

    std::string asar = app_asar(app_path);
    if (!asar.empty()) {
        nx_asar_t *a = nx_asar_open(asar.c_str());
        if (!a) { fprintf(out, "ui-scan: cannot open asar: %s\n", asar.c_str()); return 1; }
        s.add("asar", "asar.main", "path=" + asar + " entries=" + std::to_string(a->n_entries));
        int scanned = 0;
        for (size_t i = 0; i < a->n_entries && scanned < 400; i++) {
            const nx_asar_entry_t *e = &a->entries[i];
            if (!is_asset(e->path)) continue;
            if (e->size == 0 || e->size > (64u << 20)) continue;
            if (!inc.empty() && !include_match(inc, e->path)) continue;
            char *txt = nx_asar_read_text(a, e);
            if (!txt) continue;
            std::string src(txt, e->size);
            free(txt);
            if (is_css(e->path)) scan_css(e->path, src, a->data_base + e->offset, s);
            else scan_text(e->path, src, a->data_base + e->offset, s);
            scanned++;
        }
        nx_asar_free(a);
    } else if (is_dir(app_path)) {
        scan_dir(app_path, depth, inc, s);
    } else {
        FILE *f = fopen(app_path, "rb");
        if (!f) { fprintf(out, "ui-scan: cannot read: %s\n", app_path); return 1; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > (64L << 20)) { fclose(f); fprintf(out, "ui-scan: unsupported size\n"); return 1; }
        std::string text((size_t)sz, '\0');
        if (fread(&text[0], 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return 1; }
        fclose(f);
        if (is_css(app_path)) scan_css(app_path, text, 0, s);
        else scan_text(app_path, text, 0, s);
    }

    if (s.rows.empty()) {
        fprintf(out, "ui-scan: no UI rows found (root=%s)\n", app_path);
        return 1;
    }
    mkdirs(dir);
    write_all(join(dir, "ui.tsv"), emit_tsv(s));
    write_all(join(dir, "ui.node"), emit_nodes(s));
    write_all(join(dir, "ui.cpp"), emit_cpp(s));
    write_all(join(dir, "ui.hex"), emit_hex(s));

    std::string meta;
    meta += "app\t" + s.app + "\n";
    meta += "asar\t" + asar + "\n";
    meta += "bytes\t" + std::to_string(s.bytes) + "\n";
    meta += "files\t" + std::to_string(s.files) + "\n";
    meta += "nodes\t" + std::to_string(s.rows.size()) + "\n";
    meta += "links\t" + std::to_string(s.links.size()) + "\n";
    meta += "scan\t" + now_iso() + "\n";
    write_all(join(dir, "ui.meta.tsv"), meta);

    fprintf(out, "UI_SCAN\t%s\n", dir.c_str());
    fprintf(out, "UI_APP\t%s\n", s.app.c_str());
    fprintf(out, "UI_ASAR\t%s\n", asar.empty() ? "-" : asar.c_str());
    fprintf(out, "UI_FILES\t%d\n", s.files);
    fprintf(out, "UI_BYTES\t%zu\n", s.bytes);
    fprintf(out, "UI_NODES\t%zu\n", s.rows.size());
    fprintf(out, "UI_LINKS\t%zu\n", s.links.size());
    for (const char *k : {"action", "handler", "command", "state", "persist", "keybinding", "module"}) {
        int n = 0;
        for (const Row &r : s.rows) if (r.kind == k) n++;
        fprintf(out, "UI_%s\t%d\n", k, n);
    }
    return 0;
}

extern "C" int nx_ui_flow(const char *dir, const char *app, const char *action,
                          const char *match, const char *format, int depth, FILE *out) {
    (void)depth;
    if (!out) out = stdout;
    std::string d = dir && *dir ? dir : "";
    if (d.empty() && app && *app) {
        std::string base = join(home(), ".neodax/apps");
        DIR *dp = opendir(base.c_str());
        if (dp) {
            struct dirent *e;
            while ((e = readdir(dp)) != NULL) {
                if (e->d_name[0] == '.') continue;
                std::string p = join(base, e->d_name);
                std::ifstream mf(join(p, "ui.meta.tsv"));
                std::string line;
                while (std::getline(mf, line)) {
                    if (line.rfind("app\t", 0) == 0 && line.find(app) != std::string::npos) {
                        d = p;
                        break;
                    }
                }
                if (!d.empty()) break;
            }
            closedir(dp);
        }
        if (d.empty()) {
            d = default_dir(app);
            if (!exists(join(d, "ui.tsv"))) d.clear();
        }
    }
    if (d.empty()) { fprintf(out, "ui-flow: --dir or --app required\n"); return 2; }
    return run_flow(d, action ? action : "", match ? match : "", format ? format : "tsv", out);
}
