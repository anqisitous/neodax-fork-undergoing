/* nx_cli_registry.c — コマンドとオプションの一元化 (種別ごとにまとめる)
 *
 * 方針: **同じ種別同士だけを 1 箇所にまとめる。**
 *   種別 = 観測 / 判定 / 修復 / 解析 / 手順 / 画面
 * 種別が違うものを同じ表に混ぜない (混ぜると「何をする道具か」が読めなくなる)。
 *
 * コマンド表 nx_cli_cmds は種別ごとの区画に分かれ、区画の中だけが
 * 互いに関係する。オプションも同じ考えで、意味のある族ごとに
 * まとめて宣言する (nx_opt_family)。実装はここを見ない。
 */
#include "nx_cli_registry.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 種別。command も option もこの区画に沿って並べる。                  */
/* ------------------------------------------------------------------ */
typedef enum {
    K_OBSERVE = 0,  /* 観測: 状態を読むだけ。変更しない */
    K_JUDGE,        /* 判定: 良し悪しを決める。変更しない */
    K_REPAIR,       /* 修復: 永続層に書く */
    K_ANALYZE,      /* 解析: バイナリ・構造を見る */
    K_PROCEDURE,    /* 手順: AE などの段取り */
    K_UI,           /* 画面: 表示・撮影 */
    K_KIND_COUNT
} nx_kind_t;

static const char *kKindName[K_KIND_COUNT] = {
    "観測", "判定", "修復", "解析", "手順", "画面"
};

/* ------------------------------------------------------------------ */
/* オプションの族。
 *
 * 族に分ける基準は「見た目が似ているか」ではなく、**同じものを指すか**。
 * 下と同じ意味なら同じ族。違うものを指すなら別の族にする。
 *
 * 実測して確かめた同一性 (推測でまとめない):
 *   --state  leveldb のディレクトリ。9 コマンドで同じ使われ方
 *   --key    「そのキーを含むもの」を探す部分文字列。hint として同じ
 *   --app    稼働中ストアを見に行く際の app 名。同じ
 *   --store  ストアの一部を指す文字列。同じ
 *
 * 実測して確かめた相違 (まとめてはいけない):
 *   --dir は 2 か所で **違うもの** を指す:
 *            tpl-cpp --dir : cpp テンプレートの置き場所
 *            trace   --dir : 同上 (資産の探索先)
 *          → state ではない。leveldb の dir と混ぜない。
 *          (state 側は --state を使う。名前を揃えない)
 *   --path は app のパス (best/app-analyze/cut など) で、
 *          leveldb の dir ではない。
 *   --file は asar / pe / recipe の入力ファイル。
 *   --path と --file と --dir はどれも「対象」だが指すものが違う。
 *
 * したがって「対象」という 1 族にはしない。指すものごとに分ける。
 * ------------------------------------------------------------------ */
typedef enum {
    /* --- 状態のありか (leveldb) --- */
    O_STATE = 0,   /* --state  leveldb のディレクトリ */
    O_LIVE,        /* --store --key --app  稼働中ストアの探し方 */
    /* --- 対象のありか (状態ではない) --- */
    O_ASSET,       /* --dir   テンプレート資産の置き場所 */
    O_APP_PATH,    /* --path  app のパス */
    O_INPUT,       /* --file  入力ファイル (asar / pe / recipe) */
    O_PICK,        /* --id --symbol --kind  複数から 1 つ選ぶ */
    /* --- 振る舞い --- */
    O_WRITE,       /* --apply --live --dry-run --force  状態に書くかどうか */
    O_EMIT,        /* --show --bake --keep  成果物を出すか、どう残すか。
                     状態を書くのとは別の軸。--show は「実行しないで表示」で
                     あり、--dry-run の「書かないで計画だけ」とは違う。 */
    O_SCOPE,       /* --depth --upto --step --limit  どこまで */
    O_FILTER,      /* --include --within --context --match  絞る */
    O_FAMILY_COUNT
} nx_opt_family_t;

static const char *kFamilyName[O_FAMILY_COUNT] = {
    "state", "live", "asset", "app-path", "input", "pick",
    "write", "emit", "scope", "filter"
};

