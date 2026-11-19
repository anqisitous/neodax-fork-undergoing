# DeepAI Code タブ復元の全解析

対象: DeepAI Code 0.3.41 (Electron 44.4.5)
状態: `~/Library/Application Support/deepai-code-v2/Local Storage/leveldb`
解析日: 2026-11-19

この文書は「消したタブが戻る」経路を、asar の中の実装・永続キー・
実データの 3 点で突き合わせた記録。生成物は同じ `analysis/` に置く。

---

## 0. 結論

戻る原因は LevelDB のバイナリでも asar のキャッシュでもない。

**`t3code:right-panel-state:v2` が surface の保持元で、
`workspace-tabs` はそこから毎回作り直される。**

```
t3code:right-panel-state:v2            ← 本当の保持元 (スレッドごとの surfaces)
        │  ThreadRouteView が byThreadKey[*].surfaces を平坦化
        ▼
surfaceTabs (WorkspaceTabs の b / ThreadRouteView の th)
        │  useEffect が include() → reconcile()
        ▼
deepai-code:workspace-tabs:v1          ← 表示用。消しても再構築される
```

したがって `workspace-tabs` だけを消す操作は、
**right-panel に死んだスレッドの entry が残っている限り必ず戻る。**
`delete --all-stale` が一瞬消えて翌朝戻ったのはこれ。

---

## 1. ストアの全体像

| localStorage キー | persist name | version | 何を持つか |
|---|---|---|---|
| `deepai-code:workspace-tabs:v1` | `deepai-code:workspace-tabs:v1` | 1 | `tabs` / `activeId` / `recentIds` |
| `t3code:right-panel-state:v2` | `t3code:right-panel-state:v2` | 14 | `byThreadKey` の `surfaces` ほか |
| `t3code:closed-views:v2` | `t3code:closed-views:v2` | 3 | 閉じた view の entries |
| `t3code:last-editor` | (persist でない) | - | `"file-manager"` |

実データは `analysis/state/` に生 JSON で保存した。

- `state/workspace_tabs.json`
- `state/right_panel.json`
- `state/closed_views.json`
- `state/last_editor.json`

---

## 2. asar 内の実装（chunk と関数）

asar: `/Applications/DeepAI Code.app/Contents/Resources/app.asar`

### 2.1 workspace-tabs ストア

`apps/server/dist/client/assets/FadeTruncate-D2BAc9UB.js`

- `create(...)` の actions:
  `removeThread` / `include` / `open` / `close` / `replace` /
  `reorder` / `promoteDraft` / `reconcile`
- 永続化:

```js
{
  name: `deepai-code:workspace-tabs:v1`,
  version: 1,
  storage: createJSONStorage(() => window.localStorage),
  partialize: ({tabs, activeId, recentIds}) => ({tabs, activeId, recentIds}),
  migrate: LZ,
  merge: (e, t) => ({...t, ...LZ(e)})
}
```

`reconcile(n)` は `tabs.filter(n)` で、条件に合わないタブを落とす。
`removeThread(threadRef)` は `deletedThreadKeys` にキーを足してから
`reconcile(e => !SSe(e.target, threadRef))` する。

### 2.2 surfaceTabs を作る場所

`apps/server/dist/client/assets/ThreadRouteView-DSg65wV-.js`

```js
eh = Xc(e => e.byThreadKey),
th = useMemo(() => Object.entries(eh).flatMap(([e, t]) => {
    let n = je(e);
    if (!n || e === P($c)) return [];
    return t.surfaces.map(e => ({
        target: {kind: `surface`, threadRef: n, surfaceId: e.id},
        icon: ...,
        title: ...
    }));
}), [...]);

// WorkspaceTabs へ:
surfaceTabs: th
```

`Xc` は `rightPanelStore` のストア。つまり `th` は
**right-panel の全スレッドの surfaces** をそのままタブ候補にしている。

### 2.3 タブへ再注入する場所

