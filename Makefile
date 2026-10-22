# =================================================
#  NeoDAX Makefile
#  Default target: launch setup.sh (interactive)
#  `make build` builds non-interactively.
# =================================================

.PHONY: all setup build clean install test

all:
	@chmod +x setup.sh
	@bash setup.sh

setup: all

build:
	@chmod +x setup.sh
	@bash setup.sh build

test:
	@bash tests/run_tests.sh

clean:
	@rm -rf .build
	@rm -f neodax neodax.exe js/neodax.node
	@find src arch -name '*.o' -delete 2>/dev/null || true

install:
	@echo "Run setup.sh to build first, then copy neodax to your PATH manually."