/* 族ごとのオプション名。指すものが同じものだけを同じ行に置く。 */
static const char *kFamilyOpts[O_FAMILY_COUNT][8] = {
    /* O_STATE    */ { "--state", NULL },
    /* O_LIVE     */ { "--store", "--key", "--app", NULL },
    /* O_ASSET    */ { "--dir", NULL },
    /* O_APP_PATH */ { "--path", NULL },
    /* O_INPUT    */ { "--file", NULL },
    /* O_PICK     */ { "--id", "--symbol", "--kind", NULL },
    /* O_WRITE    */ { "--apply", "--live", "--dry-run", "--force", NULL },
    /* O_EMIT     */ { "--show", "--bake", "--keep", NULL },
    /* O_SCOPE    */ { "--depth", "--upto", "--step", "--limit", NULL },
    /* O_FILTER   */ { "--include", "--within", "--context", "--match", NULL },
};

/* そのオプションが属する族。無ければ -1。 */
static int opt_family(const char *name) {
    for (int f = 0; f < O_FAMILY_COUNT; f++)
        for (int i = 0; kFamilyOpts[f][i]; i++)
            if (strcmp(kFamilyOpts[f][i], name) == 0) return f;
    return -1;
}

/* ------------------------------------------------------------------ */
/* コマンド表。種別ごとの区画。区画の中だけを読めば、その種別の      */
/* 道具が全部わかる。                                                 */
/* ------------------------------------------------------------------ */
const nx_cli_cmd_t nx_cli_cmds[] = {
/* --- 観測: 読むだけ ---
   keys / tab-keys は同じ「KV を列挙」だが、見る対象が違う
   (keys=全キー, tab-keys=タブが持つキー) ので別コマンドのまま。 */
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

/* --- 判定: 決めるだけ ---
   check は「advise の件数だけ版」。同じ detect を呼び、出す量が違うだけ
   なので advise に統合する (alias_of)。--count で件数だけになる。
   plan は advise とは別物: advise は 1 件の可否、plan は全体の事実→計画。
   case は条件ごとに分岐を出すので別物。
   splice / resolve / trace は同じ資産を扱うが、
      splice  = 観測 → 固定コードに渡す (データとコードの分離)
      resolve = 資産を状態に当てて評価 (書かない)
      trace   = 資産の形を辿る (状態を見ない)
   → 対象が違うので別コマンドのまま。 */
{ "advise",    "判定",        "判定", "--state --id --count",    "壊れているかだけ判定。変更しない",            NULL, NULL },{ "check",     "閉じ残し",    "判定", "--state",                "(advise --count と同じ。統合済み)",            "advise", NULL },
{ "plan",      "計画",        "判定", "--state",                "事実 → 問題 → 計画",                          NULL, NULL },
{ "case",      "場合分け",    "判定", "--path --kind --case",   "条件ごとの分岐を出す",                        NULL, NULL },
{ "splice",    "観測→判定",   "判定", "--state --key --show --bake --keep", "観測を TSV で渡し検査コードに判定させる", NULL, NULL },
{ "trace",     "追跡",        "判定", "--dir --show --use-cut",  "収束点から辿り trace を焼いて走らせる",        NULL, NULL },
{ "resolve",   "資産を当てる","判定", "--state --key --apply",   "ノード資産を状態に当てて独立評価",            NULL, NULL },
{ "node",      "コマンドを呼ぶ","判定", "",                     "コマンドをノード経由で呼ぶ (実装を共有)",    NULL, NULL },
{ "arena",     "board 探索",  "判定", "--state --player-a --player-b --limit --depth", "2 人の player が board を進めて finish を競う", NULL, NULL },
{ "project",   "分析の保存",  "判定", "--dir --state --analysis --run --show --list --dry-run", "分析をプロジェクトに保存 (先に path、後で書き込み)", NULL, NULL },

/* --- 修復: 書く ---
   fix   = アプリ全体の一発入口。AE は recipe→recipe-run、DeepAI は clean。
     clean = LevelDB 状態だけを直す (アプリ全体は fix)。
     heal  = 検出→書く→再水和 を一息でやる (clean の特殊形)
     repair= 4 往復して収束させる (clean と違って反復する)
   重なりが強いので、 heal は clean の別名にする
   (clean が selected=heal を出す。同じ経路を 2 回書かない)。
   repair は反復が本質なので独立のまま。 */
{ "fix",       "アプリ修復",  "修復", "--path --target --state --app --store --key --apply --no-live --dry-run", "AE は recipe→run、DeepAI は状態修復を 1 発で",  NULL, NULL },
{ "apply",     "適用",        "修復", "--state --attach --app --store --key --id --live", "永続層へ反映",   NULL, NULL },
{ "clean",     "状態のみ",    "修復", "--state --no-live --dry-run --app --store --key", "LevelDB 状態だけを直す (アプリ全体は fix)",  NULL, NULL },
{ "delete",    "削除",        "修復", "--state --id --app --store --live --no-store-sync --force --all-stale --key", "1 件だけ消す (--all-stale でまとめて)",                     NULL, NULL },
{ "repair",    "修復実行",    "修復", "--state",                "観測 → 計画 → 実行 → 再観測 (4 往復)",        NULL, NULL },
{ "heal",      "復元",        "修復", "--state --app --key --dry-run", "検出→書き戻し→再水和 (clean と同じ)",  "clean", NULL },
{ "layout",    "整列",        "修復", "--app",                  "ウィンドウ配置を直す",                        NULL, NULL },
{ "baseline",  "基準",        "修復", "--app --key",            "基準状態を取る",                              NULL, NULL },
{ "stop",      "停止",        "修復", "--app --key",            "止める",                                      NULL, NULL },

/* --- 解析: バイナリ・構造 ---
   asar --find と seek は「探す」だが、
     seek = リポジトリのソースを探す
     asar = asar の中を探す
   対象が違うので別のまま。 */
{ "seek",      "検索",        "解析", "--ignore-case --regex --binary --files --word --include --context --limit --time", "grep の代わり。範囲を切って探す", NULL, NULL },
{ "asar",      "asar",        "解析", "--file --grep --find --context --limit --extract --out", "asar の一覧・検索・取り出し",  NULL, NULL },
{ "tpl-cpp",   "テンプレート", "解析", "--dir --svg --record --ng", "cpp テンプレートの構成・annulus・学習",  NULL, NULL },
{ "search",    "探索",        "解析", "--path --symbol --probe-ms --apply", "候補を探す",                               NULL, NULL },
{ "replay",    "再生",        "解析", "--state",                "記録を再生する",                              NULL, NULL },
{ "simulate",  "模擬",        "解析", "--state --asar",         "状態を模擬する",                              NULL, NULL },
{ "spec",      "仕様",        "解析", "--hex --file --ws",          "構造を認識する (magic/check 付き spec)",       NULL, NULL },
{ "gen",       "cpp decoder", "解析", "--spec --class",             "spec から C++ デコーダクラスを生成",            NULL, NULL },
{ "pe",        "PE 解析",     "解析", "--file --disasm --limit",   "Windows PE の解析",                           NULL, NULL },
{ "pipeline",  "pipeline",    "解析", "--file --strategy --limit",   "decode+encode 戦略選択",                      NULL, NULL },

/* --- 手順: AE などの段取り ---
   best と app-analyze は対象が同じ app のパスなので近いが、
     best        = 1 個特定して最善を選ぶ
     app-analyze = 分析して保存する
   出力が違うので別のまま。 */
{ "cut",       "入口を外す",  "手順", "--path --symbol --abort --step --apply", "解析→保存→1段ずつ clone/patch/検証",   NULL, NULL },
{ "best",      "1個特定して最善", "手順", "--path --kind --apply",   "パスを入れて実行",                           NULL, NULL },
{ "app-analyze","app 分析",   "手順", "--path --fix",            "分析して保存",                                NULL, NULL },
{ "recipe",    "レシピ",      "手順", "--path --target",         "レシピを作る",                                NULL, NULL },
{ "recipe-show","レシピ表示", "手順", "--file",                  "レシピを表示",                                NULL, NULL },
{ "recipe-run", "レシピ実行", "手順", "--file --upto --apply",   "レシピを実行",                                NULL, NULL },
{ "advise-proc","手順導出",   "手順", "--state --apply",         "状態から修復手順を導出",                      NULL, NULL },
{ "strategy-memory","戦略記憶","手順", "--record-app --record-symbol --record-branch --record-ok", "分岐の成否を記録", NULL, NULL },
{ "close",     "閉じる",      "手順", "--state --id",            "surface を閉じる",                            NULL, NULL },
{ "kill",      "殺す",        "手順", "--match",                 "名前で止める",                                NULL, NULL },
{ "serve",     "待受",        "手順", "",                        "待ち受ける",                                  NULL, NULL },

/* --- 画面: 表示・撮影 --- */
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

/* 別名なら本体を返す。別名でなければそのまま返す。
   中身が同じコマンドを 2 つ持たないため、呼ぶ側は必ずこれを経る。 */
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

/* 宣言していないオプションを検出する。 */
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
