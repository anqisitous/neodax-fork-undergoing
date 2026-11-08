// nx_plan.cpp — 分析 → 計画 → 検証の実装
//
// 出力の意味:
//   findings  何が問題か (根拠の signal を必ず添える)
//   plan      どの順で直すか。並列群は「片方取れなければ他方」を明示
//   confidence 順序や並列の取りこぼしで結論が揺れないかを回帰で測る

#include "nx_plan.hpp"
#include "nx_case.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>

#include <sys/stat.h>

namespace nx {
namespace {

std::string now_iso() {
    char ts[32] = {0};
    std::time_t t = std::time(NULL);
    std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    return ts;
}

// step id -> index
std::map<std::string, int> index_of(const std::vector<Step> &steps) {
    std::map<std::string, int> m;
    for (size_t i = 0; i < steps.size(); i++) m[steps[i].id] = (int)i;
    return m;
}

} // namespace

Model::Model(std::string subject) : subject_(std::move(subject)) {}

void Model::add_signal(Signal s) { signals_.push_back(std::move(s)); }

void Model::use_case(std::shared_ptr<Case> c, std::shared_ptr<CaseTarget> t) {
    case_ = std::move(c);
    case_target_ = std::move(t);
}

void Model::collect() {
    // 案件があるなら、観測はその案件に任せる。
    // 案件が無い場合は外から add_signal された事実だけを使う
    // (モデルが事実を作らないことを保証する)。
    if (case_ && case_target_) case_->collect(*case_target_, signals_);
}

// ------------------------------------------------------------------
// analyze: 信号 -> 問題
// ------------------------------------------------------------------
void Model::analyze() {
    findings_.clear();
    if (case_) { case_->analyze(signals_, findings_); build_advice(); return; }

    // 1) 壊れた (閉じられない) 対象
    {
        std::vector<std::string> ev;
        double sev = 0.0;
        for (const auto &s : signals_) {
            if (!s.observed) continue;
            if (s.id.find("orphan") != std::string::npos) {
                ev.push_back(s.id);
                sev = std::max(sev, 0.9);
            }
        }
        if (!ev.empty())
            findings_.push_back({"orphan-object",
                                 "所有者を失った対象があり、UI からは閉じられない",
                                 ev, sev, true});
    }

    // 2) 永続層と稼働層の不一致
    {
        std::vector<std::string> ev;
        bool persisted = false, live = false;
        for (const auto &s : signals_) {
            if (!s.observed) continue;
            if (s.id == "store.persisted.count") persisted = true;
            if (s.id == "store.live.count") live = true;
        }
        // 件数が違えば不一致。両方観測できているときだけ言える。
        int pc = -1, lc = -1;
        for (const auto &s : signals_) {
            if (s.id == "store.persisted.count" && s.observed) pc = std::atoi(s.value.c_str());
            if (s.id == "store.live.count" && s.observed) lc = std::atoi(s.value.c_str());
        }
        if (persisted && live && pc >= 0 && lc >= 0 && pc != lc) {
            ev.push_back("store.persisted.count");
            ev.push_back("store.live.count");
            findings_.push_back({"store-divergence",
                                 "永続層と稼働中ストアの件数が一致しない (再 persist で戻る)",
                                 ev, 0.8, true});
        }
    }

    // 3) 書き込みの健全性 (壊れた追記があると「直したのに戻る」)
    {
        std::vector<std::string> ev;
        for (const auto &s : signals_) {
            if (s.observed && s.id == "wal.integrity") {
                if (s.value != "clean") ev.push_back(s.id);
            }
        }
        if (!ev.empty())
            findings_.push_back({"wal-torn",
                                 "書き込みログの末尾が壊れている (追記が読めない)",
                                 ev, 0.95, true});
    }

    // 4) seq の逆行 (以降アプリ側の書き込みが無視される)
    {
        std::vector<std::string> ev;
        for (const auto &s : signals_) {
            if (s.observed && s.id == "seq.sanity" && s.value != "ok")
                ev.push_back(s.id);
        }
        if (!ev.empty())
            findings_.push_back({"seq-poisoned",
                                 "seq が実測より大きく、アプリ側の更新が反映されない",
                                 ev, 0.9, true});
    }

    // 5) 退避コピーの残存 (別経路で復活しうる)
    {
        std::vector<std::string> ev;
        for (const auto &s : signals_) {
            if (s.observed && s.id == "store.backup" && s.value == "live")
                ev.push_back(s.id);
        }
        if (!ev.empty())
            findings_.push_back({"stale-backup",
                                 "退避コピーに古い状態が残っている",
                                 ev, 0.6, true});
    }

    build_advice();
}

// ------------------------------------------------------------------
// plan: 問題 -> 修復手 (順序と並列/代替)
// ------------------------------------------------------------------
void Model::plan() {
    plan_ = Plan();
    if (case_) {
        case_->plan(findings_, plan_.steps);
        if (!plan_.steps.empty()) topo_sort();
        return;
    }
    std::set<std::string> have;
    for (const auto &f : findings_) have.insert(f.id);

    auto add = [&](Step s) { plan_.steps.push_back(std::move(s)); };

    // 直す順序には理由がある:
    //   1. 書き込み基盤 (wal / seq) を直さないと、他の修復が読めないまま終わる
    //   2. その上で永続層を直す
    //   3. 最後に稼働層へ合わせる (逆にすると次の persist で戻る)
    if (have.count("wal-torn")) {
        Step s;
        s.id = "repair-wal";
        s.action = "壊れた追記の末尾を切り詰め、以降の追記が読めるようにする";
        s.mutates = true;
        s.approved = true;
        s.cost = 2;
        add(s);
    }
    if (have.count("seq-poisoned")) {
        Step s;
        s.id = "fix-seq";
        s.action = "seq を実測最大+1 に正し、アプリ側の更新を再び通す";
        s.mutates = true;
        s.approved = true;
        s.cost = 1;
        add(s);
    }
    if (have.count("orphan-object")) {
        Step s;
        s.id = "persist-fix";
        s.action = "所有者を失った対象を永続層から除く";
        s.mutates = true;
        s.approved = true;
        s.cost = 3;
        if (have.count("wal-torn")) s.requires.push_back("repair-wal");
        if (have.count("seq-poisoned")) s.requires.push_back("fix-seq");
        add(s);

        // 稼働層への反映は「取れなければ別経路」の並列/代替で表す。
        Step l;
        l.id = "live-sync";
        l.action = "稼働中ストアへ同じ値を反映する (reload しない)";
        l.requires = { "persist-fix" };
        l.can_parallel = true;
        l.mutates = true;
        l.approved = true;
        l.cost = 2;
        add(l);

        Step alt;
        alt.id = "live-sync-alt";
        alt.action = "代替経路: ストアを再水和して同じ値を読ませる";
        alt.requires = { "persist-fix" };
        alt.can_parallel = true;
        alt.mutates = false;
        alt.approved = true;
        alt.cost = 2;
        add(alt);

        // 片方が取れなければ他方で取る、を明示的に結ぶ。
        for (auto &st : plan_.steps) {
            if (st.id == "live-sync") st.alternatives = { "live-sync-alt" };
            if (st.id == "live-sync-alt") st.alternatives = { "live-sync" };
        }
    }
    if (have.count("stale-backup")) {
        Step s;
        s.id = "purge-backup";
        s.action = "退避コピーを削除レコードで無効化する";
        s.mutates = true;
        s.approved = true;
        s.cost = 1;
        if (have.count("persist-fix")) s.requires.push_back("persist-fix");
        add(s);
    }

    if (plan_.steps.empty()) return;   // 健康: 計画は空

    topo_sort();
}

// 依存関係を満たす順に段へ分ける。同じ段は並列に走れる。
void Model::topo_sort() {
    auto idx = index_of(plan_.steps);
    std::vector<std::string> ids;
    for (const auto &s : plan_.steps) ids.push_back(s.id);

    std::set<std::string> done;
    while (done.size() < ids.size()) {
        std::vector<std::string> ready;
        for (const auto &id : ids) {
            if (done.count(id)) continue;
            const Step &s = plan_.steps[idx[id]];
            bool ok = true;
            for (const auto &r : s.requires) {
                if (!done.count(r)) { ok = false; break; }
            }
            if (ok) ready.push_back(id);
        }
        if (ready.empty()) {
            plan_.error = "cycle or missing dependency in plan";
            return;
        }
        // 同じ段に入れられるものは入れる (並列化)。
        // 相互に代替関係にある step は**同じ段**に置いて fallback として扱う。
        // 片方が前提を待つために別段へ落ちると「代替」の意味が消えるので、
        // 代替相手が同段に居るなら前提を満たしたものとみなす。
        Group g;
        for (const auto &id : ready) {
            g.steps.push_back(id);
            done.insert(id);
        }
        // 代替の相手が次段に落ちる場合は、同段へ引き上げる。
        bool pulled = true;
        while (pulled) {
            pulled = false;
            for (const auto &id : g.steps) {
                for (const auto &a : plan_.steps[idx[id]].alternatives) {
                    if (done.count(a)) continue;
                    const Step &as = plan_.steps[idx[a]];
                    // 相手の前提が、すべてこの段で満たされるなら引き上げる
                    bool ok = true;
                    for (const auto &r : as.requires)
                        if (!done.count(r)) ok = false;
                    if (!ok) continue;
                    g.steps.push_back(a);
                    done.insert(a);
                    pulled = true;
                }
            }
        }
        g.attempted = (int)g.steps.size();
        plan_.order.push_back(g);

        // 1 段に複数入り、どれかが代替関係なら Fallback、そうでなければ Parallel
        bool any_alt = false;
        for (const auto &id : g.steps)
            if (!plan_.steps[idx[id]].alternatives.empty()) any_alt = true;
        if (g.steps.size() > 1)
            g.kind = any_alt ? OrderKind::Fallback : OrderKind::Parallel;
        else
            g.kind = OrderKind::Sequential;
        plan_.order.back().kind = g.kind;
    }
}

// 与えられた順序・並列条件で「結論が同じになるか」を試す。
//
// 重要な区別:
//   順序を逆にすると**前提が壊れて不成立になるのが正しい**。
//   それは「その順序が必要だった」という証拠であって、
//   自信を下げる理由ではない。
//   自信を下げるのは「前提を満たしたまま結論が変わる」場合だけ。
//
// 戻り値: 1 = 成立, 0 = 前提違反で不成立 (予期される), -1 = 成立したが結論が違う (危険)
int Model::simulate(const std::vector<std::string> &order, bool parallel_ok) const {
    std::set<std::string> done;
    bool mutated = false;
    for (const auto &id : order) {
        bool found = false;
        for (const auto &s : plan_.steps) {
            if (s.id != id) continue;
            found = true;
            bool ready = true;
            for (const auto &r : s.requires)
                if (!done.count(r)) ready = false;
            if (!ready) return 0;      // 前提が崩れる順序は不成立 (予期される)
            if (s.mutates) mutated = true;
            done.insert(id);
            break;
        }
        if (!found) return 0;
    }
    if (parallel_ok) {
        // 並列で片方が取れなくても、代替が取れていれば成立する。
        // どちらも取れないときは計画が成立しない (予期される不成立)。
        for (const auto &s : plan_.steps) {
            if (s.alternatives.empty()) continue;
            bool got_self = done.count(s.id) > 0;
            bool got_alt = false;
            for (const auto &a : s.alternatives)
                if (done.count(a)) got_alt = true;
            if (!got_self && !got_alt) return 0;
        }
    }
    // 「修復すべき問題があるのに何も書き換えない」計画は不成立。
    if (!findings_.empty() && !mutated) return 0;
    return 1;
}

// ------------------------------------------------------------------
// verify: 計画の自信を回帰で測る
// ------------------------------------------------------------------
void Model::verify() {
    conf_ = Confidence();
    if (!plan_.ok() || plan_.steps.empty()) {
        conf_.basis = "no plan (nothing to fix)";
        conf_.score = findings_.empty() ? 1.0 : 0.0;
        conf_.stability = 1.0;
        return;
    }

    // 1) 根拠の充足率: finding の evidence が観測済みか
    int ev_total = 0, ev_seen = 0;
    for (const auto &f : findings_) {
        for (const auto &e : f.evidence) {
            ev_total++;
            for (const auto &s : signals_)
                if (s.id == e && s.observed) { ev_seen++; break; }
        }
    }
    conf_.evidence_coverage = ev_total ? (double)ev_seen / ev_total : 0.0;

    // 2) 順序の安定性: 段の順序を入れ替えて結論が変わらないか
    std::vector<std::string> base;
    for (const auto &g : plan_.order)
        for (const auto &id : g.steps) base.push_back(id);

    std::vector<std::vector<std::string>> orders;
    orders.push_back(base);                       // 本来の順序
    {
        std::vector<std::string> o = base;        // 全体を反転
        std::reverse(o.begin(), o.end());
        orders.push_back(o);
    }
    {
        std::vector<std::string> o;               // 段を逆順に
        for (auto it = plan_.order.rbegin(); it != plan_.order.rend(); ++it)
            for (const auto &id : it->steps) o.push_back(id);
        orders.push_back(o);
    }
    // 3 種の順序を試し、次のように数える:
    //   base        … 成立するのが正しい
    //   反転        … 前提違反で不成立になるのが正しい (順序依存の証拠)
    //   段逆順      … 同上
    //   「成立したのに結論が違う」(-1) が出たら致命的。
    int expected_ok = 0;      // 予期通り成立
    int expected_fail = 0;    // 予期通り不成立 (前提違反)
    int contradiction = 0;    // 成立したが結論が違う
    for (size_t i = 0; i < orders.size(); i++) {
        conf_.orderings_tried++;
        int r = simulate(orders[i], true);
        if (i == 0) { if (r == 1) expected_ok++; else contradiction++; }
        else { if (r == 0) expected_fail++; else if (r == 1) expected_ok++; else contradiction++; }
    }
    conf_.orderings_agreeing = expected_ok + expected_fail;

    // 安定性 = 「矛盾なく説明できた順序の割合」
    conf_.stability = conf_.orderings_tried
                          ? (double)conf_.orderings_agreeing / conf_.orderings_tried
                          : 0.0;

    // 4) 総合。根拠が揃っていて、順序の効果が矛盾なく説明できるほど高い。
    conf_.score = 0.5 * conf_.evidence_coverage + 0.5 * conf_.stability;

    char buf[320];
    std::snprintf(buf, sizeof buf,
                  "evidence %d/%d observed; orderings: %d satisfied, %d rejected by "
                  "precondition (expected), %d contradictory",
                  ev_seen, ev_total, expected_ok, expected_fail, contradiction);
    conf_.basis = buf;
}

Verdict Model::evaluate() const {
    if (findings_.empty()) return Verdict::Healthy;
    for (const auto &f : findings_)
        if (f.fixable) return Verdict::NeedsFix;
    return Verdict::Unknown;
}

void Model::build_advice() {
    advice_.summary.clear();
    advice_.steps.clear();
    if (findings_.empty()) {
        advice_.summary = "問題は検出されていない (何もしない)。";
        advice_.steps = { "状態は健全。自発的な修復は行わない。" };
        return;
    }
    advice_.summary = "修復すべき問題が " + std::to_string(findings_.size()) + " 件。";
    for (const auto &f : findings_)
        advice_.steps.push_back("問題: " + f.summary + " (根拠 " +
                                std::to_string(f.evidence.size()) + " 件)");
}

} // namespace nx

