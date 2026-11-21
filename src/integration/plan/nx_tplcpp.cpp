#include "nx_tplcpp.hpp"

#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <functional>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

struct RawNode {
    std::string name, label, run, layout, annulus, element, action;
    std::string file;
    int line = 0;
};

struct RawLink {
    std::string a, kind, b;
};

struct RawTpl {
    std::string id, kind, target, graph;
    std::vector<RawNode> nodes;
    std::vector<RawLink> links;
    std::string file;
};

std::string trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) b++;
    while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

bool starts_with(const std::string &s, const char *p) {
    return s.rfind(p, 0) == 0;
}

/* "node: name label ... run: cmd" を分解する。run:/layout:/annulus: は
   ラベルから切り離す (ラベルは表示用の説明)。 */
void parse_node(const std::string &v, RawNode &n) {
    std::string rest = trim(v);
    size_t sp = rest.find_first_of(" \t");
    n.name = sp == std::string::npos ? rest : rest.substr(0, sp);
    rest = sp == std::string::npos ? "" : trim(rest.substr(sp + 1));

    std::string label;
    std::istringstream is(rest);
    std::string tok;
    while (is >> tok) {
        std::string val;
        bool ok = false;
        const char *keys[] = {"run:", "layout:", "annulus:", "element:", "action:"};
        for (const char *k : keys) {
            if (starts_with(tok, k)) {
                val = tok.substr(std::strlen(k));
                if (val.empty() && is >> val) {}
                if (starts_with(tok, "run:")) n.run = val;
                else if (starts_with(tok, "layout:")) n.layout = val;
                else if (starts_with(tok, "element:")) n.element = val;
                else if (starts_with(tok, "action:")) n.action = val;
                else n.annulus = val;
                ok = true;
                break;
            }
        }
        if (!ok) { if (!label.empty()) label += " "; label += tok; }
    }
    n.label = label;
}

bool parse_block(const std::string &text, const std::string &file, RawTpl &t) {
    size_t b = text.find("===== NX TEMPLATE =====");
    if (b == std::string::npos) return false;
    size_t e = text.find("/NX TEMPLATE", b);
    if (e == std::string::npos) return false;

    int line = 1;
    for (size_t i = 0; i < b; i++) if (text[i] == '\n') line++;

    std::istringstream is(text.substr(b, e - b));
    std::string ln;
    while (std::getline(is, ln)) {
        std::string s = trim(ln);
        if (starts_with(s, "//")) s = trim(s.substr(2));
        if (s.empty()) continue;
        size_t c = s.find(':');
        if (c == std::string::npos) continue;
        std::string key = trim(s.substr(0, c));
        std::string val = trim(s.substr(c + 1));

        if (key == "id") t.id = val;
        else if (key == "kind") t.kind = val;
        else if (key == "target") t.target = val;
        else if (key == "learn") {
            if (starts_with(val, "graph=")) t.graph = val.substr(6);
            else t.graph = val;
        } else if (key == "node") {
            RawNode n; n.file = file; n.line = line;
            parse_node(val, n);
            if (!n.name.empty()) t.nodes.push_back(n);
        } else if (key == "link") {
            std::istringstream ls(val);
            RawLink l;
            if (ls >> l.a >> l.kind >> l.b) t.links.push_back(l);
        }
        line++;
    }
    t.file = file;
    /* 部分資産を受け入れる。
       以前は id: が無いと捨てていたが、部分資産 (ノードを 1 つだけ
       書いたもの) は他と組み合わせて使う前提なので、捨てない。
       id が無ければファイル名から作る (同じファイルなら同じ id)。
       何も書いていない (ノードも link も無い) ものだけを捨てる。 */
    if (t.nodes.empty() && t.links.empty()) return false;
    if (t.id.empty()) {
        size_t sl = file.find_last_of("/\\");
        std::string base = (sl == std::string::npos) ? file : file.substr(sl + 1);
        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) base = base.substr(0, dot);
        if (base.empty()) base = "partial";
        t.id = base;
    }
    return true;
}

