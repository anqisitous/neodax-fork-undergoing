// nx_artifact.cpp — 「消しても復活する残骸」検出の実装 (AppKit 非依存)
//
// 多相の実体はすべてここに置く。CLI / view には kind 分岐を書かない。

#include "nx_artifact.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace nx {

// 走査から除外するディレクトリ (残骸ではない / 巨大すぎる)
static bool is_skipped_dir(const std::string &name) {
    return name == ".git" || name == "node_modules" || name == "bin" ||
           name == ".build" || name == "build";
}

static bool ends_with(const std::string &s, const char *suf) {
    const size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

// ------------------------------------------------------------------ Severity

const char *severity_name(Severity s) {
    switch (s) {
        case Severity::Info:     return "info";
        case Severity::Warning:  return "warning";
        case Severity::Critical: return "critical";
    }
    return "info";
}

// ------------------------------------------------------------------ Artifact

Severity Artifact::severity() const { return Severity::Warning; }

std::string Artifact::summary() const {
    char b[512];
    std::snprintf(b, sizeof b, "%-12s %s (%llu B)", kind(), path.c_str(),
                  (unsigned long long)size);
    return b;
}

bool Artifact::affects(const std::string &target) const {
    if (target.empty()) return false;
    for (const std::string &r : restores())
        if (r == target || ends_with(r, ("/" + target).c_str())) return true;
    return false;
}

// ------------------------------------------------------------------ PatchFile

Severity PatchFile::severity() const {
    // 丸ごと削除の diff を含むパッチは、消したファイルを確実に復活させる。
    return whole_file_deletion ? Severity::Critical : Severity::Warning;
}

std::string PatchFile::summary() const {
    char b[512];
    std::snprintf(b, sizeof b,
                  "%-12s %s (%llu B, deleted=%d, modified=%d, whole-file-delete=%s)",
                  kind(), path.c_str(), (unsigned long long)size,
                  deleted_files, modified_files,
                  whole_file_deletion ? "yes" : "no");
    return b;
}

// ------------------------------------------------------------------ CheckpointDir

std::string CheckpointDir::summary() const {
    std::string kids;
    for (size_t i = 0; i < children.size() && i < 6; i++) {
        if (i) kids += ", ";
        kids += children[i];
    }
    if (children.size() > 6) kids += ", ...";
    char b[512];
    std::snprintf(b, sizeof b, "%-12s %s (%zu files: %s)", kind(), path.c_str(),
                  children.size(), kids.c_str());
    return b;
}

bool CheckpointDir::affects(const std::string &target) const {
    if (target.empty()) return false;
    for (const std::string &r : restored_)
        if (r == target || ends_with(r, ("/" + target).c_str())) return true;
    return false;
}

// ------------------------------------------------------------------ ArchiveFile

std::string ArchiveFile::summary() const {
    char b[512];
    std::snprintf(b, sizeof b, "%-12s %s (%llu B, tar=%s)", kind(), path.c_str(),
                  (unsigned long long)size, is_tar_gz ? "yes" : "?");
    return b;
}

// ------------------------------------------------------------------ RestoreScript

std::string RestoreScript::summary() const {
    char b[512];
    std::snprintf(b, sizeof b, "%-12s %s (%zu commands)", kind(), path.c_str(),
                  commands.size());
    return b;
}

// ------------------------------------------------------------------ BackupFile

std::string BackupFile::summary() const {
    char b[512];
    std::snprintf(b, sizeof b, "%-12s %s (%llu B)", kind(), path.c_str(),
                  (unsigned long long)size);
    return b;
}

// ------------------------------------------------------------------ helpers

// パッチを解析して、復活対象ファイルと削除種別を埋める。
static void parse_patch(PatchFile &pf, const fs::path &full) {
    std::ifstream in(full);
    if (!in) return;

    std::string line;
    bool in_block = false;
    bool minus_only = true;   // このファイルブロックに + 行が無い = 丸ごと削除
    bool deleted_mode = false;
    std::string cur;

    auto flush = [&]() {
        if (cur.empty()) return;
        pf.restored_.push_back(cur);
        if (deleted_mode || minus_only) {
            pf.deleted_files++;
            if (minus_only) pf.whole_file_deletion = true;
        } else {
            pf.modified_files++;
        }
        cur.clear();
    };

    while (std::getline(in, line)) {
        if (line.rfind("diff --git ", 0) == 0) {
            flush();
            in_block = true;
            minus_only = true;
            deleted_mode = false;
            // diff --git a/X b/X
            const size_t bpos = line.find(" b/");
            if (bpos != std::string::npos) cur = line.substr(bpos + 3);
            continue;
        }
        if (!in_block) continue;

        if (line.rfind("deleted file mode", 0) == 0) { deleted_mode = true; continue; }
        if (line.rfind("--- ", 0) == 0) {
            if (line == "--- /dev/null") { deleted_mode = true; }
            continue;
        }
        if (line.rfind("+++ ", 0) == 0) {
            if (line != "+++ /dev/null") minus_only = false;
            continue;
        }
        if (!line.empty() && line[0] == '+' && line.rfind("+++", 0) != 0)
            minus_only = false;
    }
    flush();
}

// 復元手順書からコマンド行を拾う。
static void parse_restore_doc(RestoreScript &rs, const fs::path &full) {
    std::ifstream in(full);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        // ``` フェンス内のコマンド、または git/tar/rm で始まる行
        const char *keys[] = { "git apply", "git checkout", "git restore",
                               "git stash", "tar -x", "tar x", "patch -p", "mv ", "cp " };
        for (const char *k : keys)
            if (line.find(k) != std::string::npos) { rs.commands.push_back(line); break; }
    }
}

