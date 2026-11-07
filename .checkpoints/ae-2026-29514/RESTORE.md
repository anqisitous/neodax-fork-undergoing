# AE 解析チェックポイント — 復元手順

**作成**: 2026-11-07 / ブランチ `vibe/structure-analysis`

## 保存したもの

| ファイル | 内容 |
|---|---|
| `tracked.patch` | 追跡ファイルの変更 (10ファイル, +403/−17) |
| `untracked.tar.gz` | 未追跡ファイル (ae/, gui/model/, track/, neodax-detect.c 他) |
| `untracked.list` | 上記のファイル一覧 |
| `ae-runtime.txt` | AE プロセス・バージョン情報 |
| `ae-state.txt` | AE の MRU プロジェクト・prefs 時刻 |

## AE の状態

- バージョン: **26.3.0** (`/Applications/Adobe After Effects 2026`)
- 直近プロジェクト: `~/Documents/After Effects Crash_5.aep` (62,769 B, 11/07 00:56)
- 注: AppleScript では AE のライブ状態を取得できなかった
  (`DoScriptFile` は常に 0 を返し、`File`/`$.writeln` 出力も不可)

## 復元手順

### コード側を元に戻す
```bash
cd /Users/kennichiroshimizu/Downloads/neodax-fork-undergoing
git checkout vibe/structure-analysis
git stash list                      # 念のため確認
git apply .checkpoints/ae-2026-29514/tracked.patch
tar -xzf .checkpoints/ae-2026-29514/untracked.tar.gz
```

### AE 側
1. AE を起動（クラッシュダイアログが出たら「閉じる」）
2. `~/Documents/After Effects Crash_5.aep` を開く
3. 必要なら `ae/create_comp.jsx` を DoScriptFile で実行

## 解析を再開するとき

「AE の解析を再開して」と伝えれば、このチェックポイントから続行します。

## 既知の制約

- `osascript` に画面キャプチャ・キーストローク権限がない
- `alert()` を AE 内で呼ぶと **AE がダイアログでブロック**するので使わないこと
- AE への状態問い合わせは、戻り値ではなく **一時ファイル経由** か
  **.aep を直接解析** する方が確実