std::string learning_path() {
    const char *home = std::getenv("HOME");
    if (!home || !*home) home = ".";
    return std::string(home) + "/.neodax/tpl-learning.tsv";
}

/* graph -> branch ごとの成否回数。ng が 2 回以上なら枝刈り。 */
struct Learn {
    std::map<std::string, std::pair<int, int>> m; /* key -> (ok, ng) */
    bool pruned(const std::string &k) const {
        auto it = m.find(k);
        return it != m.end() && it->second.second >= 2 && it->second.second > it->second.first;
    }
};

Learn load_learning() {
    Learn l;
    std::ifstream f(learning_path());
    std::string a, b, c;
    int d;
    while (f >> a >> b >> c >> d) {
        /* 学習は「グラフ単位」(= 1 つの永続手続き) に寄せる。
           個々の scratch 由来の枝は b/c に出るが、判定はグラフで行う。 */
        const std::string k = a + "/*";
        if (auto it = l.m.find(k); it != l.m.end()) {
            if (d > 0) it->second.first += d; else it->second.second += -d;
        } else {
            l.m[k] = d > 0 ? std::make_pair(d, 0) : std::make_pair(0, -d);
        }
    }
    return l;
}

int find_node(const std::vector<std::string> &names, const std::string &n) {
    for (size_t i = 0; i < names.size(); i++) if (names[i] == n) return (int)i;
    return -1;
}

/* ノード id だけで探す。部分資産をまたいで link を張るときに使う。 */
int find_node_by_id(const std::vector<nx_tpl_node_t> &nodes, const char *id) {
    for (size_t i = 0; i < nodes.size(); i++)
        if (std::strcmp(nodes[i].id, id) == 0) return (int)i;
    return -1;
}

/* 同じファイルの名前を優先し、無ければ全体から探す。
   部分資産は「persist を宣言せずに persist を指す link を書ける」
   ので、名前だけで引ける必要がある。 */
int find_node_any(const std::vector<std::string> &names, const std::string &own_id,
                  const std::string &node) {
    int local = find_node(names, own_id + "::" + node);
    if (local >= 0) return local;
    const std::string suffix = "::" + node;
    for (size_t i = 0; i < names.size(); i++) {
        const std::string &nm = names[i];
        if (nm.size() >= suffix.size() &&
            nm.compare(nm.size() - suffix.size(), suffix.size(), suffix) == 0)
            return (int)i;
    }
    return -1;
}

/* Tarjan SCC。同じ ring = 同じ annulus (同時に入れ替え可能なまとまり)。 */
void scc(const std::vector<std::vector<int>> &adj, std::vector<int> &ring, int &nring) {
    int n = (int)adj.size();
    std::vector<int> idx(n, -1), low(n, 0), st;
    std::vector<char> on(n, 0);
    ring.assign(n, -1);
    nring = 0;
    int counter = 0;

    std::function<void(int)> dfs = [&](int v) {
        idx[v] = low[v] = counter++;
        st.push_back(v); on[v] = 1;
        for (int w : adj[v]) {
            if (idx[w] < 0) { dfs(w); low[v] = std::min(low[v], low[w]); }
            else if (on[w]) low[v] = std::min(low[v], idx[w]);
        }
        if (low[v] == idx[v]) {
            while (true) {
                int w = st.back(); st.pop_back(); on[w] = 0;
                ring[w] = nring;
                if (w == v) break;
            }
            nring++;
        }
    };
    for (int i = 0; i < n; i++) if (idx[i] < 0) dfs(i);
}

std::string esc(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

} // namespace

extern "C" const char *nx_tplcpp_default_dir(void) {
    static std::string d;
    const char *home = std::getenv("HOME");
    d = std::string(home && *home ? home : ".") + "/.neodax/templates";
    return d.c_str();
}

/* RawTpl の集合から 1 つの graph を組む。
   scan (ディレクトリ) も open (cpp/svg/node) もここを通る。
   組む処理を 2 度書かない。 */
