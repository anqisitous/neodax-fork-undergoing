#include "nx_cli_registry.h"
#include <stdio.h>
#include <string.h>

typedef enum {
    K_OBSERVE = 0,
    K_JUDGE,
    K_REPAIR,
    K_ANALYZE,
    K_PROCEDURE,
    K_UI,
    K_KIND_COUNT
} nx_kind_t;

static const char *kKindName[K_KIND_COUNT] = {
    "観測", "判定", "修復", "解析", "手順", "画面"
};

typedef enum {
    O_STATE = 0,
    O_LIVE,
    O_ASSET,
    O_APP_PATH,
    O_INPUT,
    O_PICK,
    O_WRITE,
    O_EMIT,
    O_SCOPE,
    O_FILTER,
    O_FAMILY_COUNT
} nx_opt_family_t;

static const char *kFamilyName[O_FAMILY_COUNT] = {
    "state", "live", "asset", "app-path", "input", "pick",
    "write", "emit", "scope", "filter"
};

static const char *kFamilyOpts[O_FAMILY_COUNT][8] = {
    { "--state", NULL },
    { "--store", "--key", "--app", NULL },
    { "--dir", NULL },
    { "--path", NULL },
    { "--file", NULL },
    { "--id", "--symbol", "--kind", NULL },
    { "--apply", "--live", "--dry-run", "--force", NULL },
    { "--show", "--bake", "--keep", NULL },
    { "--depth", "--upto", "--step", "--limit", NULL },
    { "--include", "--within", "--context", "--match", "--action", NULL },
};

static int opt_family(const char *name) {
    for (int f = 0; f < O_FAMILY_COUNT; f++)
        for (int i = 0; kFamilyOpts[f][i]; i++)
            if (strcmp(kFamilyOpts[f][i], name) == 0) return f;
    return -1;
}

