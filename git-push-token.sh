#!/bin/sh
# トークンを env で渡し、その場限りの askpass で push する。
# credential.helper には何も保存しない (終了時に askpass も消える)。
#
#   GIT_PUSH_TOKEN=ghp_xxx GIT_PUSH_URL=https://github.com/OWNER/REPO.git \
#     ./git-push-token.sh HEAD:main
#
# GIT_PUSH_URL を省略すると origin に push する。
set -eu

: "${GIT_PUSH_TOKEN:?GIT_PUSH_TOKEN を設定してください}"

url=${GIT_PUSH_URL:-}
refspec=${1:-HEAD}

askpass=$(mktemp "${TMPDIR:-/tmp}/askpass.XXXXXX")
trap 'rm -f "$askpass"' EXIT INT TERM
cat >"$askpass" <<'EOF'
#!/bin/sh
printf '%s\n' "$GIT_PUSH_TOKEN"
EOF
chmod +x "$askpass"

if [ -n "$url" ]; then
  GIT_ASKPASS="$askpass" GIT_PUSH_TOKEN="$GIT_PUSH_TOKEN" \
    git -c credential.helper= push "$url" "$refspec"
else
  GIT_ASKPASS="$askpass" GIT_PUSH_TOKEN="$GIT_PUSH_TOKEN" \
    git -c credential.helper= push origin "$refspec"
fi
