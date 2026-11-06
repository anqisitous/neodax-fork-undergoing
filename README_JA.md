# NeoDAX 日本語ガイド

> 🇬🇧 **[English README](README.md)** — 英語版の README はこちら

**マルチアーキテクチャ対応のバイナリ解析フレームワーク — 逆アセンブラ、CFG、デコンパイラ、エミュレータ**

C99 · 外部依存ゼロ · x86-64 · AArch64 · RISC-V · ELF · PE · Mach-O

- バージョン: 1.1.1（`package.json` 準拠）
- ライセンス: Apache-2.0
- 言語: C99 / JavaScript（Node.js ネイティブアドオン）/ Objective-C++（macOS GUI）
- 対応プラットフォーム: Linux · macOS · Android（Termux）
- リポジトリ: https://github.com/VersaNexusIX/NeoDAX
- npm: https://www.npmjs.com/package/neodax

---

## 概要

NeoDAX は、実行バイナリを静的・半動的に解析するためのツールキットです。C99 で書かれたコアエンジンは外部ライブラリに依存せず、ELF / PE / Mach-O の各フォーマットと x86-64 / AArch64 / RISC-V の各アーキテクチャを扱えます。

構成要素は次の 3 つです。

- **CLI (`neodax`)** — 逆アセンブル、関数検出、相互参照、CFG、デコンパイル、エミュレーションなどをコマンドラインで実行します。
- **JavaScript API / REST サーバー** — Node.js ネイティブアドオン（`js/neodax.node`）として同じエンジンを JS から利用でき、ローカル REST サーバーも同梱しています。
- **macOS GUI / 補助ツール** — Objective-C++ で書かれたネイティブビューア（`gui/neodax-viewer.mm`）、最小 CLI（`gui/neodax-ui.mm`）、解析対象ピッカー（`gui/neodax-picker.mm`）。

このほか、`src/` 以下には Mach-O 向けの静的解析フレームワーク（依存性追跡・実行順序予測・並列実行検出・状態追跡）が含まれています。詳細は [src/README.md](src/README.md) を参照してください。

---

## 特徴

### コア解析

| 機能 | フラグ | 説明 |
|---|---|---|
| 逆アセンブル | *(デフォルト)* | x86-64, AArch64, RISC-V RV64GC |
| セクション一覧 | `-l` | 仮想アドレス、ファイルオフセット、サイズ、フラグ、命令数 |
| 16 進バイト表示 | `-a` | 命令と並べて生バイトを表示 |
| シンボル解決 | `-y` | ELF symtab/dynsym、PE エクスポート、Mach-O nlist |
| C++ デマングル | `-d` | Itanium ABI |
| 関数検出 | `-f` | シンボル誘導 + ヒューリスティック（ELF, PE, Mach-O） |
| 命令グループ | `-g` | call/branch/ret/stack/syscall を色分け |
| 相互参照 | `-r` | call と branch の xref テーブル |
| 文字列参照 | `-t` | `.rodata` の文字列をインライン注釈 |
| CFG | `-C` | 2 パス方式。jump-trick や不透明述語（opaque predicate）を考慮 |
| ループ検出 | `-L` | 支配木解析による自然ループ検出 |
| コールグラフ | `-G` | 関数の呼び出し関係 |
| switch テーブル | `-W` | ジャンプテーブルのパターン検出 |
| Unicode 文字列 | `-u` | UTF-8 + UTF-16LE/BE（誤検出フィルタ付き） |
| 標準解析一式 | `-x` | 上記すべて |

### 高度な解析

| 機能 | フラグ | JS API | 対応アーキテクチャ |
|---|---|---|---|
| シンボリック実行 | `-P` | `.symexec(idx)` | ARM64, x86-64, RISC-V |
| NR リフティング | `-Q` | `.ssa(idx)` | ARM64, RISC-V |
| デコンパイラ（疑似 C） | `-D` | `.decompile(idx)` | ARM64, RISC-V |
| エミュレータ | `-I` | `.emulate(idx, regs)` | ARM64, RISC-V |
| エントロピー解析 | `-e` | `.entropy()` | すべて |
| 再帰下降逆アセンブル | `-R` | `.rda(section)` | ARM64, x86-64, RISC-V |
| 命令妥当性フィルタ（IVF） | `-V` | `.ivf()` | ARM64, x86-64, RISC-V |
| ポリモーフィック難読化マップ | `--poly` | — | すべて |
| DSA（全関数） | `--dsa` | — | ARM64, RISC-V |
| **AIRE** | `--aire` | — | すべて |
| VM ディスパッチトレース | `--vm-trace` | — | ARM64, x86-64, RISC-V |
| すべて | `-X` | — | — |