static uint64_t size_of(const fs::path &p) {
    std::error_code ec;
    auto s = fs::file_size(p, ec);
    return ec ? 0 : (uint64_t)s;
}

static int64_t file_mtime(const fs::path &p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    // C++17 の file_clock を epoch 秒へ雑に寄せる (表示用なので厳密でなくてよい)
    return (int64_t)t.time_since_epoch().count() / 1000000000LL;
}

// ------------------------------------------------------------------ CheckpointRule

void CheckpointRule::scan(const std::string &root,
                          std::vector<std::unique_ptr<Artifact>> &out) const {
    const fs::path base(root);
    // 候補ディレクトリ名 (拡張子ではなく「復元の道具箱」を意味する名前)
    const char *names[] = { ".checkpoints", ".neodax-checkpoints",
                            ".restore", ".backup-checkpoints", nullptr };
    for (int i = 0; names[i]; i++) {
        const fs::path dir = base / names[i];
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;

        auto cp = std::make_unique<CheckpointDir>();
        cp->path = names[i];
        // 直下のファイル名を列挙しつつ、下位の patch が復活させる対象も引き継ぐ
        // (ディレクトリ自体も「復元のソース」なので affects() で拾えるようにする)
        for (auto &ent : fs::recursive_directory_iterator(dir, ec)) {
            if (ec) break;
            const std::string nm = ent.path().filename().string();
            if (ent.is_directory(ec)) {
                cp->children.push_back(nm + "/");
                continue;
            }
            cp->children.push_back(nm);
            if (ends_with(nm, ".patch") || ends_with(nm, ".diff")) {
                PatchFile tmp;
                tmp.path = nm;
                parse_patch(tmp, ent.path());
                for (const std::string &r : tmp.restored_) cp->restored_.push_back(r);
            }
        }
        std::sort(cp->children.begin(), cp->children.end());
        out.push_back(std::move(cp));
    }
}

// ------------------------------------------------------------------ PatchRule

void PatchRule::scan(const std::string &root,
                     std::vector<std::unique_ptr<Artifact>> &out) const {
    const fs::path base(root);
    std::error_code ec;
    std::string skip_dir;
    for (auto &ent : fs::recursive_directory_iterator(base, ec)) {
        if (ec) break;
        if (ent.is_directory(ec)) {
            if (is_skipped_dir(ent.path().filename().string()))
                skip_dir = ent.path().string();
            continue;
        }
        // 除外ディレクトリ配下は深さで弾く (この libc++ には disable_recursion_pending が無い)
        if (!skip_dir.empty() &&
            ent.path().string().rfind(skip_dir + "/", 0) == 0)
            continue;
        const std::string nm = ent.path().filename().string();
        if (!ends_with(nm, ".patch") && !ends_with(nm, ".diff")) continue;

        auto pf = std::make_unique<PatchFile>();
        pf->path = fs::relative(ent.path(), base, ec).string();
        if (pf->path.empty()) pf->path = ent.path().string();
        pf->size = size_of(ent.path());
        pf->mtime = file_mtime(ent.path());
        parse_patch(*pf, ent.path());
        out.push_back(std::move(pf));
    }
}