static int build_graph(std::vector<RawTpl> &tpls, const std::string &graph_name,
                       nx_tpl_graph_t *out);

extern "C" int nx_tplcpp_scan(const char *dir, nx_tpl_graph_t *out) {
    if (!out) return -1;
    std::memset(out, 0, sizeof(*out));
    const char *dd = dir && *dir ? dir : nx_tplcpp_default_dir();
    std::string dstr = dd ? dd : "";

    std::error_code ec;
    if (!fs::is_directory(dstr, ec)) {
        /* 既定に無ければリポジトリの同梱テンプレートを見る。 */
        const char *fb[] = {"src/templates", "templates", "../src/templates"};
        bool found = false;
        for (const char *p : fb) {
            if (fs::is_directory(p, ec)) { dstr = p; found = true; break; }
        }
        if (!found) return -1;
    }
    const char *d = dstr.c_str();

    std::vector<RawTpl> tpls;
    for (const auto &ent : fs::directory_iterator(d, ec)) {
        if (ec) break;
        if (!ent.is_regular_file()) continue;
        if (ent.path().extension() != ".cpp") continue;
        std::ifstream f(ent.path());
        if (!f) continue;
        std::stringstream ss; ss << f.rdbuf();
        RawTpl t;
        if (parse_block(ss.str(), ent.path().string(), t)) tpls.push_back(t);
    }
    if (tpls.empty()) return -1;
    return build_graph(tpls, "", out);
}

