// nx_bestfix.cpp — 動かない問題を 1 個に絞り、その 1 個に最善を尽くす

#include "nx_bestfix.hpp"
#include "nx_case.hpp"
#include "nx_plan.hpp"
#include "nx_recipe.hpp"
#include "nx_template.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Remedy {
    const char *symbol;
    const char *note;
    int confidence;   // 0..100
};

/* 問題 1 つに対する修復候補を、確度の高い順に返す。 */
std::vector<Remedy> remedies_for(const std::string &id) {
    if (id == "license.should-check-gate")
        return {{"ShouldCheckLicense", "サインイン入口を外す (確認済み)", 95}};
    if (id == "license.root-spawns")
        return {{"AdobeGCInvokeClient", "外部の起動を止める", 70},
                {"licensingglue12_GLOBAL__N_112InvokeGoCart", "呼び出し元ごと止める", 60}};
    if (id == "license.exit-on-signin")
        return {{"DoSetup", "ライセンス初期化を起動列から外す (最後の手段)", 30}};
    return {};
}

std::string lib_of(const std::string &app) {
    return app + "/Contents/Frameworks/AfterFXLib.framework/Versions/A/AfterFXLib";
}

} // namespace

extern "C" int nx_bestfix(const char *app, const char *kind, int apply, FILE *out) {
    if (!out) out = stdout;
    if (!app || !*app) { fprintf(out, "best: --path required\n"); return 2; }

    nx::register_builtin_cases();
    nx::CaseTarget target;
    target.kind = kind && *kind ? kind : "app";
    target.path = app;

    std::vector<std::shared_ptr<nx::Case>> cases =
        nx::CaseRegistry::instance().match(target);
    if (cases.empty()) {
        fprintf(out, "  [best] この対象を扱うテンプレートがありません\n");
        return 1;
    }

    /* テンプレートは登録順に試す。最初に扱えるものを 1 つだけ使う。 */
    nx::Model m(target.path);
    m.use_case(cases[0], std::make_shared<nx::CaseTarget>(target));
    m.collect();
    m.analyze();
    m.plan();
    m.verify();

    fprintf(out, "=== best: 1 個だけ特定して最善を尽くす ===\n");
    fprintf(out, "app = %s\n", app);
    fprintf(out, "findings:\n");
    for (const auto &f : m.findings())
        fprintf(out, "  %-28s sev=%.2f fixable=%s\n", f.id.c_str(), f.severity,
                f.fixable ? "yes" : "no");

    /* 問題を 1 個に絞る: 修復候補があり、確度が最も高いもの。 */
    int chosen = -1, chosen_conf = -1;
    double chosen_sev = -1;
    for (size_t i = 0; i < m.findings().size(); i++) {
        const auto &f = m.findings()[i];
        auto rs = remedies_for(f.id);
        if (rs.empty()) continue;
        int conf = rs[0].confidence;
        if (conf > chosen_conf || (conf == chosen_conf && f.severity > chosen_sev)) {
            chosen = (int)i;
            chosen_conf = conf;
            chosen_sev = f.severity;
        }
    }
    if (chosen < 0) {
        fprintf(out, "  [best] コードで直せる問題を特定できません (報告のみ)\n");
        return 1;
    }

    const nx::Finding &f = m.findings()[(size_t)chosen];
    std::vector<Remedy> rs = remedies_for(f.id);
    fprintf(out, "特定した問題: %s (sev=%.2f, 修復確度=%d)\n",
            f.id.c_str(), f.severity, chosen_conf);

    std::string lib = lib_of(app);
    int attempts = 0, ok = 0;
    for (const auto &r : rs) {
        nx_site_t site{};
        if (nx_recipe_locate_symbol(lib.c_str(), r.symbol, &site) != 0) {
            fprintf(out, "  [候補] %-42s → 見つからない (skip)\n", r.symbol);
            continue;
        }
        fprintf(out, "  [候補] %-42s @0x%llx  %s\n", r.symbol, site.vaddr, r.note);
        if (!apply) continue;

        attempts++;
        fprintf(out, "  [実行] %s\n", r.symbol);
        if (nx_cut(app, r.symbol, 1, out) == 0) {
            fprintf(out, "  [成功] %s を適用しました (1 個だけ)\n", r.symbol);
            ok = 1;
            break;
        }
        fprintf(out, "  [失敗] %s → 次の候補へ\n", r.symbol);
    }

    if (!apply) {
        fprintf(out, "  [best] 計画のみ。--apply で最善を尽くします。\n");
        return 0;
    }
    if (!ok) {
        fprintf(out, "  [best] この問題に対する候補はすべて失敗しました (試行 %d)\n", attempts);
        return 1;
    }
    return 0;
}