const nx_cli_cmd_t nx_cli_cmds[] = {
{ "windows",   "ウィンドウ",  "観測", "",                       "画面上のウィンドウを列挙",                    NULL, NULL },
{ "tabs",      "タブ一覧",    "観測", "--state",                "永続層のタブを TSV で出す",                   NULL, NULL },
{ "dump",      "生の状態",    "判定", "--state --key",         "解決された状態の生 JSON を出す (--key で任意キー)", NULL, NULL },
{ "keys",      "キー一覧",    "観測", "--state",                "KV のキー・seq・生存を C 側で列挙",            NULL, NULL },
{ "tab-keys",  "タブのキー",  "観測", "--state",                "タブが持つキーを列挙",                        NULL, NULL },
{ "threads",   "チャット台帳", "観測", "--db --list --deleted --within --apply", "チャットの重複を検出",               NULL, NULL },
{ "probe",     "レンダラ観測", "観測", "--app",                  "renderer の origin/キーを観測",               NULL, NULL },
{ "templates", "テンプレ群",  "観測", "",                       "保存済みテンプレートを列挙",                  NULL, NULL },
{ "apps",      "app一覧",     "観測", "",                       "保存済み app フォルダを表示",                  NULL, NULL },
{ "app",       "直前のapp",   "観測", "--path",                 "直前の app と保存フォルダを表示",              NULL, NULL },
{ "cut-status","cut の途中",  "観測", "",                       "チェックポイントの stage を表示",              NULL, NULL },

{ "advise",    "判定",        "判定", "--state --id --count",    "壊れているかだけ判定。変更しない",            NULL, NULL },
{ "check",     "閉じ残し",    "判定", "--state",                "(advise --count と同じ。統合済み)",            "advise", NULL },
{ "plan",      "計画",        "判定", "--state",                "事実 → 問題 → 計画",                          NULL, NULL },
{ "case",      "場合分け",    "判定", "--path --kind --case",   "条件ごとの分岐を出す",                        NULL, NULL },
{ "splice",    "観測→判定",   "判定", "--state --key --show --bake --keep", "観測を TSV で渡し検査コードに判定させる", NULL, NULL },
{ "trace",     "追跡",        "判定", "--dir --show --use-cut",  "収束点から辿り trace を焼いて走らせる",        NULL, NULL },
{ "resolve",   "資産を当てる","判定", "--state --key --apply",   "ノード資産を状態に当てて独立評価",            NULL, NULL },
{ "node",      "コマンドを呼ぶ","判定", "",                     "コマンドをノード経由で呼ぶ (実装を共有)",    NULL, NULL },
{ "arena",     "board 探索",  "判定", "--state --player-a --player-b --limit --depth", "2 人の player が board を進めて finish を競う", NULL, NULL },
{ "project",   "分析の保存",  "判定", "--dir --state --analysis --run --show --list --dry-run", "分析をプロジェクトに保存 (先に path、後で書き込み)", NULL, NULL },

{ "fix",       "アプリ修復",  "修復", "--path --target --state --app --store --key --apply --no-live --dry-run", "AE は recipe→run、DeepAI は状態修復を 1 発で",  NULL, NULL },
{ "apply",     "適用",        "修復", "--state --attach --app --store --key --id --live", "永続層へ反映",   NULL, NULL },
{ "clean",     "状態のみ",    "修復", "--state --no-live --dry-run --app --store --key", "LevelDB 状態だけを直す (アプリ全体は fix)",  NULL, NULL },
{ "delete",    "削除",        "修復", "--state --id --app --store --live --no-store-sync --force --all-stale --key", "1 件だけ消す (--all-stale でまとめて)",                     NULL, NULL },
{ "repair",    "修復実行",    "修復", "--state",                "観測 → 計画 → 実行 → 再観測 (4 往復)",        NULL, NULL },
{ "heal",      "復元",        "修復", "--state --app --key --dry-run", "検出→書き戻し→再水和 (clean と同じ)",  "clean", NULL },
{ "layout",    "整列",        "修復", "--app",                  "ウィンドウ配置を直す",                        NULL, NULL },
{ "baseline",  "基準",        "修復", "--app --key",            "基準状態を取る",                              NULL, NULL },
{ "stop",      "停止",        "修復", "--app --key",            "止める",                                      NULL, NULL },

{ "style",     "整形",        "解析", "--check --diff --write --mode --markers --outside", "連続する行を }{ で詰める。マーカーが無ければ //[ //] を付ける", NULL, NULL },
{ "seek",      "検索",        "解析", "--ignore-case --regex --binary --files --word --include --context --limit --time", "grep の代わり。範囲を切って探す", NULL, NULL },
{ "asar",      "asar",        "解析", "--file --grep --find --context --limit --extract --out", "asar の一覧・検索・取り出し",  NULL, NULL },
{ "tpl-cpp",   "テンプレート", "解析", "--dir --svg --record --ng", "cpp テンプレートの構成・annulus・学習",  NULL, NULL },
{ "ui-scan",   "UI 挙動抽出", "解析", "--path --dir --depth --include", "asar/JS から操作→コマンド→状態→永続を静的に抽出", NULL, NULL },
{ "ui-flow",   "UI アルゴリズム", "解析", "--dir --app --action --match --format --depth", "抽出済み ui.tsv から操作の流れを出す", NULL, NULL },
{ "search",    "探索",        "解析", "--path --symbol --probe-ms --apply", "候補を探す",                               NULL, NULL },
{ "replay",    "再生",        "解析", "--state",                "記録を再生する",                              NULL, NULL },
{ "simulate",  "模擬",        "解析", "--state --asar",         "状態を模擬する",                              NULL, NULL },
{ "spec",      "仕様",        "解析", "--hex --file --ws",          "構造を認識する (magic/check 付き spec)",       NULL, NULL },
{ "gen",       "cpp decoder", "解析", "--spec --class",             "spec から C++ デコーダクラスを生成",            NULL, NULL },
{ "pe",        "PE 解析",     "解析", "--file --disasm --limit",   "Windows PE の解析",                           NULL, NULL },
{ "pipeline",  "pipeline",    "解析", "--file --strategy --limit",   "decode+encode 戦略選択",                      NULL, NULL },

{ "cut",       "入口を外す",  "手順", "--path --symbol --abort --step --apply", "解析→保存→1段ずつ clone/patch/検証",   NULL, NULL },
{ "best",      "1個特定して最善", "手順", "--path --kind --apply",   "パスを入れて実行",                           NULL, NULL },
{ "app-analyze","app 分析",   "手順", "--path --fix",            "分析して保存",                                NULL, NULL },
{ "recipe",    "レシピ",      "手順", "--path --target",         "レシピを作る",                                NULL, NULL },
{ "recipe-show","レシピ表示", "手順", "--file",                  "レシピを表示",                                NULL, NULL },
{ "recipe-run", "レシピ実行", "手順", "--file --upto --apply",   "レシピを実行",                                NULL, NULL },
{ "advise-proc","手順導出",   "手順", "--state --apply",         "状態から修復手順を導出",                      NULL, NULL },
{ "strategy-memory","戦略記憶","手順", "--record-app --record-symbol --record-branch --record-ok", "分岐の成否を記録", NULL, NULL },
{ "close",     "閉じる",      "手順", "--state --id",            "surface を閉じる",                            NULL, NULL },
{ "steer",     "ステア",      "手順", "--app",                  "DeepAI に Cmd+Shift+Enter (steer) を送る",   NULL, NULL },
{ "kill",      "殺す",        "手順", "--match",                 "名前で止める",                                NULL, NULL },
{ "serve",     "待受",        "手順", "",                        "待ち受ける",                                  NULL, NULL },

{ "capture",   "撮影",        "画面", "--app --out",             "画面を撮る",                                  NULL, NULL },
{ "display",   "表示",        "画面", "--app --state",           "表示する",                                    NULL, NULL },
{ NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

const nx_cli_cmd_t *nx_cli_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; nx_cli_cmds[i].name; i++)
        if (strcmp(nx_cli_cmds[i].name, name) == 0) return &nx_cli_cmds[i];
    return NULL;
}