/* 実際に graph を組む部分。scan と open の両方がここを通る。 */
static int build_graph(std::vector<RawTpl> &tpls, const std::string &graph_name,
                       nx_tpl_graph_t *out) {
    if (tpls.empty()) return -1;

    Learn learn = load_learning();

    std::vector<std::string> names;
    std::vector<nx_tpl_node_t> nodes;
    std::vector<std::string> graph_ids;

    for (auto &t : tpls) {
        graph_ids.push_back(t.graph.empty() ? t.id : t.graph);
        for (auto &n : t.nodes) {
            nx_tpl_node_t nn;
            std::memset(&nn, 0, sizeof(nn));
            std::snprintf(nn.id, sizeof(nn.id), "%s", n.name.c_str());
            std::snprintf(nn.kind, sizeof(nn.kind), "%s%s%s",
                          n.layout.empty() ? "" : "layout",
                          n.run.empty() ? "" : (n.layout.empty() ? "cmd" : "+cmd"),
                          (n.layout.empty() && n.run.empty()) ? "step" : "");
            std::snprintf(nn.target, sizeof(nn.target), "%s", t.target.c_str());
            std::snprintf(nn.label, sizeof(nn.label), "%s", n.label.c_str());
            std::snprintf(nn.run, sizeof(nn.run), "%s", n.run.c_str());
            std::snprintf(nn.element, sizeof(nn.element), "%s", n.element.c_str());
            std::snprintf(nn.action, sizeof(nn.action), "%s", n.action.c_str());
            std::snprintf(nn.file, sizeof(nn.file), "%s", n.file.c_str());
            nn.line = n.line;
            /* 同名ノードは 1 つに寄せる (部分資産が同じ persist を
               宣言していても増やさない)。 */
            if (find_node(names, t.id + "::" + n.name) >= 0 ||
                find_node_by_id(nodes, n.name.c_str()) >= 0)
                continue;
            names.push_back(t.id + "::" + n.name);
            nodes.push_back(nn);
        }
    }

    std::vector<nx_tpl_edge_t> edges;
    std::vector<std::string> edge_branch;
    std::vector<std::vector<int>> adj(nodes.size());
    for (size_t t = 0; t < tpls.size(); t++) {
        auto &tp = tpls[t];
        for (auto &l : tp.links) {
            /* 部分資産を組み合わせるので、link の端点が同じファイルに
               無いことが普通にある。なので「ファイル内で」ではなく、
               「全体で」名前を探す。見つかった最初のノードを使う。 */
            int a = find_node_any(names, tp.id, l.a);
            int b = find_node_any(names, tp.id, l.b);
            if (a < 0 || b < 0) continue;
            nx_tpl_edge_t e;
            std::memset(&e, 0, sizeof(e));
            e.from = a; e.to = b;
            std::snprintf(e.kind, sizeof(e.kind), "%s", l.kind.c_str());
            const std::string branch = graph_ids[t];
            e.pruned = learn.pruned(branch + "/*") ||
                       learn.pruned(branch + "/" + l.a + "/" + l.kind) ||
                       learn.pruned(branch + "/" + l.b + "/" + l.kind);
            edges.push_back(e);
            edge_branch.push_back(branch);
            adj[a].push_back(b);
            adj[b].push_back(a);
        }
    }

    /* 実行順: requires は「a が b を必要とする」= b を先に。 */
    std::vector<std::vector<int>> need(nodes.size());
    std::vector<int> indeg(nodes.size(), 0);
    for (auto &e : edges) {
        if (std::strcmp(e.kind, "requires") == 0) {
            need[e.to].push_back(e.from); /* e.to の後に e.from */
            indeg[e.from]++;
        } else if (std::strcmp(e.kind, "converges") == 0) {
            need[e.from].push_back(e.to); /* e.from の後に e.to (収束) */
            indeg[e.to]++;
        }
    }
    std::vector<int> order;
    std::vector<int> q;
    for (size_t i = 0; i < nodes.size(); i++) if (indeg[i] == 0) q.push_back((int)i);
    while (!q.empty()) {
        int v = q.back(); q.pop_back();
        order.push_back(v);
        for (int w : need[v]) if (--indeg[w] == 0) q.push_back(w);
    }
    for (size_t i = 0; i < nodes.size(); i++) {
        if (std::find(order.begin(), order.end(), (int)i) == order.end())
            order.push_back((int)i); /* 循環はそのまま末尾 (annulus で可視化) */
    }

    /* annulus: requires の深さ (detect=0 → prune=1 → persist=2 → live=3)。
       同じ ring = 同じ段。app 側の表示を置き換えず、段の写像として使う。 */
    int nring = 0;
    std::vector<int> ring(nodes.size(), 0);
    for (int guard = 0; guard < 64; guard++) {
        bool changed = false;
        for (auto &e : edges) {
            int outer = -1, inner = -1;
            if (std::strcmp(e.kind, "requires") == 0) { outer = e.from; inner = e.to; }
            else if (std::strcmp(e.kind, "converges") == 0) { outer = e.from; inner = e.to; }
            else continue;
            if (ring[outer] < ring[inner] + 1) {
                ring[outer] = ring[inner] + 1;
                changed = true;
            }
        }
        if (!changed) break;
    }
    for (size_t i = 0; i < nodes.size(); i++) nring = std::max(nring, ring[i] + 1);
    (void)adj;

    out->nodes = (nx_tpl_node_t *)std::calloc(nodes.size(), sizeof(nx_tpl_node_t));
    out->edges = (nx_tpl_edge_t *)std::calloc(edges.size(), sizeof(nx_tpl_edge_t));
    out->order = (int *)std::calloc(order.size(), sizeof(int));
    out->ring = (int *)std::calloc(ring.size(), sizeof(int));
    if (!out->nodes || !out->edges || !out->order || !out->ring) {
        nx_tplcpp_free(out);
        return -1;
    }
    std::memcpy(out->nodes, nodes.data(), nodes.size() * sizeof(nx_tpl_node_t));
    std::memcpy(out->edges, edges.data(), edges.size() * sizeof(nx_tpl_edge_t));
    std::memcpy(out->order, order.data(), order.size() * sizeof(int));
    std::memcpy(out->ring, ring.data(), ring.size() * sizeof(int));
    out->n_nodes = nodes.size();
    out->n_edges = edges.size();
    out->n_order = order.size();
    out->n_rings = nring;
    std::snprintf(out->graph, sizeof(out->graph), "%s",
                  !graph_name.empty() ? graph_name.c_str()
                  : (graph_ids.empty() ? "" : graph_ids[0].c_str()));
    return 0;
}


