.PHONY: all vm test test-debug debug bundle pipeline interp setup clean

SHEN   = ../shen-scheme/_build/bin/shen-scheme
CFLAGS = -Wall -Wextra -O2
# Debug build enables ZINCVM_DEBUG: C primitives route type-errors through
# vm_throw as defense-in-depth (normally owned by the Shen safe wrappers).
DCFLAGS = -Wall -Wextra -O0 -g -DZINCVM_DEBUG
GC_LIB = -lgc

all: zincvm zincdec

zincvm: vm/zincvm.c
	$(CC) $(CFLAGS) -o $@ vm/zincvm.c $(GC_LIB)

zincvm-debug: vm/zincvm.c
	$(CC) $(DCFLAGS) -o $@ vm/zincvm.c $(GC_LIB)

debug: zincvm-debug
	@echo "Built ./zincvm-debug (ZINCVM_DEBUG: C-level type-error defense-in-depth active)"

zincvm-asan: vm/zincvm.c
	$(CC) $(CFLAGS) -O0 -g -fsanitize=address -o $@ vm/zincvm.c $(GC_LIB)

zincdec: vm/zincdec.c
	$(CC) $(CFLAGS) -o $@ vm/zincdec.c $(GC_LIB)

test: zincvm
	./zincvm

test-debug: zincvm-debug
	./zincvm-debug

test-asan: zincvm-asan
	ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 ./zincvm-asan

asan: zincvm-asan
	ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 ./zincvm-asan globals.csexp

bundle: shen/serialize.shen
	$(SHEN) script shen/serialize.shen 2>/dev/null
	@echo "Bundle written to globals.csexp ($$(wc -c < globals.csexp) bytes)"

run-bundle: zincvm globals.csexp
	./zincvm globals.csexp

pipeline: zincvm
	@$(SHEN) eval -e '(tc -)' \
	  -l shen/normalize.shen -l shen/zinc.shen -l shen/compile.shen \
	  -e '(define compile-expr X -> (zinc->native (zinc-c (debruijn [] (normalize-term (kmacros X))))))' \
	  -e '(compile-expr [+ 1 2])' 2>/dev/null | tail -1 > /tmp/test1.csexp
	./zincvm /tmp/test1.csexp

interp:
	$(SHEN) script shen/interp.shen

setup:
	@if [ ! -d ../shen-scheme ]; then \
		git clone https://github.com/tizoc/shen-scheme ../shen-scheme; \
	else \
		echo "shen-scheme already present"; \
	fi
	@$(MAKE) -C ../shen-scheme

clean:
	rm -f zincvm zincdec *.csexp globals.csexp
