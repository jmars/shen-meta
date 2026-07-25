.PHONY: all vm test bundle clean

SHEN   = ../shen-scheme/_build/bin/shen-scheme
CFLAGS = -Wall -Wextra -O2

all: vm

vm: zincvm

zincvm: zincvm.c
	$(CC) $(CFLAGS) -o $@ $<

test: vm
	./zincvm

bundle: serialize.shen
	$(SHEN) script serialize.shen 2>/dev/null
	@echo "Bundle written to globals.csexp ($$(wc -c < globals.csexp) bytes)"

run-bundle: vm bundle
	./zincvm globals.csexp

pipeline: vm
	@$(SHEN) eval -e '(tc -)' \
	  -l normalize.shen -l zinc.shen -l compile.shen \
	  -e '(define compile-expr X -> (zinc->native (zinc-c (debruijn [] (normalize-term (kmacros X))))))' \
	  -e '(compile-expr [+ 1 2])' 2>/dev/null | tail -1 > /tmp/test1.csexp
	./zincvm /tmp/test1.csexp

interp:
	$(SHEN) script interp.shen

clean:
	rm -f zincvm *.csexp globals.csexp