/* ------------------------------------------------------------------ */
/* 中核の入口: cpp / svg / node を auto で受ける                      */
/* ------------------------------------------------------------------ */

/* なぜ必要か:

   呼び側が「これは cpp だ」「これは svg だ」と先に判定して、
   それに合った関数を選ぶ形にすると、種類が増えるたびに呼び側が増える。
   中核では「渡されたものを読む」だけにしたい。種類の判定は中でやる。

   受けるもの:
     dir     ディレクトリ          → 中の資産を全部読む (部分資産も可)
     .cpp    資産                  → ブロックを読む
     .svg    図                    → 図からノードと矢印を拾って構成にする
     .tsv/.txt/.node               → 1 行 1 ノード/リンクとして読む
     (文字列そのもの)              → 中身を見て判定する
*/
extern "C" const char *nx_tplcpp_kind_name(nx_tpl_src_kind_t k) {
    switch (k) {
        case NX_TPL_SRC_DIR:  return "dir";
        case NX_TPL_SRC_CPP:  return "cpp";
        case NX_TPL_SRC_SVG:  return "svg";
        case NX_TPL_SRC_NODE: return "node";
        case NX_TPL_SRC_HEX:  return "hex";
        default:              return "none";
    }
}

/* 拡張子で判定する。拡張子が無い/知らないなら中身で判定する。 */
static nx_tpl_src_kind_t kind_by_ext(const std::string &s) {
    auto ends = [&](const char *e) {
        size_t n = std::strlen(e);
        return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
    };
    if (ends(".cpp") || ends(".cc") || ends(".hpp")) return NX_TPL_SRC_CPP;
    if (ends(".svg")) return NX_TPL_SRC_SVG;
    if (ends(".tsv") || ends(".txt") || ends(".node")) return NX_TPL_SRC_NODE;
    if (ends(".hex")) return NX_TPL_SRC_HEX;
    return NX_TPL_SRC_NONE;
}

/* 中身で判定する。 */
static nx_tpl_src_kind_t kind_by_content(const std::string &s) {
    if (s.find("===== NX TEMPLATE =====") != std::string::npos) return NX_TPL_SRC_CPP;
    if (s.find("<svg") != std::string::npos) return NX_TPL_SRC_SVG;
    /* node 形式: "node<TAB>id<TAB>label" か "node id label" の行 */
    if (s.find("node ") != std::string::npos || s.find("node\t") != std::string::npos)
        return NX_TPL_SRC_NODE;
    if (s.find("link ") != std::string::npos || s.find("link\t") != std::string::npos)
        return NX_TPL_SRC_NODE;
    if (s.find("# function ") != std::string::npos)
        return NX_TPL_SRC_HEX;
    return NX_TPL_SRC_NONE;
}

extern "C" nx_tpl_src_kind_t nx_tplcpp_probe(const char *src) {
    if (!src || !*src) return NX_TPL_SRC_NONE;
    std::error_code ec;
    if (fs::is_directory(src, ec)) return NX_TPL_SRC_DIR;
    nx_tpl_src_kind_t k = kind_by_ext(src);
    if (k != NX_TPL_SRC_NONE) return k;
    std::ifstream f(src, std::ios::binary);
    if (f) {
        std::stringstream ss; ss << f.rdbuf();
        return kind_by_content(ss.str());
    }
    /* ファイルでもない → 中身そのものとして見る */
    return kind_by_content(src);
}

/* svg からノードと矢印を拾う。
   我々が出す svg (nx_tplcpp_svg) は円に id、線に from→to を持つので、
   それを読み戻せる。読めなければ 0 件で返す (捏造しない)。 */
