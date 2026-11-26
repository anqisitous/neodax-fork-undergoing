#!/bin/sh
# トークンを env / リポジトリ内 / keychain から自動で選び、その場限りの
# askpass で push する。credential.helper には何も保存しない。
#
#   GIT_PUSH_TOKEN=ghp_xxx ./git-push-token.sh HEAD:main
#   ./git-push-token.sh main
#
# トークンの探索順:
#   1. GIT_PUSH_TOKEN / GH_TOKEN / GITHUB_TOKEN
#   2. <git-dir>/neodax-token          (リポジトリ専用。chmod 600)
#   3. ~/.config/neodax/github-token   (ユーザー共通。chmod 600)
#   4. macOS keychain (github.com)
#
# push 前整形は、対象リポジトリで `git config neodax.styleOnPush` を
# 設定したときだけ動く (true=変更ファイル / all=全ソース)。
# まだ //[ //] が無いファイルには、整形時にマーカーを付ける。
set -eu

refspec=${1:-HEAD}

load_token() {
    if [ -n "${GIT_PUSH_TOKEN:-}" ]; then printf '%s\n' "$GIT_PUSH_TOKEN"; return; fi
    if [ -n "${GH_TOKEN:-}" ]; then printf '%s\n' "$GH_TOKEN"; return; fi
    if [ -n "${GITHUB_TOKEN:-}" ]; then printf '%s\n' "$GITHUB_TOKEN"; return; fi

    gitdir=$(git rev-parse --git-dir 2>/dev/null || true)
    for f in "${gitdir:-/nonexistent}/neodax-token" \
             "${HOME}/.config/neodax/github-token"; do
        if [ -r "$f" ]; then
            t=$(tr -d '\r\n' < "$f")
            if [ -n "$t" ]; then printf '%s\n' "$t"; return; fi
        fi
    done

    if command -v security >/dev/null 2>&1; then
        security find-internet-password -s github.com -w 2>/dev/null || true
    fi
}

style_before_push() {
    case "$refspec" in
        refs/tags/*) return 0 ;;
    esac
    mode=$(git config --get neodax.styleOnPush 2>/dev/null || true)
    [ -n "$mode" ] || return 0
    here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
    style="$here/bin/neodax-style"
    if [ ! -x "$style" ]; then
        make -s -C "$here/src" ../bin/neodax-style >/dev/null 2>&1 || return 0
    fi
    [ -x "$style" ] || return 0

    # 未コミットの変更と混ぜない。整形してからコミットし、再度 push する。
    if [ -n "$(git status --porcelain 2>/dev/null || true)" ]; then
        echo "neodax-style: working tree が汚れているため push 前整形を省略" >&2
        return 0
    fi

    list=$(mktemp "${TMPDIR:-/tmp}/neodax-style-files.XXXXXX")
    trap 'rm -f "$list"' EXIT INT TERM
    if [ "$mode" = "all" ]; then
        git ls-files > "$list" 2>/dev/null || true
    else
        base=$(git rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>/dev/null || true)
        [ -n "$base" ] || base=origin/main
        git diff --name-only --diff-filter=ACMR "$base"...HEAD > "$list" 2>/dev/null || true
        if [ ! -s "$list" ]; then
            git show --pretty='' --name-only HEAD > "$list" 2>/dev/null || true
        fi
    fi
    [ -s "$list" ] || return 0

    # 1 ファイルずつ渡す (空白入りパスでも壊さない)。
    while IFS= read -r f; do
        [ -f "$f" ] || continue
        "$style" --mode outside --write "$f" >/dev/null 2>&1 || true
    done < "$list"

    changed=$(git diff --name-only 2>/dev/null || true)
    [ -n "$changed" ] || return 0
    printf '%s\n' "$changed" | while IFS= read -r f; do
        [ -n "$f" ] && git add -- "$f"
    done
    if ! git diff --cached --quiet; then
        git commit -q -m "style: apply //[ //] chained formatting" -- $changed
    fi
}

style_before_push

token=$(load_token)
: "${token:?GIT_PUSH_TOKEN / neodax-token / keychain のどれにもトークンがありません}"

askpass=$(mktemp "${TMPDIR:-/tmp}/askpass.XXXXXX")
trap 'rm -f "$askpass"' EXIT INT TERM
cat >"$askpass" <<'EOF'
#!/bin/sh
printf '%s\n' "$GIT_PUSH_TOKEN"
EOF
chmod +x "$askpass"

if [ -n "${GIT_PUSH_URL:-}" ]; then
    GIT_ASKPASS="$askpass" GIT_PUSH_TOKEN="$token" \
        git -c credential.helper= push "$GIT_PUSH_URL" "$refspec"
else
    GIT_ASKPASS="$askpass" GIT_PUSH_TOKEN="$token" \
        git -c credential.helper= push origin "$refspec"
fi