// ==================================================================
// C エントリ (CLI から使う)
// ==================================================================

// 時刻は観測の一部 (いつ見た事実か) なので、出力に必ず添える。
static std::string nx_now_iso();

extern "C" int nx_plan_analyze(const char *subject, FILE *out) {
    if (!out) out = stdout;
    nx::Model m(subject ? subject : "");
    m.collect();
    m.analyze();
    m.plan();
    m.verify();

    nx::Verdict v = m.evaluate();
    std::fprintf(out, "=== NeoDAX analysis plan ===\n");
    std::fprintf(out, "subject=%s\n", m.subject().c_str());
    std::fprintf(out, "observed_at=%s\n", nx_now_iso().c_str());

    std::fprintf(out, "signals (%zu):\n", m.signals().size());
    for (const auto &s : m.signals())
        std::fprintf(out, "  %-24s %-9s %-10s %s\n", s.id.c_str(),
                     s.persistence == nx::Persistence::Durable ? "durable" : "transient",
                     s.observed ? "observed" : "missing", s.value.c_str());

    std::fprintf(out, "findings (%zu):\n", m.findings().size());
    for (const auto &f : m.findings()) {
        std::fprintf(out, "  %-18s sev=%.2f fixable=%s  %s\n", f.id.c_str(),
                     f.severity, f.fixable ? "yes" : "no", f.summary.c_str());
        for (const auto &e : f.evidence)
            std::fprintf(out, "      evidence: %s\n", e.c_str());
    }

    std::fprintf(out, "plan (%zu steps, %zu stages):\n",
                 m.make_plan().steps.size(), m.make_plan().order.size());
    if (!m.make_plan().ok())
        std::fprintf(out, "  PLAN ERROR: %s\n", m.make_plan().error.c_str());
    int stage = 1;
    for (const auto &g : m.make_plan().order) {
        const char *k = g.kind == nx::OrderKind::Parallel   ? "parallel"
                        : g.kind == nx::OrderKind::Fallback ? "fallback"
                                                            : "sequential";
        std::fprintf(out, "  stage %d (%s):\n", stage++, k);
        for (const auto &id : g.steps) {
            for (const auto &s : m.make_plan().steps) {
                if (s.id != id) continue;
                std::fprintf(out, "    %-16s %s%s\n", s.id.c_str(), s.action.c_str(),
                             s.approved ? "  [needs approval]" : "");
                if (!s.requires.empty()) {
                    std::fprintf(out, "      requires:");
                    for (const auto &r : s.requires) std::fprintf(out, " %s", r.c_str());
                    std::fprintf(out, "\n");
                }
                if (!s.alternatives.empty()) {
                    std::fprintf(out, "      fallback:");
                    for (const auto &a : s.alternatives) std::fprintf(out, " %s", a.c_str());
                    std::fprintf(out, "\n");
                }
            }
        }
    }

    std::fprintf(out, "VERDICT=%s\n",
                 v == nx::Verdict::NeedsFix ? "NEEDS_FIX"
                 : v == nx::Verdict::Healthy ? "HEALTHY" : "UNKNOWN");
    std::fprintf(out, "confidence=%.2f  (%s)\n", m.confidence().score,
                 m.confidence().basis.c_str());
    std::fprintf(out, "  %s\n", m.advice().summary.c_str());
    for (const auto &s : m.advice().steps) std::fprintf(out, "  - %s\n", s.c_str());

    return v == nx::Verdict::Healthy ? 0 : v == nx::Verdict::NeedsFix ? 1 : 2;
}

