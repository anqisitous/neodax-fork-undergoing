// search.hpp — neodax の検索になるネイティブ検索 (C++ クラス設計)
//
// 目的:
//   状態ディレクトリやソースを「外部ツールを使わずに neodax で」調べる。
//   検索語・対象・出力規則をクラスに分け、CLI は nx_seek() だけを見る。
//
// クラス:
//   SeekOptions  検索の指定 (語・正規表現・大小・対象拡張子・上限)
//   TextQuery    1 行が検索語に一致するかを判定する (literal / regex)
//   FileFilter   走査対象のファイルかを決める (拡張子・バイナリ・大きさ)
//   Corpus       パスを列挙する (ディレクトリ再帰・symlink を辿らない)
//   Searcher     走査して一致行を出力する (文脈・件数/時間の上限つき)
//
// 出力形式は seek と同じ "path:line: text"。
// 解析結果ではなく「事実の列挙」だけを行う。判定は呼び出し側に残す。

#ifndef NX_SEARCH_HPP
#define NX_SEARCH_HPP

#include <cstdio>
#include <regex>
#include <string>
#include <vector>

namespace nx {

// 検索の指定
struct SeekOptions {
    std::string pattern;
    std::string root = ".";
    std::string includes;        // "*.txt,*.plist" (空なら全部)
    bool ignore_case = false;
    bool regex_mode = false;
    bool word = false;           // 単語境界
    bool binary = false;         // バイナリも走査する
    bool files_only = false;     // 一致したファイル名だけ
    int  context = 0;            // 前後の行数
    int  limit = 1000;           // 一致行の上限 (0 で無制限)
    int  time_ms = 15000;        // 走査時間の上限
    int  max_file_mb = 64;       // 1 ファイルの上限
    int  max_depth = 16;         // 再帰の深さ

    /* A: 停止条件を先に固定する */
    long long max_total_bytes = 512LL * 1024 * 1024;  // 全体の読み取り上限
    int  max_line_bytes = 65536; // 1 行の評価上限 (regex の入力を有界に)
    int  regex_deadline_ms = 2000; // regex 1 回の目安 (超過は行長で抑制)
};

// 1 件の一致
struct SeekHit {
    std::string path;
    long        line = 0;        // 1 始まり
    std::string text;
    size_t      col = 0;         // 行内の位置
};

// 1 行の一致判定。literal と regex を同じ顔で扱う。
class TextQuery {
public:
    explicit TextQuery(const SeekOptions &o);
    bool ok() const { return error_.empty(); }
    const std::string &error() const { return error_; }
    bool match(const std::string &line, size_t *col) const;
    // 大きなファイルを分割する前に、一致し得るかを安く調べる。
    // literal 以外は常に true (regex は行ごとに見る)。
    bool prefilter(const std::string &data) const;

private:
    bool literal_ = true;
    std::string needle_;         // literal: 比較用に大小を揃えた語
    std::string lit_frag_;       // regex でも使える literal 断片 (prefilter 用)
    bool has_lit_ = false;
    std::string error_;
    bool ignore_case_ = false;
    bool word_ = false;
    std::regex re_;
};

// 走査対象の選別。拡張子とバイナリの扱いをここに閉じる。
class FileFilter {
public:
    explicit FileFilter(const SeekOptions &o);
    bool accept_path(const std::string &path) const;
    bool accept_content(const std::string &head) const;
    bool too_large(long size) const { return size > max_bytes_; }

private:
    std::vector<std::string> globs_;
    bool binary_ = false;
    long max_bytes_ = 0;
};

// ディレクトリを再帰的に列挙する。symlink は辿らない (.git は除く)。
class Corpus {
public:
    explicit Corpus(int max_depth) : max_depth_(max_depth) {}
    std::vector<std::string> collect(const std::string &root) const;

private:
    int max_depth_;
};

// 走査と出力。状態は持たない (Searcher 1 回で 1 走査)。
class Searcher {
public:
    Searcher(const SeekOptions &o, FILE *out);
    int run();

private:
    void scan_file(const std::string &path, long long deadline_ms);
    void emit(const SeekHit &hit) const;
    bool timed_out(long long deadline_ms) const;

    SeekOptions opt_;
    TextQuery   query_;
    FileFilter  filter_;
    FILE       *out_;
    long        files_ = 0;
    long        matched_ = 0;
    long        hits_ = 0;
    long        skipped_binary_ = 0;
    long        skipped_stat_ = 0;      // 権限/消失などで読めなかった B の可視化
    long        line_truncated_ = 0;    // 行長上限で評価を切った行数
    long long   bytes_read_ = 0;        // A: 全体の読み取り量
    bool        truncated_ = false;
    bool        time_up_ = false;
    bool        bytes_up_ = false;
};

} // namespace nx

#endif
