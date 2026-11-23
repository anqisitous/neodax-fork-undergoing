# neodax-java-review — JDK の javac を使う Java reviewer
#
#   make -C tools/java-review check   # コンパイル確認 (JDK が必要)
#
# 実行は単一ファイル起動なので、ビルド成果物は作らない。
JAVAC ?= javac

check:
	@rm -rf /tmp/neodax-java-review-check
	@mkdir -p /tmp/neodax-java-review-check
	$(JAVAC) -Xlint:all -d /tmp/neodax-java-review-check JavaReview.java

.PHONY: check
