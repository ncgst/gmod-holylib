"""Inspect exact production physics resolver behavior against caller-supplied ELF fixtures.

No engine instructions are executed. The fixture JSON is an array containing
file, sha256, phook and fus fields; addresses are hexadecimal ELF virtual
addresses. Binaries are not redistributed.
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
template = (root / 'tests/physics_symbol_resolver_harness.cpp').read_text(encoding='utf-8')
assert template.count('// INSERT_PRODUCTION_RESOLVER') == 1
fixtures = json.loads(args.fixtures.read_text(encoding='utf-8'))
for fixture in fixtures:
    path = (args.fixtures.parent / fixture['file']).resolve()
    assert hashlib.sha256(path.read_bytes()).hexdigest() == fixture['sha256'], path
    fixture['file'] = str(path)
if args.work_dir:
    args.work_dir.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='physics-resolver-', dir=args.work_dir) as directory:
    directory = Path(directory)
    cpp = directory / 'resolver.cpp'
    cpp.write_text(template.replace('// INSERT_PRODUCTION_RESOLVER', body), encoding='utf-8')
    binary = directory / ('resolver.exe' if args.msvc else 'resolver')
    if args.msvc:
        command = [args.compiler, '/nologo', '/EHsc', '/std:c++17', '/W4', '/D_CRT_SECURE_NO_WARNINGS',
                   str(cpp), '/Fe:' + str(binary)]
    else:
        command = [args.compiler, '-std=c++17', '-Wall', '-Wextra', '-pedantic', str(cpp), '-o', str(binary)]
    subprocess.run(command, cwd=directory, check=True)
    modes = ['nominal', 'corrupt-phook-name', 'duplicate-phook-name', 'wrong-type-name',
             'zero-vtable-typeinfo', 'remove-slot', 'wrong-slot', 'oversized-data-segment', 'duplicate-fus-signature', 'remove-callers',
             'no-read', 'non-executable', 'two-executable-segments', 'null-module']
    for fixture in fixtures:
        print('Fixture SHA-256: ' + fixture['sha256'], flush=True)
        for mode in modes:
            subprocess.run([str(binary), fixture['file'], fixture['phook'], fixture['fus'], mode],
                           check=True, timeout=60)
