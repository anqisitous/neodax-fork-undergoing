// nx_graph.cpp — グラフモデルの実装 (AppKit 非依存)
//
// 多相の実体はすべてここに置く。viewer (.mm) には分岐を書かない。

#include "nx_graph.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace nx {

// ------------------------------------------------------------------ Layout

float RadialLayout::arrange(std::vector<Node *> &m, Point c) const {
    const float PI2 = 6.2831853f;
    const size_t n = m.size();
    if (n == 0) return 0.0f;
    if (n == 1) return SingleLayout().arrange(m, c);
    const float rad = std::max(1.1f, (float)n / PI2);
    for (size_t k = 0; k < n; k++) {
        const float a = PI2 * (float)k / (float)n + 0.5f;
        m[k]->pos.x = c.x + std::cos(a) * rad;
        m[k]->pos.y = c.y + std::sin(a) * rad;
    }
    return rad;
}

float SingleLayout::arrange(std::vector<Node *> &m, Point c) const {
    if (m.empty()) return 0.0f;
    m[0]->pos = c;
    return 0.0f;
}

const Layout *layout_for(size_t n) {
    static const RadialLayout radial;
    static const SingleLayout single;
    return n > 1 ? static_cast<const Layout *>(&radial)
                 : static_cast<const Layout *>(&single);
}

// ------------------------------------------------------------------ Graph

bool Graph::is_state_dependent_name(const std::string &s) {
    // "wait" と "spawn" は同じ「状態で動く/動かないを決める線」として扱う。
    // 注意: "Block" はコードブロックの意味でも使われるため入れない。
    //       小文字 "lock" は "block" に部分一致するため入れない。
    static const char *pat[] = {
        "Wait", "wait", "Sleep", "sleep", "Join", "join", "Semaphore", "semaphore",
        "CondWait", "cond_wait", "pthread_cond", "pthread_mutex", "RunLoop", "RunModal",
        "DispatchGroup", "WaitForProcess", "waitpid", "Mutex", "Barrier", "Gate",
        "Acquire", "AcquireLock", "Lock", "Waiter",
        "Spawn", "spawn", "Launch", "launch", "Relaunch", "relaunch",
        "Fork", "fork", "Exec", "exec", "LSOpen", "XPC",
        nullptr
    };
    for (int i = 0; pat[i]; i++)
        if (s.find(pat[i]) != std::string::npos) return true;
    return false;
}

struct Graph::Impl {
    // 所有
    std::vector<std::unique_ptr<FunctionNode>> func_own;
    std::vector<std::unique_ptr<VirtualGroupNode>> virt_nodes_own;
    std::vector<std::unique_ptr<AnnulusGroup>> annulus_own;
    std::vector<std::unique_ptr<RoleGroup>> role_own;

    // 仮想環 (RoleGroup::annuli には含めず、描画用に別に持つ)
    std::vector<AnnulusGroup *> virtual_annuli;
};

Graph::Graph() : p_(new Impl) {}
Graph::~Graph() = default;

FunctionNode *Graph::add_function(uint64_t addr, const std::string &name,
                                  int role, int annulus) {
    auto fn = std::make_unique<FunctionNode>();
    fn->addr = addr;
    fn->name = name;
    fn->role = role;
    fn->annulus = annulus;
    FunctionNode *raw = fn.get();
    p_->func_own.push_back(std::move(fn));
    funcs_.push_back(raw);
    return raw;
}