const nx_cli_cmd_t *nx_cli_resolve(const nx_cli_cmd_t *cmd) {
    int guard = 0;
    while (cmd && cmd->alias_of && guard++ < 8) {
        const nx_cli_cmd_t *next = nx_cli_find(cmd->alias_of);
        if (!next) break;
        cmd = next;
    }
    return cmd;
}

int nx_cli_takes(const nx_cli_cmd_t *cmd, const char *opt) {
    if (!cmd || !cmd->takes || !opt) return 0;
    size_t n = strlen(opt);
    const char *p = cmd->takes;
    while ((p = strstr(p, opt)) != NULL) {
        char after = p[n];
        if ((p == cmd->takes || p[-1] == ' ') && (after == 0 || after == ' ')) return 1;
        p += n;
    }
    return 0;
}

const char *nx_cli_val(const nx_cli_ctx *c, const char *name, const char *dflt) {
    if (!c || !name) return dflt;
    for (int i = 2; i < c->argc; i++)
        if (strcmp(c->argv[i], name) == 0 && i + 1 < c->argc) return c->argv[i + 1];
    return dflt;
}

int nx_cli_flag(const nx_cli_ctx *c, const char *name) {
    if (!c || !name) return 0;
    for (int i = 2; i < c->argc; i++)
        if (strcmp(c->argv[i], name) == 0) return 1;
    return 0;
}

int nx_cli_check_opts(const nx_cli_ctx *c) {
    if (!c || !c->cmd) return 0;
    int bad = 0;
    for (int i = 2; i < c->argc; i++) {
        const char *a = c->argv[i];
        if (a[0] != '-' || a[1] != '-') continue;
        if (nx_cli_takes(c->cmd, a)) continue;
        fprintf(stderr, "%s: 未知のオプション %s (このコマンドが取るのは: %s)\n",
                c->cmd->name, a, c->cmd->takes[0] ? c->cmd->takes : "(なし)");
        bad++;
    }
    return bad ? 1 : 0;
}

void nx_cli_usage(void *out_file, const char *group) {
    FILE *o = out_file ? (FILE *)out_file : stdout;
    for (int k = 0; k < K_KIND_COUNT; k++) {
        if (group && strcmp(group, kKindName[k]) != 0) continue;
        fprintf(o, "\n== %s ==\n", kKindName[k]);
        for (int i = 0; nx_cli_cmds[i].name; i++) {
            const nx_cli_cmd_t *e = &nx_cli_cmds[i];
            if (strcmp(e->group, kKindName[k]) != 0) continue;
            fprintf(o, "  %-14s %s\n", e->name, e->desc);
            if (e->takes[0]) fprintf(o, "                 %s\n", e->takes);
        }
    }
    if (!group) {
        fprintf(o, "\n== オプションの族 ==\n");
        for (int f = 0; f < O_FAMILY_COUNT; f++) {
            fprintf(o, "  %s:", kFamilyName[f]);
            for (int i = 0; kFamilyOpts[f][i]; i++) fprintf(o, " %s", kFamilyOpts[f][i]);
            fprintf(o, "\n");
        }
    }
}
