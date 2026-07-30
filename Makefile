.PHONY: all vm test bundle pipeline interp setup clean

SHEN   = ../shen-scheme/_build/bin/shen-scheme
CFLAGS = -Wall -Wextra -O2 -Ivendor/bartlett-gc
GC_LIB = vendor/bartlett-gc/libbgc.a

all: zincvm zincdec

$(GC_LIB):
	$(MAKE) -C vendor/bartlett-gc libbgc.a

setup:
	@if [ ! -d ../shen-scheme ]; then \
		echo "Cloning shen-scheme..."; \
		git clone https://github.com/tizoc/shen-scheme.git ../shen-scheme; \
	else \
		echo "shen-scheme already present"; \
	fi

zincvm: vm/zincvm.c $(GC_LIB)
	$(CC) $(CFLAGS) -o $@ vm/zincvm.c $(GC_LIB)

zincvm-asan: vm/zincvm.c $(GC_LIB)
	$(CC) $(CFLAGS) -O0 -g -fsanitize=address -o $@ vm/zincvm.c $(GC_LIB)

zincdec: vm/zincdec.c $(GC_LIB)
	$(CC) $(CFLAGS) -o $@ vm/zincdec.c $(GC_LIB)

test: zincvm
	./zincvm

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

clean:
	rm -f zincvm zincdec *.csexp globals.csexp