void Graph::build_hierarchy() {
    p_->annulus_own.clear();
    p_->role_own.clear();
    p_->virt_nodes_own.clear();
    p_->virtual_annuli.clear();
    annulus_ptrs_.clear();
    role_ptrs_.clear();

    // role -> annulus -> members (生成順を保つため map + vector を併用)
    std::vector<int> role_order;
    std::map<int, std::vector<int>> role_annuli;               // role -> annulus ids (出現順)
    std::map<std::pair<int, int>, std::vector<Node *>> cells; // (role,annulus) -> members

    for (FunctionNode *f : funcs_) {
        if (role_annuli.find(f->role) == role_annuli.end())
            role_order.push_back(f->role);
        auto &rr = role_annuli[f->role];
        if (std::find(rr.begin(), rr.end(), f->annulus) == rr.end())
            rr.push_back(f->annulus);
        cells[{f->role, f->annulus}].push_back(f);
    }

    // 状態依存の環を数える
    std::map<int, std::vector<int>> state_annuli;   // role -> 状態依存な annulus id
    for (auto &kv : cells) {
        bool dep = false;
        for (Node *n : kv.second)
            if (is_state_dependent_name(n->name)) { dep = true; break; }
        if (dep) state_annuli[kv.first.first].push_back(kv.first.second);
    }

    for (int role : role_order) {
        auto rg = std::make_unique<RoleGroup>();
        rg->role = role;

        for (int annulus : role_annuli[role]) {
            auto r = std::make_unique<AnnulusGroup>();
            r->role = role;
            r->annulus = annulus;
            r->members = cells[{role, annulus}];
            rg->annuli.push_back(r.get());
            p_->annulus_own.push_back(std::move(r));
        }

        // 同じ role に状態依存の環が 2 つ以上 → 仮想ノードで束ねる (階層2)
        auto sit = state_annuli.find(role);
        if (sit != state_annuli.end() && sit->second.size() >= 2) {
            auto vn = std::make_unique<VirtualGroupNode>();
            vn->role = role;
            for (int r : sit->second) vn->grouped_annuli.push_back(r);
            VirtualGroupNode *vnraw = vn.get();
            p_->virt_nodes_own.push_back(std::move(vn));

            auto vr = std::make_unique<VirtualAnnulusGroup>();
            vr->role = role;
            vr->members.push_back(vnraw);      // 仮想ノードを 1 つ抱える
            p_->virtual_annuli.push_back(vr.get());
            p_->annulus_own.push_back(std::move(vr));
        }

        role_ptrs_.push_back(rg.get());
        p_->role_own.push_back(std::move(rg));
    }

    // 描画用に、実環 + 仮想環を role 順で並べる
    for (RoleGroup *rg : role_ptrs_) {
        for (AnnulusGroup *r : rg->annuli) annulus_ptrs_.push_back(r);
        for (AnnulusGroup *vr : p_->virtual_annuli)
            if (vr->role == rg->role) annulus_ptrs_.push_back(vr);
    }
}

void Graph::layout(float wrap) {
    const float RGAPX = 3.0f, RGAPY = 3.0f, GAP = 3.0f;

    float gx = 0, gy = 0, rowH = 0;
    for (RoleGroup *rg : role_ptrs_) {
        float packX = 0, packY = 0, packRowH = 0, localW = 0;

        // 実環 + 仮想環を role ごとに配置
        std::vector<AnnulusGroup *> ordered;
        for (AnnulusGroup *r : rg->annuli) ordered.push_back(r);
        for (AnnulusGroup *vr : p_->virtual_annuli)
            if (vr->role == rg->role) ordered.push_back(vr);

        for (AnnulusGroup *r : ordered) {
            const size_t n = r->members.size();
            const float w = (n > 1) ? 2 * std::max(1.1f, (float)n / 6.2831853f) : 1.4f;
            const float h = w;

            if (packX + w > 26.0f && packX > 0) {
                packX = 0;
                packY -= packRowH + RGAPY;
                packRowH = 0;
            }
            Point center{packX + w * 0.5f, packY - h * 0.5f};
            layout_for(n)->arrange(r->members, center);

            r->box = Rect{packX, packY - h, w, h};
            packX += w + RGAPX;
            if (h > packRowH) packRowH = h;
            if (packX > localW) localW = packX;
        }

        const float localH = -(packY - packRowH);

        if (gx + localW > wrap && gx > 0) {
            gx = 0;
            gy -= rowH + GAP;
            rowH = 0;
        }

        // ローカル座標をグローバルへ
        for (AnnulusGroup *r : ordered) {
            for (Node *m : r->members) { m->pos.x += gx; m->pos.y += gy; }
            r->box.x += gx;
            r->box.y += gy;
        }
        rg->box = Rect{gx, gy - localH, localW, localH};
        gx += localW + GAP;
        if (localH > rowH) rowH = localH;
    }

    // 全体センタリング
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (FunctionNode *f : funcs_) {
        minx = std::min(minx, f->pos.x); maxx = std::max(maxx, f->pos.x);
        miny = std::min(miny, f->pos.y); maxy = std::max(maxy, f->pos.y);
    }
    for (auto &vn : p_->virt_nodes_own) {
        minx = std::min(minx, vn->pos.x); maxx = std::max(maxx, vn->pos.x);
        miny = std::min(miny, vn->pos.y); maxy = std::max(maxy, vn->pos.y);
    }
    if (minx > maxx) return;
    const float cx = (minx + maxx) * 0.5f, cy = (miny + maxy) * 0.5f;
    for (FunctionNode *f : funcs_) { f->pos.x -= cx; f->pos.y -= cy; }
    for (auto &vn : p_->virt_nodes_own) { vn->pos.x -= cx; vn->pos.y -= cy; }
    for (RoleGroup *rg : role_ptrs_) { rg->box.x -= cx; rg->box.y -= cy; }
    for (AnnulusGroup *r : annulus_ptrs_)  { r->box.x -= cx; r->box.y -= cy; }
}

std::vector<Node *> Graph::virtual_nodes() const {
    std::vector<Node *> out;
    for (auto &vn : p_->virt_nodes_own) out.push_back(vn.get());
    return out;
}

} // namespace nx
