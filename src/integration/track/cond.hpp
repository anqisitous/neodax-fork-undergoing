// cond.hpp — 起動条件を多相なクラスで表す
//
// 目的:
//   アプリが「起動する / 起動しない」を決める条件を、if の並びではなく
//   クラスの集合として持つ。条件を足すときはクラスを 1 つ足すだけで済む。
//
// 設計:
//   Condition        条件の抽象。名前・説明・評価・見た値を持つ
//   FileCondition    ファイル/ディレクトリの存在・権限
//   DirWritableCondition  ディレクトリに書けるか
//   FrameworksCondition   必須フレームワークが揃っているか
//   ArgsCondition    起動を変える引数が付いていないか
//   StateCondition   復元元の状態が壊れていないか
//   Probe            条件を集めて評価し、判定と理由を出す
//
// ここは OS 固有の名前を最小限にし、条件そのものをデータとして扱う。

#ifndef NX_COND_HPP
#define NX_COND_HPP

#include <cstdio>
#include <string>
#include <vector>

namespace nx {

// 評価の結果
struct Verdict {
    bool ok = true;          // 条件が成立しているか
    std::string seen;        // 実際に見た値 (不成立の理由になる)
};

class Condition {
public:
    virtual ~Condition() = default;
    virtual const char *id() const = 0;      // "C1" など
    virtual const char *what() const = 0;    // 人間が読む説明
    virtual Verdict evaluate() const = 0;    // 評価
};

// パスが「在る・実行できる・読める」こと
class FileCondition : public Condition {
public:
    FileCondition(std::string id, std::string what, std::string path,
                  bool need_exec = false, bool need_read = false)
        : id_(std::move(id)), what_(std::move(what)), path_(std::move(path)),
          exec_(need_exec), read_(need_read) {}
    const char *id() const override { return id_.c_str(); }
    const char *what() const override { return what_.c_str(); }
    Verdict evaluate() const override;

private:
    std::string id_, what_, path_;
    bool exec_ = false, read_ = false;
};

// ディレクトリに書けること
class DirWritableCondition : public Condition {
public:
    DirWritableCondition(std::string id, std::string what, std::string dir)
        : id_(std::move(id)), what_(std::move(what)), dir_(std::move(dir)) {}
    const char *id() const override { return id_.c_str(); }
    const char *what() const override { return what_.c_str(); }
    Verdict evaluate() const override;

private:
    std::string id_, what_, dir_;
};

// 必須のフレームワーク/サブディレクトリが揃っていること
class FrameworksCondition : public Condition {
public:
    FrameworksCondition(std::string id, std::string what, std::string root,
                        std::vector<std::string> need)
        : id_(std::move(id)), what_(std::move(what)), root_(std::move(root)),
          need_(std::move(need)) {}
    const char *id() const override { return id_.c_str(); }
    const char *what() const override { return what_.c_str(); }
    Verdict evaluate() const override;

private:
    std::string id_, what_, root_;
    std::vector<std::string> need_;
};

// 起動の仕方を変える引数が付いていないこと
class ArgsCondition : public Condition {
public:
    ArgsCondition(std::string id, std::string what, std::vector<std::string> args,
                  std::vector<std::string> deny_prefixes)
        : id_(std::move(id)), what_(std::move(what)), args_(std::move(args)),
          deny_(std::move(deny_prefixes)) {}
    const char *id() const override { return id_.c_str(); }
    const char *what() const override { return what_.c_str(); }
    Verdict evaluate() const override;

private:
    std::string id_, what_;
    std::vector<std::string> args_, deny_;
};

// 復元元の状態が壊れていないこと
class StateCondition : public Condition {
public:
    StateCondition(std::string id, std::string what, std::string root,
                   std::vector<std::string> name_hints)
        : id_(std::move(id)), what_(std::move(what)), root_(std::move(root)),
          hints_(std::move(name_hints)) {}
    const char *id() const override { return id_.c_str(); }
    const char *what() const override { return what_.c_str(); }
    Verdict evaluate() const override;

private:
    std::string id_, what_, root_;
    std::vector<std::string> hints_;
};

// 条件を集めて評価する
class Probe {
public:
    void add(Condition *c) { conds_.push_back(c); }     // 所有はしない
    // 1 つずつ落としたときに起動可否がどう転ぶかを表にする
    void report_drop_each(FILE *out) const;
    // 素の判定
    void report(FILE *out) const;
    bool all_ok() const;

private:
    std::vector<Condition *> conds_;
};

} // namespace nx

#endif // NX_COND_HPP
