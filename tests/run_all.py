"""Run the host interpreter, audio, BBAL and WAV-tool regression tests.

The test DLL is built from the checked-in C sources. No game BIN/LIB/SAV
files or proprietary compiler executables are required.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def compiler_command():
    zig = shutil.which('zig')
    if zig:
        return [zig, 'cc']
    for name in ('gcc', 'clang'):
        found = shutil.which(name)
        if found:
            return [found]
    raise SystemExit('A host C compiler is required: install zig, gcc or clang')


def run(args):
    print('+', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True, cwd=ROOT)


def main():
    cc = compiler_command()
    build = ROOT/'build'
    build.mkdir(exist_ok=True)
    common = cc + ['-std=c99', '-Wall', '-Wextra', '-Werror', '-O2',
                   '-I', ROOT/'src']

    run(common + [
        ROOT/'src/bbvm_core.c', ROOT/'src/bbvm_runtime.c',
        ROOT/'src/bbvm_lib.c', ROOT/'tests/host_test.c',
        '-o', build/'host_test.exe'])
    run([build/'host_test.exe'])

    audio_sources = [
        ROOT/'src/bbvm_core.c', ROOT/'src/bbvm_runtime.c',
        ROOT/'src/bbvm_lib.c', ROOT/'src/bbvm_audio.c',
        ROOT/'src/bbvm_audio_ports.c', ROOT/'src/bbvm_mixer.c',
        ROOT/'tests/layout_probe.c',
    ]
    library = build/('bbvm_host.dll' if os.name == 'nt' else 'libbbvm_host.so')
    run(common + audio_sources + ['-shared', '-Wl,--export-all-symbols',
                                  '-o', library])
    run([sys.executable, ROOT/'tests/test_audio.py'])
    run([sys.executable, ROOT/'tests/test_audio_library.py'])
    run([sys.executable, ROOT/'tests/test_wav_tools.py'])


if __name__ == '__main__':
    main()