static void parse_svg(const std::string &s, RawTpl &t) {
    /* <circle ... data-id="node"> と <text ...>id</text> の組を拾う。 */
    auto attr = [](const std::string &line, const char *name) -> std::string {
        std::string key = std::string(name) + "=\"";
        size_t p = line.find(key);
        if (p == std::string::npos) return "";
        p += key.size();
        size_t e = line.find('"', p);
        return e == std::string::npos ? "" : line.substr(p, e - p);
    };
    std::istringstream is(s);
    std::string ln;
    while (std::getline(is, ln)) {
        if (ln.find("<rect") != std::string::npos || ln.find("<circle") != std::string::npos) {
            std::string id = attr(ln, "data-id");
            if (id.empty()) id = attr(ln, "id");
            if (!id.empty()) {
                RawNode n; n.name = id; n.label = attr(ln, "data-label");
                n.element = attr(ln, "data-element");
                n.action = attr(ln, "data-action");
                t.nodes.push_back(n);
            }
        } else if (ln.find("<line") != std::string::npos) {
            std::string a = attr(ln, "data-from"), b = attr(ln, "data-to");
            std::string k = attr(ln, "data-kind");
            if (!a.empty() && !b.empty()) {
                RawLink l; l.a = a; l.b = b; l.kind = k.empty() ? "next" : k;
                t.links.push_back(l);
            }
        }
    }
}

/* node 形式 (TSV / 空白区切り) を読む。
     node <id> [label...]
     link <from> <kind> <to>                                     */
static void parse_node_lines(const std::string &s, RawTpl &t) {
    std::istringstream is(s);
    std::string ln;
    while (std::getline(is, ln)) {
        std::string c = trim(ln);
        if (c.empty() || c[0] == '#') continue;
        std::istringstream ls(c);
        std::string kw; ls >> kw;
        if (kw == "node") {
            RawNode n;
            parse_node(c.substr(5), n);
            if (!n.name.empty()) t.nodes.push_back(n);
        } else if (kw == "link") {
            RawLink l;
            if (ls >> l.a >> l.kind >> l.b) t.links.push_back(l);
        }
    }
}

static void parse_hex(const std::string &s, RawTpl &t) {
    std::istringstream is(s);
    std::string ln, prev;
    std::set<std::string> seen;
    bool has_functions = false;
    auto add = [&](const std::string &name, const std::string &label) {
        if (name.empty() || !seen.insert(name).second) return;
        RawNode n; n.name = name; n.label = label;
        t.nodes.push_back(n);
        if (!prev.empty()) {
            RawLink l; l.a = prev; l.kind = "next"; l.b = name;
            t.links.push_back(l);
        }
        prev = name;
    };
    while (std::getline(is, ln)) {
        std::string c = trim(ln);
        if (c.empty()) continue;
        if (starts_with(c, "# function ")) {
            has_functions = true;
            std::istringstream ls(c.substr(11));
            std::string addr, name;
            ls >> addr >> name;
            std::string id = "fn_" + addr;
            for (char &ch : id) if (!std::isalnum((unsigned char)ch)) ch = '_';
            add(id, c.substr(2));
            continue;
        }
        if (c[0] == '#') continue;
        std::istringstream ls(c);
        std::string addr;
        ls >> addr;
        if (addr.size() < 4) continue;
        if (addr.back() == ':') addr.pop_back();
        bool hex = addr.rfind("0x", 0) == 0 || addr.rfind("0X", 0) == 0;
        for (char ch : addr) if (std::isxdigit((unsigned char)ch)) hex = true;
        if (!hex) continue;
        if (has_functions) continue;
        std::string id = "hex_" + addr;
        for (char &ch : id) if (!std::isalnum((unsigned char)ch)) ch = '_';
        add(id, c);
    }
}

