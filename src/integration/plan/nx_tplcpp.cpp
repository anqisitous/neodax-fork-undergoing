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
    std::string name, label, run, layout, annulus;
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
        const char *keys[] = {"run:", "layout:", "annulus:"};
        for (const char *k : keys) {
            if (starts_with(tok, k)) {
                val = tok.substr(std::strlen(k));
                if (val.empty() && is >> val) {}
                if (starts_with(tok, "run:")) n.run = val;
                else if (starts_with(tok, "layout:")) n.layout = val;
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
    return !t.id.empty();
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
            std::snprintf(nn.file, sizeof(nn.file), "%s", n.file.c_str());
            nn.line = n.line;
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
            int a = find_node(names, tp.id + "::" + l.a);
            int b = find_node(names, tp.id + "::" + l.b);
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
                  graph_ids.empty() ? "" : graph_ids[0].c_str());
    return 0;
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
        std::fprintf(out, "  %-18s ring=%d kind=%-8s run=%-10s %s\n",
                     n.id, g->ring ? g->ring[i] : -1, n.kind, n.run, n.label);
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
                        "stroke=\"%s\" stroke-width=\"1.2\"/>",
                     nx[e.from], ny[e.from], nx[e.to], ny[e.to],
                     e.pruned ? "#5a3030" : "#3f6d8f");
        if (std::strcmp(e.kind, "requires") != 0)
            std::fprintf(f, "<text x=\"%.0f\" y=\"%.0f\" fill=\"#6b7a90\" "
                            "font-size=\"9\">%s</text>",
                         (nx[e.from] + nx[e.to]) / 2, (ny[e.from] + ny[e.to]) / 2 - 3, e.kind);
        std::fprintf(f, "\n");
    }

    for (size_t i = 0; i < g->n_nodes; i++) {
        const nx_tpl_node_t &n = g->nodes[i];
        bool layout = std::strstr(n.kind, "layout") != nullptr;
        std::fprintf(f, "<rect x=\"%.0f\" y=\"%.0f\" width=\"12\" height=\"12\" "
                        "fill=\"%s\"/>\n", nx[i] - 6, ny[i] - 6,
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
