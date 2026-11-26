#!/bin/sh
# pre-push hook: //[ //] が無いソースを整形してマーカーを付ける。
#
# 通常の `git push` (VS Code の「プッシュ」を含む) でも動くように、
# push の直前に整形し、変更があれば push を止める。
# 整形結果を確認して commit し、もう一度 push すると通る。
#
# 対象リポジトリで次を設定したときだけ有効:
#   git config neodax.styleOnPush true   # その push で変わるファイル
#   git config neodax.styleOnPush all    # 全 tracked ソース
set -eu

[ -n "${NEODAX_HOME:-}" ] || exit 0

# stdin の ref を見て、タグだけの push なら整形しない。
only_tags=1
while read -r local_ref local_sha remote_ref remote_sha; do
    [ -n "${local_ref:-}" ] || continue
    case "$local_ref" in
        refs/tags/*) ;;
        *) only_tags=0 ;;
    esac
done
[ "$only_tags" = 1 ] && exit 0

mode=$(git config --get neodax.styleOnPush 2>/dev/null || true)
[ -n "$mode" ] || exit 0

style="$NEODAX_HOME/bin/neodax-style"
if [ ! -x "$style" ]; then
    make -s -C "$NEODAX_HOME/src" ../bin/neodax-style >/dev/null 2>&1 || exit 0
fi
[ -x "$style" ] || exit 0

if [ -n "$(git status --porcelain 2>/dev/null || true)" ]; then
    echo "neodax-style: working tree が汚れているため整形を省略" >&2
    exit 0
fi

list=$(mktemp "${TMPDIR:-/tmp}/neodax-prepush.XXXXXX")
trap 'rm -f "$list"' EXIT INT TERM
if [ "$mode" = "all" ]; then
    git ls-files > "$list" 2>/dev/null || true
else
    base=$(git rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>/dev/null || true)
    [ -n "$base" ] || base=origin/main
    git diff --name-only --diff-filter=ACMR "$base"...HEAD > "$list" 2>/dev/null || true
fi
[ -s "$list" ] || exit 0

while IFS= read -r f; do
    [ -f "$f" ] || continue
    "$style" --mode outside --write "$f" >/dev/null 2>&1 || true
done < "$list"

if ! git diff --quiet 2>/dev/null; then
    echo "neodax-style: ソースを整形し、//[ //] を付けました。" >&2
    echo "neodax-style: 内容を確認して commit し、もう一度 push してください。" >&2
    exit 1
fi
exit 0