### 堅牢性（v1.1.1 以降）

| 機能 | 説明 |
|---|---|
| マイクロカーネル型の障害分離 | 32 個の解析パスはそれぞれ独立しており、1 つのパスが失敗しても他のパスは落ちない |
| `DAX_RUN_PASS` ラッパー | `main.c` 内のモジュール呼び出しは構造的なエラーから安全に復帰する |
| `DAX_GUARD_BIN` / `DAX_GUARD_FUNC` | すべての公開エントリポイントで、メモリアクセス前に入力を検証する |
| オーバーフロー安全なセクション演算 | `offset+size` の計算はすべて減算形式で行い、整数オーバーフローを防ぐ |
| カウンタの正規化 | 各ロードフェーズの後に `dax_clamp_counts()` で構造体のカウンタを正規化する |
| ループ予算ガード | デコードループは 1 関数あたり 65536 回で打ち切り、無限ループを防ぐ |

---

## 対応フォーマット / アーキテクチャ

|                | x86-64 | AArch64 | RISC-V RV64 |
|----------------|:------:|:-------:|:-----------:|
| **ELF64/32**   | ✓ | ✓ | ✓ |
| **PE64+/32**   | ✓ | ✓ | - |
| **Mach-O 64**  | ✓ | ✓ | - |
| **Mach-O FAT** | ✓ | ✓ | - |
| **Raw**        | ✓ | ✓ | ✓ |

---

## クイックスタート

### make（setup.sh 経由）

```bash
git clone https://github.com/VersaNexusIX/NeoDAX.git
cd NeoDAX
make
```
ルートの `make` は `setup.sh` を起動します。メニューから次のいずれかを選べます。

| メニュー | 内容 |
|---|---|
| **JS** | ネイティブコアをビルド（未ビルドの場合）したうえで、`build_js.sh` で JS アドオン `js/neodax.node` をビルド |
| **Code** | CLI バイナリをコンパイル（リポジトリ直下に `neodax`、Windows では `neodax.exe` を生成） |
| **Learn** | 学習用ドキュメントを閲覧（ブラウザで https://neo-dax.vercel.app/learning/ を開くか、ターミナルで `learn/` を閲覧） |

**Code** を選んでビルドしたら、次のように実行します。

```bash
./neodax <binary>          # 逆アセンブル
./neodax -h                # ヘルプ
./neodax -x ./binary       # 標準解析一式
./neodax -x -u <binary>    # 標準解析 + Unicode 文字列
```

`setup.sh` を直接実行しても同じメニューが表示されます。

```bash
bash setup.sh
```

### npm

git clone せずに npm からインストールすることもできます。

```bash
npm install neodax
```

`npm install` の際にネイティブアドオンが自動でコンパイルされます（`js/scripts/install.js`）。

```js
const neodax = require('neodax');

neodax.withBinary('/path/to/binary', bin => {
    console.log(bin.arch, bin.sha256);
    console.log(bin.sections().length, 'sections');
    console.log(bin.functions().length, 'functions');
});
```

### Android / Termux

```bash
pkg install nodejs clang make git
git clone https://github.com/VersaNexusIX/NeoDAX.git
cd NeoDAX && make
```

---

## ビルド方法

### 必要環境

- C99 コンパイラ: **GCC 7 以上** または **Clang 6 以上**（`setup.sh` は `clang` → `gcc` → `cc` の順に探索）
- **GNU make**（BSD では `gmake`）
- 外部ライブラリは不要
- JS アドオン / npm を使う場合: **Node.js 16 以上**（`node_api.h` などのヘッダが必要）
- GUI を使う場合: **macOS**（Cocoa / CoreGraphics を使用）と `clang++`

