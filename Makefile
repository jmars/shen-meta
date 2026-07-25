.PHONY: all vm test bundle pipeline interp clean

SHEN   = ../shen-scheme/_build/bin/shen-scheme
CFLAGS = -Wall -Wextra -O2

all: zincvm

zincvm: vm/zincvm.c
	$(CC) $(CFLAGS) -o $@ vm/zincvm.c

test: zincvm
	./zincvm

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
	rm -f zincvm *.csexp globals.csexp
