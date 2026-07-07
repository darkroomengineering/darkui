all:
	# rg35xxplus shares rg35xx's 32-bit ARM userspace, so it reuses rg35xx's
	# core builds instead of building its own; the root makefile's PLATFORMS
	# order guarantees rg35xx builds first during a full build
	test -d ../../rg35xx/cores/output || { echo "error: ../../rg35xx/cores/output not found -- build rg35xx cores first (make PLATFORMS='rg35xx rg35xxplus' or build rg35xx before rg35xxplus)"; exit 1; }
	rm -rf output
	cp -r ../../rg35xx/cores/output ./