// ------------------------------------------------------------------ ArchiveRule

void ArchiveRule::scan(const std::string &root,
                       std::vector<std::unique_ptr<Artifact>> &out) const {
    const fs::path base(root);
    std::error_code ec;
    std::string skip_dir;
    for (auto &ent : fs::recursive_directory_iterator(base, ec)) {
        if (ec) break;
        if (ent.is_directory(ec)) {
            if (is_skipped_dir(ent.path().filename().string()))
                skip_dir = ent.path().string();
            continue;
        }
        // 除外ディレクトリ配下は深さで弾く (この libc++ には disable_recursion_pending が無い)
        if (!skip_dir.empty() &&
            ent.path().string().rfind(skip_dir + "/", 0) == 0)
            continue;
        const std::string nm = ent.path().filename().string();
        const bool tgz = ends_with(nm, ".tar.gz") || ends_with(nm, ".tgz");
        const bool tar = ends_with(nm, ".tar");
        if (!tgz && !tar) continue;

        auto af = std::make_unique<ArchiveFile>();
        af->path = fs::relative(ent.path(), base, ec).string();
        if (af->path.empty()) af->path = ent.path().string();
        af->size = size_of(ent.path());
        af->mtime = file_mtime(ent.path());
        af->is_tar_gz = tgz || tar;
        out.push_back(std::move(af));
    }
}

// ------------------------------------------------------------------ RestoreScriptRule

void RestoreScriptRule::scan(const std::string &root,
                             std::vector<std::unique_ptr<Artifact>> &out) const {
    const fs::path base(root);
    std::error_code ec;
    std::string skip_dir;
    for (auto &ent : fs::recursive_directory_iterator(base, ec)) {
        if (ec) break;
        if (ent.is_directory(ec)) {
            if (is_skipped_dir(ent.path().filename().string()))
                skip_dir = ent.path().string();
            continue;
        }
        // 除外ディレクトリ配下は深さで弾く (この libc++ には disable_recursion_pending が無い)
        if (!skip_dir.empty() &&
            ent.path().string().rfind(skip_dir + "/", 0) == 0)
            continue;
        const std::string nm = ent.path().filename().string();
        const bool hit = nm == "RESTORE.md" || nm == "RESTORE.txt" ||
                         nm == "RESTORE" || ends_with(nm, ".restore.sh");
        if (!hit) continue;

        auto rs = std::make_unique<RestoreScript>();
        rs->path = fs::relative(ent.path(), base, ec).string();
        if (rs->path.empty()) rs->path = ent.path().string();
        rs->size = size_of(ent.path());
        rs->mtime = file_mtime(ent.path());
        parse_restore_doc(*rs, ent.path());
        out.push_back(std::move(rs));
    }
}

// ------------------------------------------------------------------ BackupRule

void BackupRule::scan(const std::string &root,
                      std::vector<std::unique_ptr<Artifact>> &out) const {
    const fs::path base(root);
    std::error_code ec;
    std::string skip_dir;
    for (auto &ent : fs::recursive_directory_iterator(base, ec)) {
        if (ec) break;
        if (ent.is_directory(ec)) {
            if (is_skipped_dir(ent.path().filename().string()))
                skip_dir = ent.path().string();
            continue;
        }
        // 除外ディレクトリ配下は深さで弾く (この libc++ には disable_recursion_pending が無い)
        if (!skip_dir.empty() &&
            ent.path().string().rfind(skip_dir + "/", 0) == 0)
            continue;
        const std::string nm = ent.path().filename().string();
        const bool hit = ends_with(nm, "~") || ends_with(nm, ".orig") ||
                         ends_with(nm, ".rej") || ends_with(nm, ".swp") ||
                         nm.rfind(".#", 0) == 0;
        if (!hit) continue;

        auto bf = std::make_unique<BackupFile>();
        bf->path = fs::relative(ent.path(), base, ec).string();
        if (bf->path.empty()) bf->path = ent.path().string();
        bf->size = size_of(ent.path());
        bf->mtime = file_mtime(ent.path());
        out.push_back(std::move(bf));
    }
}

