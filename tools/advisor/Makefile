# neodax-advisor — repo 内の関数どうしの関係を見る (依存なし・C のみ)
#
#   make -C tools/advisor          -> ../../bin/neodax-advisor
#   make -C tools/advisor check    -> 警告をエラー扱いでビルド確認
#
# 実装は advisor.c 1 本。整形の検査だけ src/analysis/chained_style.c を使う。

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
ROOT    := ../..
OUT     := $(ROOT)/bin/neodax-advisor

SRCS := advisor.c $(ROOT)/src/analysis/chained_style.c
INCS := -I$(ROOT)/src/analysis

all: $(OUT)

$(OUT): $(SRCS) $(ROOT)/src/analysis/chained_style.h
	@mkdir -p $(ROOT)/bin
	$(CC) $(CFLAGS) $(INCS) -o $@ $(SRCS)

check:
	$(CC) $(CFLAGS) -Werror $(INCS) -fsyntax-only advisor.c
	$(CC) $(CFLAGS) -Werror $(INCS) -o /tmp/neodax-advisor-check $(SRCS)

clean:
	rm -f $(OUT)

.PHONY: all check clean
