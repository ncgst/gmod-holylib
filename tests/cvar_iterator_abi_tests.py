"""Compile the production x86-64 ICvar iterator against a deterministic engine-ABI mock.

The harness models the libvstdlib iterator vtable verified for both deployed
engines and the pinned SDK's stale inline calls; it does not execute engine code.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default='c++')
parser.add_argument('--msvc', action='store_true')
parser.add_argument('--work-dir', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
header = (root / 'source/x64_cvar_iterator.h').read_text(encoding='utf-8')
guard = '#if defined(SYSTEM_LINUX) && defined(ARCHITECTURE_X86_64)'
start = header.rindex(guard) + len(guard)
body = header[start:header.index('#endif', start)]
template = (root / 'tests/cvar_iterator_abi_harness.cpp').read_text(encoding='utf-8')
assert template.count('// INSERT_PRODUCTION_ITERATOR') == 1
if args.work_dir:
    args.work_dir.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='cvar-iterator-abi-', dir=args.work_dir) as directory:
    directory = Path(directory)
    cpp = directory / 'iterator.cpp'
    cpp.write_text(template.replace('// INSERT_PRODUCTION_ITERATOR', body), encoding='utf-8')
    binary = directory / ('iterator.exe' if args.msvc else 'iterator')
    if args.msvc:
        command = [args.compiler, '/nologo', '/EHsc', '/std:c++17', '/W4', '/D_CRT_SECURE_NO_WARNINGS',
                   str(cpp), '/Fe:' + str(binary)]
    else:
        command = [args.compiler, '-std=c++17', '-Wall', '-Wextra', '-pedantic', str(cpp), '-o', str(binary)]
    subprocess.run(command, cwd=directory, check=True)
    subprocess.run([str(binary)], cwd=directory, check=True, timeout=15)
