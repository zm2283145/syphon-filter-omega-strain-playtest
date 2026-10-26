#!/bin/bash
# writes the list of the link's undefined (imported) symbols, one per line
cd ~/ps5work/omega/app/build
llvm-nm-18 -u llvm-pie.elf | awk '{print $NF}' | grep -v '^__wrap_' | sort -u > /mnt/d/Claude/sfos-agents/132ps5a/local/ps5/imports.txt
wc -l /mnt/d/Claude/sfos-agents/132ps5a/local/ps5/imports.txt