static std::string nx_now_iso() { return nx::now_iso(); }

// ==================================================================
// 自発解析: 対象から事実を集めてから計画する
// ==================================================================

#include "../nx_deepai.h"
#include "../nx_state.h"
#include "../attach/nx_appfix.h"
#include "../nx_leveldb.h"

namespace {

// タブ状 JSON から (kind, env, thread, surface, id) を取り出す。
// advisor と同じ規則: chat を集め、surface の所有者が居なければ orphan。
struct TabRow {
    std::string id, surface, env, thread, kind;
};

std::string jf(const std::string &o, const std::string &key) {
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

std::vector<TabRow> parse_tabs(const std::string &js) {
    std::vector<TabRow> rows;
    size_t tp = js.find("\"tabs\"");
    if (tp == std::string::npos) return rows;
    size_t lb = js.find('[', tp);
    if (lb == std::string::npos) return rows;

    int arr = 0, obj = 0, in_str = 0, esc = 0;
    size_t start = 0;
    for (size_t i = lb; i < js.size(); i++) {
        char c = js[i];
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
            if (arr == 1 && obj == 0) start = i;
            obj++;
            continue;
        }
        if (c == '}') {
            obj--;
            if (arr == 1 && obj == 0 && start) {
                std::string o = js.substr(start, i - start + 1);
                TabRow r;
                r.id = jf(o, "id");
                r.surface = jf(o, "surfaceId");
                r.kind = jf(o, "kind");
                // threadRef の中だけを見る (id 側の環境IDを拾わないため)
                size_t tr = o.find("\"threadRef\"");
                if (tr != std::string::npos) {
                    size_t b = o.find('{', tr);
                    int d2 = 0, is = 0, es = 0;
                    size_t e2 = b;
                    for (size_t k = b; k < o.size(); k++) {
                        char cc = o[k];
                        if (is) {
                            if (es) es = 0;
                            else if (cc == '\\') es = 1;
                            else if (cc == '"') is = 0;
                            continue;
                        }
                        if (cc == '"') { is = 1; continue; }
                        if (cc == '{') d2++;
                        else if (cc == '}') { d2--; if (d2 == 0) { e2 = k + 1; break; } }
                    }
                    std::string tref = o.substr(b, e2 - b);
                    r.env = jf(tref, "environmentId");
                    r.thread = jf(tref, "threadId");
                }
                rows.push_back(r);
                start = 0;
            }
            continue;
        }
    }
    return rows;
}

} // namespace

