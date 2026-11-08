// nx_case.cpp — 案件の登録簿と、組み込み案件の登録

#include "nx_case.hpp"

#include <algorithm>

namespace nx {

CaseRegistry &CaseRegistry::instance() {
    static CaseRegistry r;
    return r;
}

void CaseRegistry::add(std::shared_ptr<Case> c) {
    if (!c) return;
    // 同名は置き換える (組み込みを後から差し替えられるように)
    for (auto &e : cases_) {
        if (e && std::string(e->name()) == c->name()) { e = c; return; }
    }
    cases_.push_back(c);
}

std::vector<std::shared_ptr<Case>> CaseRegistry::match(const CaseTarget &t) const {
    std::vector<std::shared_ptr<Case>> out;
    for (const auto &c : cases_) {
        if (!c) continue;
        if (c->applicable(t)) out.push_back(c);   // 登録順を保つ (逐次に試す)
    }
    return out;
}

std::shared_ptr<Case> CaseRegistry::by_name(const std::string &n) const {
    for (const auto &c : cases_)
        if (c && n == c->name()) return c;
    return nullptr;
}

} // namespace nx
