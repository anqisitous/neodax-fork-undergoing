// nx_editor_model.hpp — code-reviewer の結果を「編集できるセル」に変換する。
//
// GUI 本体 (AppKit) から独立した純 C++。ファイル走査・fix map の読み取り・
// 関数/クラス範囲の検出だけを行う。描画もキー入力も持たない。
//
// 入力:
//   root       : ソースのルート
//   map        : `neodax-java-review <root> --map` の TSV (省略時は自動生成)
//
// 出力:
//   Node       : 直す必要があるノード (fix map の 1 行)
//   SourceFile : 番号で管理する元ファイル
//   Symbol     : 関数/クラスの定義ブロック (セル追加に使う)

#ifndef NX_EDITOR_MODEL_HPP
#define NX_EDITOR_MODEL_HPP

#include <string>
#include <vector>

namespace nxedit {

struct SourceFile {
    int id = 0;                         // 1 始まり。UI の「ファイル番号」
    std::string path;                   // root からの相対パス
    std::vector<std::string> lines;     // 改行を含まない
};

struct Node {
    std::string key;                    // task1 / stub / ...
    std::string name;                   // boids.model.rules.AlignmentRule.calculateForce
    std::string file;                   // src/.../AlignmentRule.java
    int line = 0;                       // 1 始まり
    int column = 0;                     // 1 始まり
    int body_start = 0;
    int body_end = 0;
    std::string contract;
    std::string expect;
    std::string confidence;             // high / medium / low
};

struct Symbol {
    enum Kind { Function, ClassLike };
    Kind kind = Function;
    std::string name;                   // 単純名
    std::string file;
    int start_line = 0;                 // 宣言行
    int end_line = 0;                   // 閉じ括弧の行
    int name_line = 0;
    int name_column = 0;                // 1 始まり
    int name_length = 0;
};

class Project {
public:
    // map_path が空なら `<root>` に対して java-review を実行して map を作る。
    // 失敗しても root のファイル一覧と symbol は使えるようにする。
    bool open(const std::string &root, const std::string &map_path, std::string &err);

    const std::string &root() const { return root_; }
    const std::vector<Node> &nodes() const { return nodes_; }
    const std::vector<SourceFile> &files() const { return files_; }
    const std::vector<Symbol> &symbols() const { return symbols_; }

    const SourceFile *file_by_id(int id) const;
    const SourceFile *file_by_path(const std::string &path) const;
    int file_id(const std::string &path) const;

    // 1 始まりの行範囲を含むテキスト。範囲外は空。
    std::string range_text(const std::string &file, int start_line, int end_line) const;
    std::string file_text(int id) const;

    // 1 始まりの行範囲を text で置き換える。セルの保存に使う。
    bool replace_lines(const std::string &file, int start_line, int end_line,
                       const std::string &text, std::string &err);
    bool write_file(const std::string &file, std::string &err);

    // 位置が関数/クラス名の「内側」ならその symbol。
    // 名前の端 (直前/直後) は含めない。要求の「隣接している輪郭を含めない」に対応。
    const Symbol *symbol_at(const std::string &file, int line, int column) const;

    // 位置の識別子 (関数呼び出し・型名を含む) を定義に解決する。
    // 同じファイルを優先し、無ければ同名の定義を探す。
    const Symbol *symbol_for_position(const std::string &file, int line, int column) const;

    // ノードを表示するときの範囲。body が無ければ宣言行から symbol を探す。
    void node_range(const Node &n, int &start_line, int &end_line) const;

    std::vector<Node> filtered(const std::string &query) const;

private:
    std::string root_;
    std::vector<SourceFile> files_;
    std::vector<Node> nodes_;
    std::vector<Symbol> symbols_;

    void scan_files();
    void scan_symbols();
    bool load_map_text(const std::string &text, std::string &err);
    std::string run_review_map(std::string &err) const;
};

// 1 行をタブで分ける。空要素も保持する。
std::vector<std::string> split_tab(const std::string &s);

// `path:line:column` を分ける。パスに ':' が無い前提 (macOS/Linux)。
bool parse_where(const std::string &s, std::string &file, int &line, int &column);

} // namespace nxedit

#endif