### ビルド経路の一覧

| コマンド | 対象 | 出力 |
|---|---|---|
| `make`（= `bash setup.sh`） | CLI 本体 / JS アドオン / Learn | `./neodax`（Code 選択時）、`js/neodax.node`（JS 選択時） |
| `bash build_js.sh` | JS ネイティブアドオンのみ | `js/neodax.node` |
| `npm run build` / `npm run rebuild` | JS アドオン（`js/scripts/build.js`、`rebuild` は `--force`） | `js/neodax.node` |
| `make -C src framework` | Mach-O 静的解析フレームワーク | `bin/neodax_static`, `bin/neodax_static_integrated` |
| `bash gui/build.sh` | macOS ネイティブビューアと最小 CLI | `bin/neodax-viewer`, `bin/neodax-ui` |
| `make clean` | 生成物の削除 | `src/*.o`, `arch/*.o`, `neodax`, `neodax.exe`, `js/neodax.node` を削除 |

### 注意点

- **`make -C src`（`all` ターゲット）は現在ビルドを行いません。** 旧来のビルド経路であるため、案内メッセージを表示してスキップします。CLI はルートの `make`（`setup.sh`）、GUI は `bash gui/build.sh` を使ってください。解析フレームワークを単体でビルドしたい場合のみ `make -C src framework` を使います。
- **`bash gui/build.sh` は macOS 専用です。** Cocoa / CoreGraphics / ImageIO / ApplicationServices フレームワークにリンクするため、Linux などでは動作しません。生成物は `bin/` 以下に出力されます。
  - `bin/neodax-viewer` — 解析器をプロセス内にリンクしたネイティブビューア（関数レベルのコールグラフを階層表示）
  - `bin/neodax-ui` — GUI とは独立した別プロセスで動く最小 CLI（ウィンドウ一覧、画面キャプチャ、タブ状態の出力など）
- `gui/neodax-picker.mm`（解析対象ピッカー）は `gui/build.sh` ではビルドされません。ファイル先頭のコメントにあるとおり、単体で `clang++ -std=c++17 -ObjC++ -framework Cocoa -framework CoreServices neodax-picker.mm -o neodax-picker` のようにビルドします。

---

## 主要 CLI フラグ

| フラグ | 説明 |
|------|-------------|
| `-a` | 16 進バイトを表示 |
| `-s <sec>` | 対象セクション（デフォルトは `.text`） |
| `-S` | すべての実行可能セクション |
| `-A / -E` | 開始 / 終了アドレス（16 進） |
| `-l` | セクション一覧 |
| `-n` | カラー表示なし |
| `-v` | 詳細出力 |
| `-y` | シンボル解決 |
| `-d` | C++ デマングル |
| `-f` | 関数検出 |
| `-g` | 命令グループの色分け |
| `-r` | 相互参照の注釈 |
| `-t` | 文字列参照の注釈 |
| `-C` | 制御フローグラフ（CFG） |
| `-L` | ループ検出 |
| `-G` | コールグラフ |
| `-W` | switch / ジャンプテーブル検出 |
| `-u` | Unicode 文字列スキャン |
| `-P` | シンボリック実行 |
| `-Q` | NR リフティング（NeoDAX Representation — 関数間 IR） |
| `-D` | 疑似 C へのデコンパイル（型推論 + 呼び出し解決） |
| `-I` | 関数のエミュレーション（ARM64, RISC-V） |
| `-e` | エントロピー解析 |
| `-R` | 再帰下降逆アセンブル |
| `-V` | 命令妥当性フィルタ |
| `--poly` | ポリモーフィック難読化マップ |
| `--dsa` | DSA — Dynamic Single Assignment（全関数） |
| `--aire` | AIRE 解析（文脈考慮・ガイド付き・メモリ付き） |
| `--vm-trace` | エミュレーション後に VM ディスパッチトレースを出力 |
| `-x` | 標準解析一式 |
| `-X` | すべて（`--poly --aire --vm-trace --dsa` を含む） |
| `-i` | 対話シェル（REPL） |
| `-eox <folder>` | フォルダからプラグインを読み込む |
| `-eox-list` | 読み込んだプラグインの一覧 |
| `-o <file>` | `.daxc` スナップショットを保存 |
| `-c` | `.daxc` → `.S` に変換 |
| `-h` | ヘルプ |