`apps/server/dist/client/assets/WorkspaceTabs-BHIRv4l6.js`

```js
useEffect(() => {
  if (!h || !g) return;
  let e = l.getState();                       // l = workspace-tabs store
  let t = e => e.kind === `surface` &&
               e.surfaceId.startsWith(`browser:`) &&
               (ee[i(e.threadRef)] ?? 0) > 0;

  for (let n of b)                            // b = surfaceTabs
    !t(n.target) && !v(n.target, e.deletedThreadKeys) && e.include(n.target);

  e.reconcile(n => {
    let r = n.target;
    return !v(r, e.deletedThreadKeys) && (
      r.kind !== `surface` ||
      t(r) ||
      b.some(e => it(e.target.threadRef, r.threadRef) &&
                  e.target.surfaceId === r.surfaceId)
    );
  });

  // split view 側の panes も同じ id で閉じる
  let n = new Set(l.getState().tabs.map(e => e.id));
  for (let e of H.getState().panes)
    e.target.kind === `surface` && !n.has(e.id) && H.getState().close(e.id);
}, [b, h, g, ee, N]);
```

- `b` に残っている target は `include()` で**無条件に追加**される。
- `reconcile()` は `b` に無い surface を落とす。
- つまり `b` (= right-panel の surfaces) が唯一の判定元。
- `deletedThreadKeys` はメモリのみ (`partialize` に含まれない)。

### 2.4 right-panel ストア

`apps/server/dist/client/assets/rightPanelStore-DHFiuyHr.js`

```js
x = `t3code:right-panel-state:v2`, S = 14;

removeThread: t => e(e => {
  let n = a(t);
  if (!(n in e.byThreadKey) &&
      !(n in e.threadPanelVisibilityByThreadKey) &&
      !(n in e.userActionRevisionByThreadKey) &&
      !(n in e.closeRevisionByThreadKey)) return e;
  let {[n]: r, ...i} = e.byThreadKey;
  let {[n]: o, ...s} = e.threadPanelVisibilityByThreadKey;
  let {[n]: c, ...l} = e.userActionRevisionByThreadKey;
  let {[n]: u, ...d} = e.closeRevisionByThreadKey;
  return {
    byThreadKey: i,
    threadPanelVisibilityByThreadKey: s,
    userActionRevisionByThreadKey: l,
    closeRevisionByThreadKey: d
  };
}),

{name: x, version: S, storage: localStorage,
 partialize: e => ({
   byThreadKey: Object.fromEntries(Object.entries(e.byThreadKey)
       .filter(([e]) => !w(e))),
   threadPanelVisibilityByThreadKey: ...
 })}
```

**`removeThread(threadRef)` が用意されている。**
これが「スレッドを消したときに right-panel を掃除する」本来の入口。
DeepAI は死んだスレッド（一覧に存在しないもの）に対しては
これを呼ばないので、entry が永久に残る。

---

## 3. 実データ（戻る直接の証拠）

`analysis/state/right_panel.json` の `state.byThreadKey`:

```json
{
  "28b4c9f1-...:336748f1-...": {
    "isOpen": true,
    "activeSurfaceId": "file:gui/neodax-viewer.mm",
    "surfaces": [
      {"id": "diff", "kind": "diff"},
      {"id": "file:gui/neodax-viewer.mm", "kind": "file", "relativePath": "gui/neodax-viewer.mm"}
    ]
  },
  "d3d0c88e-...:c362e560-...": {
    "isOpen": true,
    "activeSurfaceId": "file:/Users/.../Adobe After Effects 2026.app",
    "surfaces": [
      {"id": "file:/Users/.../Adobe After Effects 2026.app", "kind": "file"}
    ]
  },
  "d3d0c88e-...:73263324-...": {
    "isOpen": true,
    "activeSurfaceId": "file:README_UPSTREAM.md",
    "surfaces": [
      {"id": "file:src/integration/attach/nx_payload.c", "kind": "file"},
      {"id": "file:README_UPSTREAM.md", "kind": "file"}
    ]
  }
}
```