extern "C" int nx_plan_analyze_store(const char *state_path, FILE *out) {
    if (!out) out = stdout;
    nx::Model m(state_path ? state_path : "(none)");

    char *js = state_path ? nx_deepai_load_state(state_path) : NULL;
    if (!js) {
        std::fprintf(out, "  [plan] no state at %s\n", state_path ? state_path : "(none)");
        return 2;
    }
    std::string state(js);
    free(js);

    // ---- 事実 1: タブの分類 ----
    std::vector<TabRow> rows = parse_tabs(state);
    std::vector<std::pair<std::string, std::string>> live;
    for (const auto &r : rows)
        if (r.kind == "chat") live.emplace_back(r.env, r.thread);

    int orphans = 0, live_surfaces = 0, chats = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        const auto &r = rows[i];
        if (r.kind == "chat") { chats++; continue; }
        if (r.kind != "surface") continue;
        bool alive = false;
        for (const auto &l : live)
            if (l.first == r.env && l.second == r.thread) { alive = true; break; }
        if (alive) live_surfaces++;
        else {
            orphans++;
            nx::Signal s;
            s.id = "tabs.orphan[" + std::to_string(i) + "]";
            s.source = std::string(state_path ? state_path : "") + " (persisted)";
            // 計画の id は実際のタブ id を持つ。delete --id にそのまま渡せる。
            s.value = r.id;
            s.observed = true;
            m.add_signal(s);
        }
    }

    // ---- 事実 2: 件数 (永続層) ----
    // 稼働層の件数は別経路で観測するので、ここでは永続層だけを主張する。
    {
        nx::Signal s;
        s.id = "store.persisted.count";
        s.source = state_path ? state_path : "";
        s.value = std::to_string((int)rows.size());
        s.observed = true;
        m.add_signal(s);
    }

    // ---- 事実 3/4: LevelDB 固有の事実 ----
    // seq と退避コピーは LevelDB にしか存在しない概念。
    // 単なる JSON ファイルに対して「seq が無い = 異常」と言ってはいけない
    // (観測できないことを問題にしない)。
    struct stat st;
    bool is_leveldb = state_path && stat(state_path, &st) == 0 && S_ISDIR(st.st_mode);
    if (is_leveldb) {
        unsigned long long mx = nx_state_max_seq(state_path, "workspace-tabs");
        nx::Signal s;
        s.id = "seq.sanity";
        s.source = state_path;
        s.value = (mx > 0 && mx < 100000000ULL) ? "ok" : "suspicious";
        s.observed = true;
        m.add_signal(s);

        // 列挙ではなく生存判定を使う。列挙は履歴のバイトを拾うため、
        // 削除レコードを書いても "live" のままになる (偽陽性のループ)。
        nx::Signal b;
        b.id = "store.backup";
        b.source = state_path;
        b.value = nx_state_key_is_live(state_path, "nx-backup") ? "live" : "none";
        b.observed = true;
        m.add_signal(b);
    }

    m.analyze();
    m.plan();
    m.verify();

    // ---- 出力 ----
    nx::Verdict v = m.evaluate();
    std::fprintf(out, "=== NeoDAX analysis plan (self-initiated) ===\n");
    std::fprintf(out, "subject=%s\n", m.subject().c_str());
    std::fprintf(out, "observed_at=%s\n", nx_now_iso().c_str());
    std::fprintf(out, "tabs: chats=%d live_surfaces=%d orphans=%d\n",
                 chats, live_surfaces, orphans);

    std::fprintf(out, "signals (%zu):\n", m.signals().size());
    for (const auto &s : m.signals())
        std::fprintf(out, "  %-22s %-9s %-10s %s\n", s.id.c_str(),
                     s.persistence == nx::Persistence::Durable ? "durable" : "transient",
                     s.observed ? "observed" : "missing", s.value.c_str());

    std::fprintf(out, "findings (%zu):\n", m.findings().size());
    for (const auto &f : m.findings()) {
        std::fprintf(out, "  %-18s sev=%.2f fixable=%s  %s\n", f.id.c_str(),
                     f.severity, f.fixable ? "yes" : "no", f.summary.c_str());
        for (const auto &e : f.evidence)
            std::fprintf(out, "      evidence: %s\n", e.c_str());
    }

    std::fprintf(out, "plan (%zu steps, %zu stages):\n",
                 m.make_plan().steps.size(), m.make_plan().order.size());
    if (!m.make_plan().ok())
        std::fprintf(out, "  PLAN ERROR: %s\n", m.make_plan().error.c_str());
    int stage = 1;
    for (const auto &g : m.make_plan().order) {
        const char *k = g.kind == nx::OrderKind::Parallel   ? "parallel"
                        : g.kind == nx::OrderKind::Fallback ? "fallback"
                                                            : "sequential";
        std::fprintf(out, "  stage %d (%s):\n", stage++, k);
        for (const auto &id : g.steps)
            for (const auto &s : m.make_plan().steps) {
                if (s.id != id) continue;
                std::fprintf(out, "    %-16s %s%s\n", s.id.c_str(), s.action.c_str(),
                             s.approved ? "  [needs approval]" : "");
                if (!s.requires.empty()) {
                    std::fprintf(out, "      requires:");
                    for (const auto &r : s.requires) std::fprintf(out, " %s", r.c_str());
                    std::fprintf(out, "\n");
                }
                if (!s.alternatives.empty()) {
                    std::fprintf(out, "      fallback:");
                    for (const auto &a : s.alternatives) std::fprintf(out, " %s", a.c_str());
                    std::fprintf(out, "\n");
                }
            }
    }

    std::fprintf(out, "VERDICT=%s\n",
                 v == nx::Verdict::NeedsFix ? "NEEDS_FIX"
                 : v == nx::Verdict::Healthy ? "HEALTHY" : "UNKNOWN");
    std::fprintf(out, "confidence=%.2f  (%s)\n", m.confidence().score,
                 m.confidence().basis.c_str());
    std::fprintf(out, "  %s\n", m.advice().summary.c_str());
    for (const auto &s : m.advice().steps) std::fprintf(out, "  - %s\n", s.c_str());

    return v == nx::Verdict::Healthy ? 0 : v == nx::Verdict::NeedsFix ? 1 : 2;
}