// ------------------------------------------------------------------ Scan

struct ArtifactScan::Impl {
    std::string root;
    std::vector<std::unique_ptr<ArtifactRule>> rules;
    std::vector<std::unique_ptr<Artifact>> artifacts;
};

ArtifactScan::ArtifactScan() : p_(new Impl) {}
ArtifactScan::~ArtifactScan() = default;

void ArtifactScan::use_default_rules() {
    add_rule(std::make_unique<CheckpointRule>());
    add_rule(std::make_unique<RestoreScriptRule>());
    add_rule(std::make_unique<PatchRule>());
    add_rule(std::make_unique<ArchiveRule>());
    add_rule(std::make_unique<BackupRule>());
}

void ArtifactScan::add_rule(std::unique_ptr<ArtifactRule> rule) {
    if (rule) p_->rules.push_back(std::move(rule));
}

void ArtifactScan::run(const std::string &root) {
    p_->root = root;
    p_->artifacts.clear();
    for (auto &r : p_->rules) r->scan(root, p_->artifacts);
}

std::vector<const Artifact *> ArtifactScan::affecting(const std::string &target) const {
    std::vector<const Artifact *> hit;
    for (auto &a : p_->artifacts)
        if (a->affects(target)) hit.push_back(a.get());
    // 重大なものから並べる
    std::sort(hit.begin(), hit.end(), [](const Artifact *x, const Artifact *y) {
        return (int)x->severity() > (int)y->severity();
    });
    return hit;
}

std::vector<const Artifact *> ArtifactScan::of_kind(const char *kind) const {
    std::vector<const Artifact *> hit;
    for (auto &a : p_->artifacts)
        if (std::strcmp(a->kind(), kind) == 0) hit.push_back(a.get());
    return hit;
}

const std::vector<std::unique_ptr<Artifact>> &ArtifactScan::all() const {
    return p_->artifacts;
}

size_t ArtifactScan::count() const { return p_->artifacts.size(); }

Severity ArtifactScan::max_severity() const {
    Severity m = Severity::Info;
    for (auto &a : p_->artifacts)
        if ((int)a->severity() > (int)m) m = a->severity();
    return m;
}

std::string ArtifactScan::verdict(const std::string &target) const {
    const auto hit = affecting(target);
    if (hit.empty()) {
        if (target.empty())
            return "残骸は見つかりませんでした (git 側だけを見れば復活の原因は別です)。";
        return "「" + target + "」を復活させうる残骸は見つかりませんでした。";
    }

    std::ostringstream os;
    os << "「" << target << "」が消しても復活するのは、git ではなく次の残骸が"
       << "復元のソースになっているためです (計 " << hit.size() << " 件)。\n";
    for (const Artifact *a : hit) {
        os << "  - [" << severity_name(a->severity()) << "] "
           << a->kind() << " : " << a->path
           << " → " << a->restore_mechanism() << "\n";
        for (const std::string &r : a->restores())
            if (r == target || ends_with(r, ("/" + target).c_str()))
                os << "      復活対象: " << r << "\n";
    }
    return os.str();
}

std::vector<std::string> ArtifactScan::removal_commands(const std::string &target) const {
    std::vector<std::string> cmds;
    const auto hit = affecting(target);
    std::set<std::string> dirs;

    // まずディレクトリ単位の削除を集める (rm -rf が済めば中の個別削除は不要)
    for (const Artifact *a : hit)
        if (std::strcmp(a->kind(), "checkpoint") == 0) dirs.insert(a->path);

    auto covered = [&](const std::string &path) {
        for (const std::string &d : dirs)
            if (path.rfind(d + "/", 0) == 0) return true;
        return false;
    };

    for (const Artifact *a : hit) {
        if (std::strcmp(a->kind(), "checkpoint") == 0) {
            cmds.push_back("rm -rf " + a->path);
            cmds.push_back("echo '" + a->path + "/' >> .gitignore");
        } else if (covered(a->path)) {
            continue;  // 親ディレクトリの rm -rf で消える
        } else {
            cmds.push_back("rm -f " + a->path);
        }
    }
    // 最後に、削除そのものを git で確定させる (HEAD から消す)
    if (!target.empty())
        cmds.push_back("git add -A -- " + target + " && git commit -m 'Remove " + target + "'");
    return cmds;
}

} // namespace nx