すべてのフラグと使用例は [CLI_REFERENCE.md](CLI_REFERENCE.md) を参照してください。

---

## JavaScript API の例

ソースからビルドした場合は `require('./js')`、npm でインストールした場合は `require('neodax')` で読み込みます。TypeScript の型定義（`js/index.d.ts`）が同梱されているため、`@types/neodax` は不要です。

```js
const neodax = require('neodax');

// withBinary: コールバック終了時に自動で close
neodax.withBinary('/path/to/binary', bin => {
    console.log(bin.arch);        // 例: 'AArch64 (ARM64)'
    console.log(bin.sha256);      // SHA-256 ダイジェスト
    console.log(bin.isPie);       // true / false

    const r = bin.analyze();      // symbols → functions → xrefs → CFG → unicode を一括実行
    console.log(r.functions.length, 'functions,', r.xrefs.length, 'xrefs');

    console.log(bin.decompile(0)); // 疑似 C（ARM64）
    console.log(bin.entropy());    // パック / 暗号化領域の検出
});

// 手動でライフサイクルを管理
const bin = neodax.load('/path/to/binary');
console.log(bin.info);
bin.close();

// 非同期版
await neodax.withBinaryAsync('./binary', async bin => {
    const r = bin.analyze();
    return r.functions.length;
});
```

### 主なメソッド

| 分類 | メソッド |
|---|---|
| 解析 | `sections()`, `symbols()`, `functions()`, `xrefs()`, `xrefsTo(addr)`, `xrefsFrom(addr)`, `blocks()`, `unicodeStrings()`, `strings()`, `analyze()` |
| 検索 | `symAt(addr)`, `funcAt(addr)`, `sectionByName(name)`, `sectionAt(addr)`, `readBytes(addr, len)`, `hottestFunctions(n?)` |
| 逆アセンブル | `disasm(section?)`, `disasmFunc(...)`, `disasmRange(...)`, `disasmJson(section?, opts)` |
| 高度な解析 | `symexec(idx?)`, `ssa(idx?)`, `decompile(idx?)`, `emulate(idx?, initRegs?)`, `emulateAt(...)`, `emulateByName(...)`, `cfg()`, `cfgJson()`, `callgraph()`, `callgraphJson()`, `loops()`, `switchDetect()` |
| 検出 | `entropy()`, `rda(section?)`, `ivf()`, `poly()`, `polyJson()`, `aire()`, `aireJson()`, `vmDetect()`, `vmTrace()`, `obfuscationScore()` |
| 保存・注釈 | `saveDaxc(file)`, `addComment(...)` |
| ライフサイクル | `close()`, `[Symbol.dispose]()`（TypeScript 5+ の `using` に対応） |

モジュール関数: `neodax.load(path)`, `neodax.version()`, `neodax.withBinary(path, cb)`, `neodax.withBinaryAsync(path, cb)`

エミュレーションでは初期レジスタを指定できます。

```js
bin.emulate(0, { '0': 42n, '1': 7n });
```

サンプルスクリプトは `js/examples/` にあります。

```bash
node js/examples/01_binary_info.js /bin/ls
node js/examples/06_full_analysis.js /bin/ls
```

詳細は [js/README.md](js/README.md) と [NPM_USAGE.md](NPM_USAGE.md)（Express, Fastify, Docker, TypeScript の例）を参照してください。

---

## REST サーバーと Web UI

### Web UI

