"""Exercise the production ABI guard on hash-verified, relocated libvstdlib files.

Fixture fields: file, sha256, cvar_vtable and iterator_vtable (hex ELF RVAs).
Engine code is never executed. The ELF adapter is shared with the stringtable test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--fixtures', required=True, type=Path)
parser.add_argument('--compiler', default='c++')
parser.add_argument('--msvc', action='store_true')
parser.add_argument('--work-dir', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'source/symbols.cpp').read_text(encoding='utf-8')
guard = '#if defined(SYSTEM_LINUX) && defined(ARCHITECTURE_X86_64)'
start = source.rindex(guard) + len(guard)
body = source[start:source.index('#endif', start)]
shared = (root / 'tests/stringtable_resolver_harness.cpp').read_text(encoding='utf-8')
template = shared[:shared.index('int main(')]
template += (root / 'tests/cvar_iterator_layout_harness.cpp').read_text(encoding='utf-8')
fixtures = json.loads(args.fixtures.read_text(encoding='utf-8'))
for fixture in fixtures:
    path = (args.fixtures.parent / fixture['file']).resolve()
    assert hashlib.sha256(path.read_bytes()).hexdigest() == fixture['sha256'], path
    fixture['file'] = str(path)
if args.work_dir:
    args.work_dir.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='cvar-layout-', dir=args.work_dir) as directory:
    directory = Path(directory)
    cpp = directory / 'layout.cpp'
    cpp.write_text(template.replace('// INSERT_PRODUCTION_RESOLVER', body), encoding='utf-8')
    binary = directory / ('layout.exe' if args.msvc else 'layout')
    if args.msvc:
        command = [args.compiler, '/nologo', '/EHsc', '/std:c++17', '/W4', '/D_CRT_SECURE_NO_WARNINGS',
                   str(cpp), '/Fe:' + str(binary)]
    else:
        command = [args.compiler, '-std=c++17', '-Wall', '-Wextra', '-pedantic', str(cpp), '-o', str(binary)]
    subprocess.run(command, cwd=directory, check=True)
    modes = ['nominal', 'unknown-build', 'malformed-note', 'missing-note', 'wrong-factory',
             'null-factory', 'swapped-methods', 'null-method', 'wrong-rtti', 'wrong-cvar',
             'null-cvar', 'null-module', 'no-read', 'non-executable']
    for fixture in fixtures:
        print('Fixture SHA-256: ' + fixture['sha256'], flush=True)
        for mode in modes:
            subprocess.run([str(binary), fixture['file'], fixture['cvar_vtable'], fixture['iterator_vtable'], mode],
                           check=True, timeout=15)
