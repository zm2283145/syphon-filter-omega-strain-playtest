#!/bin/bash
# Port-only (132ps5c, 132ps5d; research/PS5.md "Imports"): writes the linked title's undefined (imported) symbols, one per
# line, for genimports.py.  usage: genimports.sh <llvm-pie.elf> <imports.txt>
set -euo pipefail
nm=$(command -v llvm-nm-18 || command -v llvm-nm)
"$nm" -u "$1" | awk '{print $NF}' | grep -v '^__wrap_' | sort -u > "$2"
wc -l < "$2"