extern "C" int nx_tplcpp_open(const char *src, nx_tpl_graph_t *out,
                              nx_tpl_src_kind_t *kind) {
    if (!out) return -1;
    if (kind) *kind = NX_TPL_SRC_NONE;
    std::memset(out, 0, sizeof(*out));
    /* src が無いときは既定の置き場所を見る。scan と同じ振る舞い。 */
    if (!src || !*src) {
        if (kind) *kind = NX_TPL_SRC_DIR;
        return nx_tplcpp_scan(NULL, out);
    }

    nx_tpl_src_kind_t k = nx_tplcpp_probe(src);
    if (kind) *kind = k;

    /* ディレクトリは既存の scan に任せる (部分資産を全部読む)。 */
    if (k == NX_TPL_SRC_DIR) return nx_tplcpp_scan(src, out);

    std::string body;
    std::string name = src;
    std::error_code ec;
    if (fs::is_regular_file(src, ec)) {
        std::ifstream f(src, std::ios::binary);
        if (!f) return -1;
        std::stringstream ss; ss << f.rdbuf();
        body = ss.str();
    } else {
        body = src;  /* 中身そのもの */
    }

    std::vector<RawTpl> tpls;
    RawTpl t;
    t.file = name;
    if (k == NX_TPL_SRC_CPP) {
        if (!parse_block(body, name, t)) return -1;
        tpls.push_back(t);
    } else if (k == NX_TPL_SRC_SVG) {
        parse_svg(body, t);
        if (t.nodes.empty()) return -1;
        if (t.id.empty()) t.id = "svg";
        tpls.push_back(t);
    } else if (k == NX_TPL_SRC_NODE) {
        parse_node_lines(body, t);
        if (t.nodes.empty()) return -1;
        if (t.id.empty()) t.id = "node";
        tpls.push_back(t);
    } else if (k == NX_TPL_SRC_HEX) {
        parse_hex(body, t);
        if (t.nodes.empty()) return -1;
        if (t.id.empty()) t.id = "hex";
        tpls.push_back(t);
    } else {
        return -1;
    }

    return build_graph(tpls, t.id, out);
}
extern "C" void nx_tplcpp_free(nx_tpl_graph_t *g) {
    if (!g) return;
    std::free(g->nodes); std::free(g->edges);
    std::free(g->order); std::free(g->ring);
    std::memset(g, 0, sizeof(*g));
}

extern "C" int nx_tplcpp_dump(const nx_tpl_graph_t *g, FILE *out) {
    if (!g || !out) return -1;
    std::fprintf(out, "graph=%s nodes=%zu edges=%zu rings=%d\n",
                 g->graph, g->n_nodes, g->n_edges, g->n_rings);
    std::fprintf(out, "order:");
    for (size_t i = 0; i < g->n_order; i++)
        std::fprintf(out, " %s", g->nodes[g->order[i]].id);
    std::fprintf(out, "\n");
    for (size_t i = 0; i < g->n_nodes; i++) {
        const nx_tpl_node_t &n = g->nodes[i];
        std::fprintf(out, "  %-18s ring=%d kind=%-8s run=%-10s element=%-12s action=%-12s %s\n",
                     n.id, g->ring ? g->ring[i] : -1, n.kind, n.run,
                     n.element[0] ? n.element : "-", n.action[0] ? n.action : "-", n.label);
    }
    for (size_t i = 0; i < g->n_edges; i++) {
        const nx_tpl_edge_t &e = g->edges[i];
        std::fprintf(out, "  %s -%s-> %s%s\n", g->nodes[e.from].id,
                     e.kind, g->nodes[e.to].id, e.pruned ? "  [pruned]" : "");
    }
    return 0;
}

