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
#   4. どれも無ければ、通常の git 認証に任せる (keychain は git が使う)
#
# このスクリプトはトークンを渡すだけ。整形は push では行わない
# (GitHub Actions 側の workflow が行う)。
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

}


token=$(load_token)

if [ -n "$token" ]; then
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
else
    # トークンが無いときは、git の通常の credential.helper に任せる。
    # このスクリプト自身は keychain を読まない。
    if [ -n "${GIT_PUSH_URL:-}" ]; then
        git push "$GIT_PUSH_URL" "$refspec"
    else
        git push origin "$refspec"
    fi
fi