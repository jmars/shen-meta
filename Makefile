.PHONY: all vm test test-debug debug bundle bundle-full pipeline interp setup clean

SHEN   = vendor/shen-scheme/bin/shen-scheme
CFLAGS = -Wall -Wextra -O2 -I vm
# Debug build enables ZINCVM_DEBUG: C primitives route type-errors through
# vm_throw as defense-in-depth (normally owned by the Shen safe wrappers).
DCFLAGS = -Wall -Wextra -O0 -g -DZINCVM_DEBUG -I vm

all: zincvm zincdec zinctest

# cosmocc produces a fat APE plus cross-build intermediates (.com.dbg,
# .aarch64.elf) alongside the output.  Stage those in a temp dir so they never
# land in the repo; only the native x86_64 ELF (.com.dbg) is copied out as the
# final binary.  Leaves the working dir unpolluted by build products.

# $1 = final binary path (zincvm / zincvm-debug / zincdec), $2 = extra CFLAGS
define compile-vm
	@T=$$(mktemp -d /tmp/$(notdir $(1)).build.XXXXXX) && \
	cosmocc $(2) -o $$T/out.ape $(3) && \
	cp $$T/out.ape.com.dbg $(1) && chmod 755 $(1); \
	st=$$?; rm -rf $$T; exit $$st
endef

zincvm: vm/zincvm.c vm/gc.c vm/gc.h vm/zinctypes.h vm/zincvm.h
	$(call compile-vm,$@,$(CFLAGS),vm/zincvm.c vm/gc.c)

zincvm-debug: vm/zincvm.c vm/gc.c vm/gc.h vm/zinctypes.h vm/zincvm.h
	$(call compile-vm,$@,$(DCFLAGS),vm/zincvm.c vm/gc.c)

debug: zincvm-debug
	@echo "Built ./zincvm-debug (ZINCVM_DEBUG: C-level type-error defense-in-depth active)"

zincvm-asan: vm/zincvm.c vm/gc.c vm/gc.h vm/zinctypes.h vm/zincvm.h
	$(call compile-vm,$@,$(CFLAGS) -O0 -g -fsanitize=address,vm/zincvm.c vm/gc.c)

zincdec: vm/zincdec.c
	$(call compile-vm,$@,$(CFLAGS),vm/zincdec.c)

zinctest: vm/zinctest.c vm/zincvm.c vm/gc.c vm/gc.h vm/zinctypes.h vm/zincvm.h
	$(call compile-vm,$@,$(CFLAGS) -DZINCTEST,vm/zinctest.c vm/zincvm.c vm/gc.c)

zinctest-debug: vm/zinctest.c vm/zincvm.c vm/gc.c vm/gc.h vm/zinctypes.h vm/zincvm.h
	$(call compile-vm,$@,$(DCFLAGS) -DZINCTEST,vm/zinctest.c vm/zincvm.c vm/gc.c)

zinctest-asan: vm/zinctest.c vm/zincvm.c vm/gc.c vm/gc.h vm/zinctypes.h vm/zincvm.h
	$(call compile-vm,$@,$(CFLAGS) -O0 -g -fsanitize=address -DZINCTEST,vm/zinctest.c vm/zincvm.c vm/gc.c)

clean:
	rm -f zincvm zincvm-debug zincdec zincvm-asan zinctest zinctest-debug zinctest-asan *.csexp globals.csexp globals-full.csexp

test: zinctest
	./zinctest

test-debug: zinctest-debug
	./zinctest-debug

test-asan: zinctest-asan
	ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 ./zinctest-asan

asan: zinctest-asan
	ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 ./zinctest-asan globals.csexp

bundle: shen/serialize-reduced.shen
	$(SHEN) script shen/serialize-reduced.shen 2>/dev/null
	@echo "Bundle written to globals.csexp ($$(wc -c < globals.csexp) bytes)"

# Full Shen OS bundle — type-unsafe, requires a guards-enabled (debug) VM.
# Written to globals-full.csexp for testing the complete OS.  The release
# C VM compiles out primitive type guards and CANNOT run this
# (shen.initialise segfaults); use ./zincvm-debug globals-full.csexp.
bundle-full: shen/serialize.shen
	$(SHEN) script shen/serialize.shen 2>/dev/null
	@echo "Full bundle written to globals-full.csexp ($$(wc -c < globals-full.csexp) bytes)"

run-bundle: zinctest globals.csexp
	./zinctest globals.csexp

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