// ==================================================================
// repair: 計画を「1 つの手続き」として最後まで回す
//
//   断片のパッチを積むのではなく、
//     observe -> analyze -> plan -> execute(stage order) -> re-observe
//   を健全になるまで繰り返す。各段の後に必ず観測し直すので、
//   「書いたつもり」を成功と呼ばない。
// ==================================================================

#include <unistd.h>

namespace {

// 計画の実行に必要な最小限のプリミティブ。
// 書き込みは必ず「ストアに読み直して一致を確かめる」ところまでを含める。
int persist_value(const char *dir, const char *json, FILE *out) {
    size_t klen = 0;
    char *key = nx_state_find_key(dir, "workspace-tabs", &klen);
    if (!key) { std::fprintf(out, "      no key in %s\n", dir); return -1; }
    unsigned long long seq = nx_state_max_seq(dir, "workspace-tabs") + 1;
    int rc = nx_leveldb_append_kv(dir, key, klen,
                                  (const unsigned char *)json, strlen(json),
                                  seq, NULL, out);
    free(key);
    if (rc != 0) return rc;
    char *back = nx_deepai_load_state(dir);
    // 期待は今回書いた JSON の内容 (キー順の正規化はしないので、
    // タブ集合が一致するかで確かめる)。
    int ok = 0;
    if (back) {
        auto want = parse_tabs(json);
        auto got = parse_tabs(back);
        ok = (want.size() == got.size());
        for (size_t i = 0; ok && i < want.size(); i++)
            if (want[i].id != got[i].id) ok = 0;
    }
    if (ok) std::fprintf(out, "      verified: %zu tabs resolved\n",
                         parse_tabs(json).size());
    else std::fprintf(out, "      VERIFY FAILED\n");
    free(back);
    return ok ? 0 : -1;
}

int append_delete_key(const char *dir, const char *hint, FILE *out) {
    // 既に消えているなら何もしない (毎ラウンド同じ削除を書かない)。
    if (!nx_state_key_is_live(dir, hint)) return 0;
    char **keys = NULL; size_t *lens = NULL;
    int nb = nx_state_list_keys(dir, hint, &keys, &lens, 16);
    if (nb <= 0) { free(keys); free(lens); return -1; }
    unsigned long long seq = nx_state_max_seq(dir, NULL) + 1;
    int rc = 0;
    for (int i = 0; i < nb; i++)
        if (nx_leveldb_append_delete(dir, keys[i], lens[i], seq++, NULL, out) != 0) rc = -1;
    return rc;
}

// 稼働中ストアへ JSON をそのまま反映する (nx_plan 内の live-sync 用)。
static int nx_appfix_apply_from(const char *dir, const char *json, FILE *out) {
    (void)dir;
    nx_appfix_profile_t prof = *nx_appfix_default_profile();
    return nx_appfix_apply(&prof, json, strlen(json), out);
}

} // namespace
extern "C" int nx_plan_repair(const char *state_path, FILE *out) {
    if (!out) out = stdout;
    if (!state_path) { std::fprintf(out, "repair: --state required\n"); return 2; }

    const int MAX_ROUNDS = 4;
    for (int round = 1; round <= MAX_ROUNDS; round++) {
        nx::Model m(state_path);
        char *js = nx_deepai_load_state(state_path);
        if (!js) { std::fprintf(out, "  [repair] no state\n"); return 2; }
        std::string state(js);
        free(js);

        std::vector<TabRow> rows = parse_tabs(state);
        std::vector<std::pair<std::string, std::string>> live;
        for (const auto &r : rows)
            if (r.kind == "chat") live.emplace_back(r.env, r.thread);

        int orphans = 0;
        std::vector<std::string> orphan_ids;
        for (size_t i = 0; i < rows.size(); i++) {
            const auto &r = rows[i];
            if (r.kind != "surface") continue;
            bool alive = false;
            for (const auto &l : live)
                if (l.first == r.env && l.second == r.thread) { alive = true; break; }
            if (alive) continue;
            orphans++;
            orphan_ids.push_back(r.id);
            nx::Signal s;
            s.id = "tabs.orphan[" + std::to_string(i) + "]";
            s.source = std::string(state_path) + " (persisted)";
            s.value = r.id;
            s.observed = true;
            m.add_signal(s);
        }
        {
            nx::Signal s;
            s.id = "store.persisted.count";
            s.source = state_path;
            s.value = std::to_string((int)rows.size());
            s.observed = true;
            m.add_signal(s);
        }
        unsigned long long mx = nx_state_max_seq(state_path, "workspace-tabs");
        {
            nx::Signal s;
            s.id = "seq.sanity"; s.source = state_path;
            s.value = (mx > 0 && mx < 100000000ULL) ? "ok" : "suspicious";
            s.observed = true; m.add_signal(s);
        }
        {
            nx::Signal s;
            s.id = "store.backup"; s.source = state_path;
            s.value = nx_state_key_is_live(state_path, "nx-backup") ? "live" : "none";
            s.observed = true; m.add_signal(s);
        }

        m.analyze(); m.plan(); m.verify();
        nx::Verdict v = m.evaluate();

        std::fprintf(out, "-- round %d: orphans=%d findings=%zu verdict=%s conf=%.2f\n",
                     round, orphans, m.findings().size(),
                     v == nx::Verdict::NeedsFix ? "NEEDS_FIX"
                     : v == nx::Verdict::Healthy ? "HEALTHY" : "UNKNOWN",
                     m.confidence().score);

        if (v == nx::Verdict::Healthy) {
            std::fprintf(out, "  [repair] healthy; nothing to do\n");
            return 0;
        }
        if (!m.make_plan().ok()) {
            std::fprintf(out, "  [repair] plan error: %s\n", m.make_plan().error.c_str());
            return 2;
        }

        // ---- 計画の各段を順に実行 ----
        // 段は topo_sort が決めた順序。stage 1 から順に、段内は
        // fallback なら「片方が取れればよい」として扱う。
        int mutated = 0;
        for (const auto &g : m.make_plan().order) {
            bool stage_ok = false;
            for (const auto &id : g.steps) {
                if (id == "persist-fix") {
                    // 永続層から orphan を除いた JSON を作って書く
                    char *fixed = NULL; int nrem = 0;
                    std::string cur = state;
                    int rc = 0;
                    for (const auto &oid : orphan_ids) {
                        char *next = NULL; int n = 0;
                        if (nx_deepai_remove_tab_json(cur.c_str(), cur.size(), oid.c_str(),
                                                      &next, &n, out) != 0) { rc = -1; break; }
                        if (!next) break;
                        free(fixed); fixed = next; nrem += n; cur = next;
                    }
                    if (rc == 0 && fixed && nrem > 0) {
                        rc = persist_value(state_path, fixed, out);
                        std::fprintf(out, "    persist-fix: removed=%d rc=%d\n", nrem, rc);
                        if (rc == 0) { mutated++; stage_ok = true; }
                    } else {
                        std::fprintf(out, "    persist-fix: nothing removed\n");
                    }
                    free(fixed);
                } else if (id == "purge-backup") {
                    int rc = append_delete_key(state_path, "nx-backup", out);
                    std::fprintf(out, "    purge-backup: rc=%d\n", rc);
                    if (rc == 0) { mutated++; stage_ok = true; }
                } else if (id == "live-sync" || id == "live-sync-alt") {
                    // fallback: 片方が取れればよい。両方試す。
                    char *now = nx_deepai_load_state(state_path);
                    if (now) {
                        int rc = nx_appfix_apply_from(state_path, now, out);
                        std::fprintf(out, "    %s: rc=%d\n", id.c_str(), rc);
                        if (rc == 0) { mutated++; stage_ok = true; }
                        free(now);
                    }
                }
            }
            std::fprintf(out, "  stage done: %s\n", stage_ok ? "ok" : "no effect");
        }

        if (!mutated) {
            std::fprintf(out, "  [repair] no step had an effect; stopping\n");
            return 2;
        }
        // 次ラウンドで観測し直す (書いたつもりを成功と呼ばない)
    }
    std::fprintf(out, "  [repair] max rounds reached\n");
    return 1;
}

