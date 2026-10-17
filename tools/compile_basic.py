from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess, tempfile

ROOT=Path(__file__).resolve().parents[1]


def find_tool_dir():
    """Locate the separately installed BB2.0++ compiler/linker.

    The compiler is intentionally not redistributed in this repository.  A
    local checkout may provide it under ``compiler/Tool``; otherwise set
    ``BBASIC_TOOL_DIR`` to the directory containing BBasic++.exe, Blink.exe,
    StdLib.lib and StdLib.fls.
    """
    candidates = []
    configured = os.environ.get('BBASIC_TOOL_DIR')
    if configured:
        candidates.append(Path(configured).expanduser())
    candidates.append(ROOT/'compiler'/'Tool')
    required = ('BBasic++.exe', 'Blink.exe', 'StdLib.lib', 'StdLib.fls')
    for candidate in candidates:
        candidate = candidate.resolve()
        if all((candidate/name).is_file() for name in required):
            return candidate
    locations = ', '.join(str(p) for p in candidates)
    raise RuntimeError(
        'BB2.0++ compiler tools not found. Install the separately licensed '
        'compiler and set BBASIC_TOOL_DIR to its Tool directory '
        f'(checked: {locations}).'
    )

def validate_assembly(text):
    # The old compiler can exhaust its four expression registers and emit a
    # random decimal value in place of a destination register. Blink accepts
    # that as an immediate; reject it before it can become a misleading BIN.
    for number,line in enumerate(text.splitlines(),1):
        tokens=line.strip().split()
        if len(tokens)<3:continue
        operation=tokens[0].upper()
        target=tokens[2] if operation=='LD' else tokens[3] if operation=='CAL' and len(tokens)>3 else None
        if target is None:continue
        target=target.split(',')[0]
        try:int(target,0)
        except ValueError:continue
        raise RuntimeError(f'Compiler emitted invalid immediate destination at assembly line {number}: {line}')

def compile_basic(source: Path, output: Path):
    tool = find_tool_dir()
    (ROOT/'build').mkdir(exist_ok=True)
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='bbx-',dir=ROOT/'build') as d:
        work=Path(d)
        for name in ['BBasic++.exe','Blink.exe','StdLib.lib','StdLib.fls']:
            shutil.copy2(tool/name,work/name)
        text=(ROOT/'sdk/AudioBB.lib').read_text(encoding='ascii')
        text+=chr(10)+(ROOT/'sdk/ResourceBB.lib').read_text(encoding='ascii')
        text+=chr(10)+(ROOT/'sdk/RenderBB.lib').read_text(encoding='ascii')
        text+=chr(10)+(ROOT/'sdk/MixerBB.lib').read_text(encoding='ascii')
        text+=chr(10)+source.read_text(encoding='utf-8')
        src=work/'program.bas'; src.write_bytes(text.encode('gbk'))
        compiled=subprocess.run([str(work/'BBasic++.exe'),str(src)],cwd=work,capture_output=True,timeout=20)
        obj=work/'test.obj'
        if not obj.exists(): raise RuntimeError('Compiler produced no fresh object: '+repr(compiled.stdout))
        validate_assembly(obj.read_text(encoding='gbk'))
        linked=subprocess.run([str(work/'Blink.exe'),str(obj),'program.bin'],cwd=work,capture_output=True,timeout=20)
        binary=work/'program.bin'
        # Historical linker returns 1 on success; also validate the fresh artifact.
        # Blink also writes a 16-byte BBE header on unresolved-symbol failure.
        # A fresh magic alone is not a successful build; the known linker uses 1
        # for success and 0 for failure (confirmed against its build logs).
        if (linked.returncode != 1 or not binary.exists()
                or len(binary.read_bytes()) <= 16
                or binary.read_bytes()[:4]!=bytes([66,66,69,0])):
            raise RuntimeError('Linker produced no valid BIN: '+repr(linked.stdout))
        b=binary.read_bytes(); output.write_bytes(b)
        output.with_suffix('.obj').write_bytes(obj.read_bytes())
        output.with_suffix('.build.log').write_bytes(compiled.stdout+compiled.stderr+linked.stdout+linked.stderr)
        output.with_suffix('.build.json').write_text(json.dumps(dict(
            input=str(source),output=str(output),size=len(b),sha256=hashlib.sha256(b).hexdigest(),
            compiler_sha256=hashlib.sha256((tool/'BBasic++.exe').read_bytes()).hexdigest(),
            compiler_exit=compiled.returncode,linker_exit=linked.returncode),indent=2),encoding='utf-8')
        print(str(output),len(b),'bytes')

if __name__=='__main__':
    ap=argparse.ArgumentParser(description='Compile BB2.0++ with the AudioBB function definitions')
    ap.add_argument('source',type=Path); ap.add_argument('output',type=Path)
    a=ap.parse_args(); compile_basic(a.source.resolve(),a.output.resolve())
