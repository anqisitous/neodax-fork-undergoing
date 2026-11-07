// learn.hpp — neodax が「何を学んだか」を汎用的に貯める
//
// 方針:
//   ・対象アプリに偏らない。特定のアプリ名/パス/識別子は一切保存しない。
//   ・保存するのは「解析の仕方」そのもの (形式の癖、どの手が効いたか)。
//   ・解析の途中経過 (部分結果) を残し、次回はそこから続ける。
//
// 匿名性の担保:
//   save() の直前に sanitize() を通し、
//   パスらしい文字列・アプリ名らしい断片・UUID を落とす。
//   これにより「何を解析したか」は残らない。

#ifndef NX_LEARN_HPP
#define NX_LEARN_HPP

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace nx {

// 学んだ 1 項目。key は「形式の特徴」で、対象アプリとは無関係。
struct Skill {
    std::string key;     // 例 "fixups.ptr=6",
    std::string value;   // 例 "stub=adrp+ldr が有効だった",
    int         hits = 0;
};

class SkillStore {
public:
    static std::string default_path();      // ~/.neodax/skills.db

    void observe(const std::string &key, const std::string &value);
    const Skill *find(const std::string &key) const;

    bool save(const std::string &path) const;
    bool load(const std::string &path);

    void dump(FILE *out) const;
    void clear() { skills_.clear(); }

    // パス・アプリ名・UUID 等を落とす。漏洩を防ぐ最後の砦。
    static std::string sanitize(const std::string &s);

private:
    std::map<std::string, Skill> skills_;
};

} // namespace nx

#endif // NX_LEARN_HPP