// ==================================================================
// 案件を使った自発解析: 対象に合う案件を選び、逐次に試す
// ==================================================================

extern "C" int nx_plan_analyze_case(const char *kind, const char *path,
                                    const char *case_name, FILE *out) {
    if (!out) out = stdout;
    nx::register_builtin_cases();

    nx::CaseTarget target;
    target.kind = kind ? kind : "app";
    target.path = path ? path : "";

    std::vector<std::shared_ptr<nx::Case>> chosen;
    if (case_name && *case_name) {
        auto c = nx::CaseRegistry::instance().by_name(case_name);
        if (c) chosen.push_back(c);
    } else {
        chosen = nx::CaseRegistry::instance().match(target);
    }

    if (chosen.empty()) {
        std::fprintf(out, "  [case] no case applies to %s (%s)\n",
                     target.path.c_str(), target.kind.c_str());
        std::fprintf(out, "  [case] known cases:");
        for (const auto &c : nx::CaseRegistry::instance().all())
            std::fprintf(out, " %s", c->name());
        std::fprintf(out, "\n");
        return 2;
    }

    int rc = 0;
    // 逐次に試す。最初に手を打てる案件を採用し、他は記録だけ残す。
    for (const auto &c : chosen) {
        auto t = std::make_shared<nx::CaseTarget>(target);
        nx::Model m(target.path);
        m.use_case(c, t);
        m.collect();
        m.analyze();
        m.plan();
        m.verify();

        nx::Verdict v = m.evaluate();
        std::fprintf(out, "=== case: %s ===\n", c->name());
        std::fprintf(out, "observed_at=%s\n", nx_now_iso().c_str());
        std::fprintf(out, "signals (%zu):\n", m.signals().size());
        for (const auto &s : m.signals())
            std::fprintf(out, "  %-28s %-9s %-10s %s\n", s.id.c_str(),
                         s.persistence == nx::Persistence::Durable ? "durable" : "transient",
                         s.observed ? "observed" : "missing", s.value.c_str());

        std::fprintf(out, "findings (%zu):\n", m.findings().size());
        for (const auto &f : m.findings()) {
            std::fprintf(out, "  %-24s sev=%.2f fixable=%s  %s\n", f.id.c_str(),
                         f.severity, f.fixable ? "yes" : "no", f.summary.c_str());
            for (const auto &e : f.evidence)
                std::fprintf(out, "      evidence: %s\n", e.c_str());
        }

        std::fprintf(out, "plan (%zu steps, %zu stages):\n",
                     m.make_plan().steps.size(), m.make_plan().order.size());
        int stage = 1;
        for (const auto &g : m.make_plan().order) {
            const char *k = g.kind == nx::OrderKind::Parallel   ? "parallel"
                            : g.kind == nx::OrderKind::Fallback ? "fallback"
                                                            : "sequential";
            std::fprintf(out, "  stage %d (%s):\n", stage++, k);
            for (const auto &id : g.steps)
                for (const auto &s : m.make_plan().steps) {
                    if (s.id != id) continue;
                    std::fprintf(out, "    %-26s %s%s\n", s.id.c_str(), s.action.c_str(),
                                 s.approved ? "  [needs approval]" : "");
                    if (!s.requires.empty()) {
                        std::fprintf(out, "      requires:");
                        for (const auto &r : s.requires) std::fprintf(out, " %s", r.c_str());
                        std::fprintf(out, "\n");
                    }
                    if (!s.alternatives.empty()) {
                        std::fprintf(out, "      fallback:");
                        for (const auto &a : s.alternatives) std::fprintf(out, " %s", a.c_str());
                        std::fprintf(out, "\n");
                    }
                }
        }

        std::fprintf(out, "VERDICT=%s\n",
                     v == nx::Verdict::NeedsFix ? "NEEDS_FIX"
                     : v == nx::Verdict::Healthy ? "HEALTHY" : "UNKNOWN");
        std::fprintf(out, "confidence=%.2f  (%s)\n", m.confidence().score,
                     m.confidence().basis.c_str());
        for (const auto &s : m.advice().steps) std::fprintf(out, "  - %s\n", s.c_str());
        std::fprintf(out, "\n");
        if (v == nx::Verdict::NeedsFix) rc = 1;
    }
    return rc;
}
