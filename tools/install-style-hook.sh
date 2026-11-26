#!/bin/sh
# 現在の git リポジトリに pre-push 整形フックを入れる。
#
#   sh /path/to/neodax/tools/install-style-hook.sh
#   git config neodax.styleOnPush all   # または true
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
gitdir=$(git rev-parse --git-dir)
hook="$gitdir/hooks/pre-push"

mkdir -p "$gitdir/hooks"
cat > "$hook" <<EOF
#!/bin/sh
NEODAX_HOME='$here/..' exec sh '$here/pre-push-style.sh'
EOF
chmod +x "$hook"
echo "installed: $hook"
echo "enable: git config neodax.styleOnPush all   # or true"