**[neo-dax.vercel.app](https://neo-dax.vercel.app)** からインストール不要で利用できます。

16 個の解析パネル: Overview · Sections · Symbols · Functions · CFG Blocks · Xrefs · Strings · Unicode · Disassembly · Decompiler · NR (NeoDAX Representation) · Symbolic Execution · Emulator · Entropy · Recursive Descent · Validity Filter

### ローカル REST サーバー

手元のバイナリを REST API 経由で解析する場合は、ローカルサーバーを起動します。

```bash
node js/server/server.js          # http://localhost:7070
PORT=8080 node js/server/server.js
# または
npm start
```

デフォルトのポートは **7070** です。すべてのエンドポイントは `POST` で、`Content-Type: application/json` と `{ "file": "/absolute/path" }` を送ります。

```bash
curl -X POST http://localhost:7070/api/info \
     -H 'Content-Type: application/json' \
     -d '{"file":"/bin/ls"}'
```

| パス | 追加フィールド | 説明 |
|------|----------------|------|
| `/api/info` | — | バイナリのメタデータ |
| `/api/sections` | — | セクションテーブル |
| `/api/symbols` | — | シンボルテーブル |
| `/api/functions` | — | 関数一覧 |
| `/api/xrefs` | — | すべての xref |
| `/api/xrefs-to` | `address` | 指定アドレスへの xref |
| `/api/xrefs-from` | `address` | 指定アドレスからの xref |
| `/api/blocks` | — | CFG ブロック |
| `/api/cfg`, `/api/cfg/json` | — | CFG |
| `/api/callgraph`, `/api/callgraph/json` | — | コールグラフ |
| `/api/loops` | — | ループ検出 |
| `/api/switch-detect` | — | switch / ジャンプテーブル検出 |
| `/api/unicode` | — | Unicode 文字列 |
| `/api/strings` | — | ASCII 文字列 |
| `/api/disasm` | `section?` | テキスト形式の逆アセンブル |
| `/api/disasm/func` | `funcIdx?` | 関数単位の逆アセンブル |
| `/api/disasm/range` | `address`, `length` | アドレス範囲の逆アセンブル |
| `/api/disasm/json` | `section?`, `group?`, `limit?` | 構造化された逆アセンブル |
| `/api/analyze` | — | フル解析パイプライン |
| `/api/sym-at` | `address` | アドレスのシンボル |
| `/api/func-at` | `address` | アドレスの関数 |
| `/api/section-at` | `address` | アドレスのセクション |
| `/api/hottest` | `n?` | ホットな関数 |
| `/api/read-bytes` | `address`, `length` | 生バイト |
| `/api/symexec` | `funcIdx?` | シンボリック実行 |
| `/api/ssa` | `funcIdx?` | SSA 形式 |
| `/api/decompile` | `funcIdx?` | 疑似 C |
| `/api/emulate` | `funcIdx?`, `initRegs?` | 具象エミュレーション |
| `/api/entropy` | — | エントロピー解析 |
| `/api/rda` | `section?` | 再帰下降逆アセンブル |
| `/api/ivf` | — | 命令妥当性フィルタ |
| `/api/poly`, `/api/poly/json` | — | ポリモーフィック難読化マップ |
| `/api/aire`, `/api/aire/json` | — | AIRE 解析 |
| `/api/vm-detect`, `/api/vm-trace` | — | VM 検出 / ディスパッチトレース |
| `/api/obfuscation-score` | — | 難読化スコア |
| `/api/comment` | — | コメントの追加 |
| `/api/save-daxc` | `file` | `.daxc` スナップショットの保存 |

`address` は `"0x..."` 形式の 16 進文字列で指定します。レスポンス中の `bigint` 値も `"0x..."` 形式の文字列で返されます。

---

## ディレクトリ構成

```
.
├── arch/       アーキテクチャ / OS 別のアセンブリスタブ (x86_64, arm64, riscv64)
├── gui/        macOS 用 Objective-C++ ソースとビルドスクリプト
│   ├── build.sh            neodax-viewer / neodax-ui をビルド (macOS 専用)
│   ├── neodax-viewer.mm    ネイティブビューア (AppKit + Core Graphics)
│   ├── neodax-ui.mm        GUI とは独立した最小 CLI
│   └── neodax-picker.mm    解析対象ピッカー
├── include/    公開ヘッダ (dax.h, elf.h, pe.h, macho.h, x86.h, arm64.h, riscv.h など)
├── js/         Node.js ネイティブアドオン + REST サーバー
│   ├── src/                N-API バインディング (neodax_napi.c)
│   ├── server/             REST サーバー (server.js, ui.html)
│   ├── examples/           サンプルスクリプト
│   ├── scripts/            install.js / build.js
│   ├── test/               basic.js
│   ├── index.js            エントリポイント
│   └── index.d.ts          TypeScript 型定義
├── learn/      学習用ドキュメント (Markdown)
├── src/        C99 コアエンジン + Mach-O 静的解析フレームワーク
│   ├── core/  analysis/  flow/  tracking/  integration/
│   └── Makefile            旧ビルド経路 (`make -C src framework`)
├── bin/        ビルド生成物 (neodax_static, neodax_static_integrated,
│               neodax-viewer, neodax-ui) ※ビルド時に作成
├── Makefile    ルート Makefile (setup.sh を起動)
├── setup.sh    対話式ビルドスクリプト
├── build_js.sh JS アドオン単体ビルド
└── package.json
```

---

## ドキュメント一覧

| ファイル | 内容 |
|------|-------------|
| [BUILDING.md](BUILDING.md) | 各プラットフォームでのビルド手順 |
| [NPM_USAGE.md](NPM_USAGE.md) | npm 依存として NeoDAX を使う方法 |
| [PUBLISHING.md](PUBLISHING.md) | CI/CD と npm 公開の手順 |
| [CLI_REFERENCE.md](CLI_REFERENCE.md) | CLI の完全なリファレンスと使用例 |
| [API.md](API.md) | C API リファレンス（障害分離 API を含む） |
| [ARCHITECTURE.md](ARCHITECTURE.md) | 内部モジュール設計と障害分離モデル |
| [ALGORITHMS.md](ALGORITHMS.md) | CFG、エントロピー、RDA、NR のアルゴリズム |
| [js/README.md](js/README.md) | JavaScript API と REST サーバー |
| [MACHO_SUPPORT.md](MACHO_SUPPORT.md) | Mach-O / macOS フォーマットの詳細 |
| [DECOMPILER.md](DECOMPILER.md) | NR（NeoDAX Representation）と疑似 C デコンパイラ |
| [EMULATOR.md](EMULATOR.md) | ARM64 / RISC-V 具象エミュレータ |
| [EXAMPLES.md](EXAMPLES.md) | 使用例（CLI, JS, REST） |
| [FAQ.md](FAQ.md) | よくある質問 |
| [OBFUSCATION.md](OBFUSCATION.md) | 難読化・パックされたバイナリの解析 |
| [UNICODE_DETECTION.md](UNICODE_DETECTION.md) | Unicode スキャナの設計 |
| [FORMAT_DAXC.md](FORMAT_DAXC.md) | `.daxc` スナップショット形式の仕様 |
| [PERFORMANCE.md](PERFORMANCE.md) | ベンチマークと最適化のメモ |
| [FUZZING.md](FUZZING.md) | AFL++、libFuzzer、ASAN のガイド |
| [INTEGRATION.md](INTEGRATION.md) | VS Code、Docker、Python、CI との連携 |
| [SECURITY.md](SECURITY.md) | 脆弱性の報告と多層防御モデル |
| [TROUBLESHOOTING.md](TROUBLESHOOTING.md) | ビルド時・実行時のトラブルシューティング |
| [PORTING.md](PORTING.md) | 新しいアーキテクチャ / フォーマットへの移植 |
| [CICD_GUIDE.md](CICD_GUIDE.md) | GitHub Actions ワークフローの解説 |
| [CONTRIBUTING.md](CONTRIBUTING.md) | コントリビューションガイドライン |
| [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) | 行動規範 |
| [CHANGELOG.md](CHANGELOG.md) | リリース履歴 |

日本語のドキュメント:

| ファイル | 内容 |
|------|-------------|
| [src/README.md](src/README.md) | Mach-O 静的解析フレームワークの概要（日本語） |
| [src/README_JA.md](src/README_JA.md) | 静的解析フレームワークの詳細ガイド（日本語） |
| [src/SETUP_GUIDE_JA.md](src/SETUP_GUIDE_JA.md) | 静的解析フレームワークの仮セットアップガイド（日本語） |

---

## ライセンス

Apache License 2.0 — 詳細は [LICENSE](LICENSE) を参照してください。

Built by [VersaNexusIX](https://github.com/VersaNexusIX)
