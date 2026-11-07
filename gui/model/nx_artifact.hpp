// nx_artifact.hpp — 「消しても復活する残骸」を検出するモデル (多相設計 / AppKit 非依存)
//
// 動機:
//   ファイルを消したのに diff / 表示が復活する、という症状の原因は git ではなく
//   「復元用の残骸 (チェックポイント, パッチ, バックアップ, tar)」であることが多い。
//   ここではその残骸を Artifact として多相に扱い、検出・分類・由来の特定まで行う。
//
// 設計方針 (nx_graph.hpp と同じ流儀):
//   ・Artifact       … 検出された残骸 1 件。kind ごとに派生クラス。
//   ・ArtifactRule   … 検出戦略。ルールごとに派生クラス。
//   ・ArtifactScan   … 上記を束ねて走査し、結果を保持する門番。
//
//   view / CLI 側に kind 分岐を書かない。判定は仮想関数に閉じる。

#ifndef NX_ARTIFACT_HPP
#define NX_ARTIFACT_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nx {

// ------------------------------------------------------------------ Artifact

// 残骸の重大度。diff を復活させうる順に強い。
enum class Severity {
    Info = 0,     // 記録として残っているだけ
    Warning = 1,  // 条件次第で復活する
    Critical = 2  // 確実に復活させる (復元パッチ / 復元手順を含む)
};

const char *severity_name(Severity s);

// 残骸 1 件。kind は派生クラスが名乗る。
class Artifact {
public:
    virtual ~Artifact() = default;

    virtual const char *kind() const = 0;              // "checkpoint" / "patch" / ...
    virtual const char *restore_mechanism() const = 0; // どうやって復活させるか
    virtual Severity severity() const;                 // 既定は Warning
    virtual bool affects(const std::string &target) const; // target を復活させうるか

    // この残骸が「復活させる対象ファイル」を列挙する (パッチなら削除行の ---/+++ から)
    virtual std::vector<std::string> restores() const { return {}; }

    virtual std::string summary() const;               // 1 行の説明

    std::string path;        // 残骸自身の絶対/相対パス
    uint64_t    size = 0;    // バイト数 (ディレクトリは 0)
    int64_t     mtime = 0;   // epoch 秒
};

// 復元用ディレクトリ (.checkpoints/ など)。中に patch/tar/手順書を抱える。
class CheckpointDir : public Artifact {
public:
    const char *kind() const override { return "checkpoint"; }
    const char *restore_mechanism() const override {
        return "git apply / tar -xzf (手順書に従った復元)";
    }
    Severity severity() const override { return Severity::Critical; }
    bool affects(const std::string &target) const override;
    std::vector<std::string> restores() const override { return restored_; }
    std::string summary() const override;

    std::vector<std::string> children;   // 直下のファイル名
    std::vector<std::string> restored_;  // 中の patch/tar が復活させる対象
};

// diff 実体を持つパッチファイル。削除行 (-) を含むとファイルを復活させる。
class PatchFile : public Artifact {
public:
    const char *kind() const override { return "patch"; }
    const char *restore_mechanism() const override { return "git apply / patch -p1"; }
    Severity severity() const override;
    std::vector<std::string> restores() const override { return restored_; }
    std::string summary() const override;

    // 解析結果
    int  deleted_files = 0;   // "deleted file mode" または --- 側のみのファイル数
    int  modified_files = 0;
    bool whole_file_deletion = false; // 1 ファイル丸ごと削除の diff を含む
    std::vector<std::string> restored_;  // 復活対象のパス
};

// 未追跡ファイルの実体アーカイブ。展開すると消したファイルが戻る。
class ArchiveFile : public Artifact {
public:
    const char *kind() const override { return "archive"; }
    const char *restore_mechanism() const override { return "tar -xzf"; }
    Severity severity() const override { return Severity::Warning; }
    std::string summary() const override;

    bool is_tar_gz = false;
};

// 復元手順書。実行すると上の patch/archive を適用する。
class RestoreScript : public Artifact {
public:
    const char *kind() const override { return "restore-doc"; }
    const char *restore_mechanism() const override { return "手順書に従った手動復元"; }
    Severity severity() const override { return Severity::Critical; }
    std::string summary() const override;

    std::vector<std::string> commands;  // 検出した復元コマンド行
};

// エディタ / ツールのバックアップ (foo.mm~, .#foo.mm, foo.mm.orig など)
class BackupFile : public Artifact {
public:
    const char *kind() const override { return "backup"; }
    const char *restore_mechanism() const override { return "エディタ復元 / mv での差し戻し"; }
    Severity severity() const override { return Severity::Info; }
    std::string summary() const override;
};

// ------------------------------------------------------------------ Rule

// 検出戦略。1 ルール = 1 種類の残骸。
class ArtifactRule {
public:
    virtual ~ArtifactRule() = default;
    virtual const char *name() const = 0;
    // root 以下を走査し、見つけた残骸を out に追加する。
    virtual void scan(const std::string &root, std::vector<std::unique_ptr<Artifact>> &out) const = 0;
};

// .checkpoints/ のような復元用ディレクトリとその中身
class CheckpointRule : public ArtifactRule {
public:
    const char *name() const override { return "checkpoint-dir"; }
    void scan(const std::string &root, std::vector<std::unique_ptr<Artifact>> &out) const override;
};

// *.patch / *.diff
class PatchRule : public ArtifactRule {
public:
    const char *name() const override { return "patch-file"; }
    void scan(const std::string &root, std::vector<std::unique_ptr<Artifact>> &out) const override;
};

// *.tar.gz / *.tgz / *.tar
class ArchiveRule : public ArtifactRule {
public:
    const char *name() const override { return "archive-file"; }
    void scan(const std::string &root, std::vector<std::unique_ptr<Artifact>> &out) const override;
};

// RESTORE.md / RESTORE.txt / *.restore.sh
class RestoreScriptRule : public ArtifactRule {
public:
    const char *name() const override { return "restore-doc"; }
    void scan(const std::string &root, std::vector<std::unique_ptr<Artifact>> &out) const override;
};

// エディタのバックアップ
class BackupRule : public ArtifactRule {
public:
    const char *name() const override { return "backup-file"; }
    void scan(const std::string &root, std::vector<std::unique_ptr<Artifact>> &out) const override;
};

// ------------------------------------------------------------------ Scan

// 走査結果。target を指定すると「その 1 ファイルを復活させうる残骸」に絞れる。
class ArtifactScan {
public:
    ArtifactScan();
    ~ArtifactScan();

    // 既定ルール一式を登録する (Checkpoint/Patch/Archive/RestoreScript/Backup)
    void use_default_rules();
    void add_rule(std::unique_ptr<ArtifactRule> rule);

    // root 以下を走査。.git と node_modules と bin は除外する。
    void run(const std::string &root);

    // target (例: "gui/neodax-viewer.mm") を復活させうる残骸だけを返す。
    std::vector<const Artifact *> affecting(const std::string &target) const;

    // 指定 kind の残骸を返す。
    std::vector<const Artifact *> of_kind(const char *kind) const;

    const std::vector<std::unique_ptr<Artifact>> &all() const;
    size_t count() const;
    Severity max_severity() const;

    // 人が読む結論。「なぜ消しても復活するのか」を 1 文で返す。
    std::string verdict(const std::string &target) const;

    // 消すためのコマンド列 (副作用なし。実行は呼び出し側の責任)
    std::vector<std::string> removal_commands(const std::string &target) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace nx

#endif // NX_ARTIFACT_HPP
