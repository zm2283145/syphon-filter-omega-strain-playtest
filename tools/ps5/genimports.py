import pathlib, re
# Port-only (132ps5c, 132ps5d): regenerates src/port/ps5/ps5_imports.cpp from a list of the linked title's imports
# (tools/ps5/genimports.sh: llvm-nm -u of llvm-pie.elf; the omega_ps5_imports target runs both). Run after the imports
# change.  usage: genimports.py <imports.txt>
import sys
root = pathlib.Path(__file__).resolve().parents[2]
names = [n.strip() for n in pathlib.Path(sys.argv[1]).read_text().split() if n.strip()]
wraps = set("chdir getcwd realpath access fchmodat fopen freopen link lstat mkdir mkstemp open openat opendir fdopendir readdir closedir pathconf "
            "readlink remove rename stat statvfs symlink truncate unlink unlinkat utimes malloc calloc realloc free __cxa_throw "
            "posix_memalign malloc_usable_size".split())
skip = {'sceKernelDebugOutText', 'chdir', 'getcwd', 'realpath', 'mkstemp'}  # weak, or implemented by the title
names = [n for n in names if n not in skip and re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', n)]
refs = [('__real_' + n if n in wraps else n, n) for n in names]
out = []
out.append('// Port-only (132ps5c, research/PS5.md "Imports"): GENERATED from the link\'s imported symbols (llvm-nm -u of\n'
           '// llvm-pie.elf, the wrapped ones through their __real_ names). The PS5 loader leaves an import that no system\n'
           '// module exports as a null pointer, and calling one faults with rip 0 (132ps5c hw1: getcwd; hw4: mkstemp in the\n'
           '// GL driver\'s shader cache). log_missing_imports() logs every import that is null at run time, at the start.\n'
           '// No headers on purpose: the symbols are declared as bytes, only their addresses are read.\n')
out.append('#include "ps5_imports.hpp"\n\n#include "ps5_log.hpp"\n')
out.append('extern "C" {')
for ref, _ in refs:
    out.append(f'extern char {ref};')
out.append('}\n\nnamespace omega::ps5 {\nnamespace {\n\nstruct Import {\n    const char* name;\n    const void* address;\n};\n\n}  // namespace\n')
out.append('unsigned log_missing_imports() {\n'
           '    // Filled at run time (each address loaded through the GOT, as the calls do), not a static table.\n'
           '    const Import imports[]{')
for ref, n in refs:
    out.append(f'        {{"{n}", &::{ref}}},')
out.append('    };')
out.append('''    unsigned missing = 0;
    for (const Import& i : imports) {
        // Read through a volatile pointer: the compiler may assume a symbol's address is never null.
        const void* const volatile* slot = &i.address;
        if (*slot == nullptr) {
            ++missing;
            log("import %s: NOT RESOLVED (null; a call faults with rip 0)", i.name);
        }
    }
    log("imports: %u checked, %u not resolved", unsigned(sizeof imports / sizeof imports[0]), missing);
    return missing;
}

}  // namespace omega::ps5
''')
(root / 'src/port/ps5/ps5_imports.cpp').write_text('\n'.join(out), encoding='utf-8', newline='\n')
(root / 'src/port/ps5/ps5_imports.hpp').write_text(
    '#pragma once\n// Port-only (132ps5c): the start-up check of the title\'s imports (ps5_imports.cpp).\n\n'
    'namespace omega::ps5 {\n\n// Logs every imported symbol the loader left null; returns their count.\nunsigned log_missing_imports();\n\n}  // namespace omega::ps5\n',
    encoding='utf-8', newline='\n')
print(len(refs))