extern "C" int nx_tplcpp_svg(const nx_tpl_graph_t *g, const char *path, FILE *out) {
    if (!g || !path) return -1;
    /* app の表示レイアウトを「置き換えない」。ここで作るのは解析用の
       1 枚の SVG で、アプリには注入しない。ring は app 側の annulus
       (同時に差し替え可能なノード群) の写像にすぎない。 */
    FILE *f = std::fopen(path, "wb");
    if (!f) return -1;

    int W = 900, H = 700;
    std::fprintf(f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    std::fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" "
                    "viewBox=\"0 0 %d %d\">\n", W, H, W, H);
    std::fprintf(f, "<rect width=\"100%%\" height=\"100%%\" fill=\"#0f1115\"/>\n");

    int cx = W / 2, cy = H / 2;
    int rings = g->n_rings > 0 ? g->n_rings : 1;
    std::vector<std::vector<int>> by_ring(rings);
    for (size_t i = 0; i < g->n_nodes; i++)
        by_ring[g->ring ? g->ring[i] : 0].push_back((int)i);

    std::vector<double> nx(g->n_nodes), ny(g->n_nodes);
    for (int r = 0; r < rings; r++) {
        double rad = 90 + r * 70;
        std::fprintf(f, "<circle cx=\"%d\" cy=\"%d\" r=\"%.0f\" fill=\"none\" "
                        "stroke=\"#2a3140\" stroke-width=\"1\"/>\n", cx, cy, rad);
        size_t m = by_ring[r].size();
        for (size_t k = 0; k < m; k++) {
            double a = -M_PI / 2 + 2 * M_PI * k / (m ? m : 1);
            int v = by_ring[r][k];
            nx[v] = cx + rad * std::cos(a);
            ny[v] = cy + rad * std::sin(a);
        }
    }

    for (size_t i = 0; i < g->n_edges; i++) {
        const nx_tpl_edge_t &e = g->edges[i];
        std::fprintf(f, "<line x1=\"%.0f\" y1=\"%.0f\" x2=\"%.0f\" y2=\"%.0f\" "
                        "stroke=\"%s\" stroke-width=\"1.2\" "
                        "data-from=\"%s\" data-to=\"%s\" data-kind=\"%s\"/>",
                     nx[e.from], ny[e.from], nx[e.to], ny[e.to],
                     e.pruned ? "#5a3030" : "#3f6d8f",
                     esc(g->nodes[e.from].id).c_str(),
                     esc(g->nodes[e.to].id).c_str(),
                     esc(e.kind).c_str());
        if (std::strcmp(e.kind, "requires") != 0)
            std::fprintf(f, "<text x=\"%.0f\" y=\"%.0f\" fill=\"#6b7a90\" "
                            "font-size=\"9\">%s</text>",
                         (nx[e.from] + nx[e.to]) / 2, (ny[e.from] + ny[e.to]) / 2 - 3, e.kind);
        std::fprintf(f, "\n");
    }

    for (size_t i = 0; i < g->n_nodes; i++) {
        const nx_tpl_node_t &n = g->nodes[i];
        bool layout = std::strstr(n.kind, "layout") != nullptr;
        std::fprintf(f, "<rect data-id=\"%s\" data-label=\"%s\" data-element=\"%s\" "
                        "data-action=\"%s\" x=\"%.0f\" y=\"%.0f\" "
                        "width=\"12\" height=\"12\" fill=\"%s\"/>\n",
                     esc(n.id).c_str(), esc(n.label).c_str(),
                     esc(n.element).c_str(), esc(n.action).c_str(),
                     nx[i] - 6, ny[i] - 6,
                     layout ? "#c9a227" : "#4c9be8");
        std::fprintf(f, "<text x=\"%.0f\" y=\"%.0f\" fill=\"#c8d2e0\" font-size=\"10\">%s</text>\n",
                     nx[i] + 9, ny[i] + 4, esc(n.id).c_str());
    }
    std::fprintf(f, "<text x=\"16\" y=\"24\" fill=\"#c8d2e0\" font-size=\"14\">%s</text>\n",
                 esc(g->graph).c_str());
    std::fprintf(f, "</svg>\n");
    std::fclose(f);
    if (out) std::fprintf(out, "svg: %s (analysis only; app layout is not replaced)\n", path);
    return 0;
}

extern "C" int nx_tplcpp_record(const nx_tpl_graph_t *g, const char *branch, int ok, FILE *out) {
    if (!g || !branch) return -1;
    const char *home = std::getenv("HOME");
    std::string dir = std::string(home && *home ? home : ".") + "/.neodax";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f(learning_path(), std::ios::app);
    if (!f) return -1;
    /* graph<TAB>branch<TAB>step<TAB>delta  (* はグラフ全体の成否) */
    f << g->graph << "\t*\t*\t" << (ok ? 1 : -1) << "\n";
    if (out) std::fprintf(out, "learn: %s/%s %s\n", g->graph, branch, ok ? "ok" : "ng");
    return 0;
}
