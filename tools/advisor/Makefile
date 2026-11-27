# neodax-advisor — repo 内の関数どうしの関係を見る (依存なし・C のみ)
#
#   make -C tools/advisor          -> ../../bin/neodax-advisor
#   make -C tools/advisor check    -> 警告をエラー扱いでビルド確認
#
# 実装は advisor.c 1 本。

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
ROOT    := ../..
OUT     := $(ROOT)/bin/neodax-advisor

SRCS := advisor.c
INCS :=

all: $(OUT)

$(OUT): $(SRCS)
	@mkdir -p $(ROOT)/bin
	$(CC) $(CFLAGS) $(INCS) -o $@ $(SRCS)

check:
	$(CC) $(CFLAGS) -Werror $(INCS) -fsyntax-only advisor.c
	$(CC) $(CFLAGS) -Werror $(INCS) -o /tmp/neodax-advisor-check $(SRCS)

clean:
	rm -f $(OUT)

.PHONY: all check clean