この 3 スレッドはタブ帯にもスレッド一覧にも存在しない。
`workspace-tabs` から該当 surface を消しても、この entry が
`surfaceTabs` に復活させ、`include()` がタブを再作成する。

---

## 4. 再現と、以前の削除が効かなかった理由

1. `delete --all-stale --live` で `workspace-tabs` から 5 件消す。
2. renderer の store は `tabs 7 -> 2` になり、ディスクも 2 になる。
3. その後、`WorkspaceTabs` の effect がもう一度走る
   （right-panel の `byThreadKey` は 5 件のまま）。
4. `for (n of b) e.include(n.target)` で 5 件が戻る。
5. `reconcile` は「`b` にある surface」を残すので、戻った 5 件は残る。

さらに、連続 `delete` では 2 つ目の基準がディスクの古い値になり、
1 つ目で消したタブを再び書き戻す競合があった（これは修正済み）。

---

## 5. 正しい消し方

死んだスレッド `T` に対して、**両方**を消す。

```
rightPanel.removeThread(T)     # byThreadKey + visibility + revisions
workspaceTabs.removeThread(T)  # deletedThreadKeys + reconcile
```

順序は right-panel を先。right-panel を消すと `b` が変わり、
`WorkspaceTabs` の effect が走って `reconcile` がタブを落とす。
`workspace-tabs` 側の `removeThread` は `deletedThreadKeys` を立てて
再注入を防ぐ保険。

永続化は両ストアの `localStorage.setItem` + `setState` + `rehydrate`。
（reload / 再起動はしない）

### 掃除対象になるキー

`a(threadRef)` = `"<environmentId>:<threadId>"`。
right-panel の 4 つの map すべてから同じキーを落とす。

- `byThreadKey`
- `threadPanelVisibilityByThreadKey`
- `userActionRevisionByThreadKey`
- `closeRevisionByThreadKey`

---

## 6. neodax の既存コマンドとの対応

| 役割 | コマンド |
|---|---|
| 任意キーの生値を読む | `dump --key t3code:right-panel-state:v2` |
| 永続層の鍵・seq・生死 | `keys` |
| タブ判定 | `advise` / `plan` / `splice` |
| 1 件削除 | `delete --id ID --live` |
| まとめて削除 | `delete --all-stale --live` |
| right-panel の復元を止める (既存) | `stop --key "t3code:right-panel-state:v2"` |
| right-panel を空の妥当な状態にする (既存) | `baseline --key "t3code:right-panel-state:v2"` |
| 分析の保存 | `project --run "tabs"` / `project --run "advise"` |
| **cpp decoder 生成** | `gen --spec all -o FILE` |
| 構造認識 | `spec --file FILE` |

`gen` と `spec` はレジストリにオプションが宣言されておらず
`rc=2` で止まっていたので、`--hex --file --ws` / `--spec --class` を
`gui/nx_cli_registry.c` に宣言した。

### 既存コマンドだけで止める場合

right-panel の **全スレッド分** を捨ててよければ、新しいコードは要らない。
`stop` は `--key` の前方一致で localStorage を消し、ストアを rehydrate する。

```sh
./bin/neodax-ui stop --key "t3code:right-panel-state:v2"
# → surfaceTabs が空になり、WorkspaceTabs の reconcile が
#   orphan surface を落とす。reload はしない。
```

`baseline --key ...` はキーを消さず、復元先を空の妥当な状態に置き換える。

**注意 (今回踏んだ罠):** `stop` は `append_store_resolve` でストアを
探すが、module hint を渡していなかったため、right-panel のような
別チャンクのストアを見つけられず `{"err":"no store","tried":1}` に
なっていた。`nx_appfix.c` の `nx_appfix_stop_restore` /
`nx_appfix_baseline_restore` で `derive_module_hint()` を呼ぶように
修正した。修正後の実行結果:

```
[appfix] store module: rightPanelStore-DHFiuyHr.js
[renderer] result: {"ok":true,"removed":[],"cleared":true,"prefix":"t3code:right-panel-state:v2"}
```

実行後、`dump --key t3code:right-panel-state:v2` は
`no live value` を返し、`tabs` は 2 件、`advise` は `VERDICT=NO`、
`splice` は `FINDINGS=0` になった。

### 死んだスレッドだけを消す場合

全スレッドの panel 状態を残したいなら、アプリ自身の `removeThread` と
同じ 4 つの map から該当キーだけを落とす（`workspace_right_panel.clean.cpp`
の `rp-prune`）。このとき right-panel を先に、workspace-tabs を後に書く。

### 追加した spec

- `json_persist`: `{"state":...,"version":N}` の汎用 zustand 封筒
- `right_panel_state`: `byThreadKey` + `threadPanelVisibilityByThreadKey`

これで `spec --file analysis/state/right_panel.json` が
`right_panel_state 40%` を返し、`gen` が C++ decoder を出す。

---

## 7. 生成物

```
analysis/
  RESTORE_CHAIN.md                      # この文書
  spec_report.txt                       # spec の認識結果
  nx_decoders_all.cpp                   # gen --spec all
  right_panel_state_decoder.cpp         # gen --spec right_panel_state
  persisted_envelope_decoder.cpp        # gen --spec json_persist
  workspace_tabs_decoder.cpp            # gen --spec json_state
  localstorage_record_decoder.cpp       # gen --spec leveldb_ls_record
  state/workspace_tabs.json             # 生データ
  state/right_panel.json                # 生データ (stop 前の戻る元)
  state/closed_views.json               # 生データ
  state/last_editor.json                # 生データ
src/templates/
  workspace_right_panel.clean.cpp       # right-panel を先に掃除するノード資産
```

変更した実装:

- `gui/nx_cli_registry.c` … `spec` / `gen` のオプション宣言
- `src/integration/spec/nx_specs_builtin.{c,h}` … `json_persist` / `right_panel_state`
- `src/integration/attach/nx_appfix.c` … stop/baseline の module hint

---

## 8. 判定に使った観測（要約）

- `tabs`: 7 件（orphan surface 5 件 + new-tab + chat）
- `dump --key t3code:right-panel-state:v2`: 死んだ 3 スレッド分の surfaces
- `spec --file analysis/state/right_panel.json`:
  `right_panel_state 40%`, `json_persist 40%`
- `keys`: `workspace-tabs` は live、`.nx-backup` は tombstone 済み
- `splice`: `FINDINGS=0`（workspace-tabs 単体では残滓なし）
- `clean`: `unresolved references` は workspace-tabs 上では 0

**workspace-tabs が健全でも戻る。** これが今回の核心。

---

## 9. プロジェクト化（ビルドで消えない）

解析結果は `analysis/project/` に保存した。ここは `bin/` の外なので、
`.app` を作り直しても残る。

```
analysis/project/
  project.tsv     kind/source
  analysis.tsv    ストア・死んだ参照・復元辺・検証の一覧
  history.tsv     保存履歴
```

資産（同じ内容をコード/データ/図で持つ）:

```
analysis/tab_restore.cpp       全ストア+コードの NX TEMPLATE (46 nodes / 30 edges)
analysis/tab_restore.svg       上の図 (解析用。アプリの表示は置き換えない)
analysis/PROJECT_ANALYSIS.tsv  project の材料
```

`gui/build.sh` は既存の `analysis/project` を消さない。無いときだけ
`analysis/PROJECT_ANALYSIS.tsv` から作り直す。実際に削除して再ビルドし、
同じ `analysis.tsv` が復元されることを確認済み。

```sh
./bin/neodax-ui project --dir analysis/project --show
./bin/neodax-ui tpl-cpp --dir analysis/tab_restore.cpp
./bin/neodax-ui trace   --dir analysis/tab_restore.cpp
```
