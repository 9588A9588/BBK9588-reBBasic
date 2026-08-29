from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


PROJECT = Path(__file__).resolve().parents[1]


def find_sdk_root() -> Path:
    candidates: list[Path] = []
    configured = os.environ.get("BBK9588_SDK_ROOT")
    if configured:
        candidates.append(Path(configured).expanduser())
    candidates.append(PROJECT.parent)
    for candidate in candidates:
        root = candidate.resolve()
        if (root / "bda_packer" / "build.py").is_file() and \
                (root / "sdk" / "include" / "bda_sdk.h").is_file():
            return root
    raise SystemExit(
        "BBK 9588 SDK source tree not found. Clone this repository as "
        "<bbk9588-bda-sdk>/re-bbasic, or set BBK9588_SDK_ROOT."
    )


SDK_ROOT = find_sdk_root()
sys.path.insert(0, str(SDK_ROOT))

from bda_packer.build import (  # noqa: E402
    ENTRY_OFFSET,
    ENTRY_VA,
    ICON_SIZES,
    ICON_START,
    build_icons,
)
from bda_packer.header import BdaHeaderFields, write_header  # noqa: E402
from bda_packer.validate import validate_bda  # noqa: E402


SOURCES = (
    PROJECT / "src" / "main.c",
    PROJECT / "src" / "bbvm_core.c",
    PROJECT / "src" / "bbvm_runtime.c",
    PROJECT / "src" / "bbvm_lib.c",
    PROJECT / "src" / "bda_backend.c",
)


def run(command: list[str], label: str) -> None:
    print(f"[{label}] {' '.join(command)}")
    subprocess.run(command, check=True)


def tool(name: str) -> str:
    candidate = SDK_ROOT / ".toolchain" / "bin" / f"mipsel-none-elf-{name}.exe"
    if candidate.is_file():
        return str(candidate)
    found = shutil.which(f"mipsel-none-elf-{name}")
    if found:
        return found
    raise SystemExit(f"missing mipsel-none-elf-{name}; run scripts/setup_toolchain.ps1")


def compile_image(work: Path) -> bytes:
    cc = tool("gcc")
    objcopy = tool("objcopy")
    common = [
        "-EL", "-march=mips32", "-msoft-float", "-mno-abicalls", "-G0",
        "-fno-pic", "-Os", "-ffreestanding", "-fno-builtin", "-nostdlib",
        "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror",
        "-I", str(SDK_ROOT / "sdk" / "include"), "-I", str(PROJECT / "src"),
    ]
    objects: list[Path] = []
    for source in SOURCES:
        obj = work / f"{source.stem}.o"
        run([cc, *common, "-c", str(source), "-o", str(obj)], f"CC {source.name}")
        objects.append(obj)

    script = work / "bbvm.ld"
    script.write_text(
        f"""ENTRY(bda_main)
SECTIONS
{{
  . = 0x{ENTRY_VA:x};
  .text : {{ *(.text.bda_main) *(.text*) }}
  .rodata : {{ *(.rodata*) }}
  .data : {{ *(.data*) *(.sdata*) *(.bss*) *(COMMON) }}
  /DISCARD/ : {{ *(.comment*) *(.note*) }}
}}
""",
        encoding="ascii",
    )
    elf = work / "bbvm.elf"
    raw = work / "bbvm.bin"
    map_file = work / "bbvm.map"
    run(
        [
            cc, "-EL", "-march=mips32", "-msoft-float", "-mno-abicalls", "-G0",
            "-fno-pic", "-nostdlib", "-Wl,--build-id=none", "-Wl,--gc-sections",
            f"-Wl,-Map,{map_file}", f"-Wl,-T,{script}",
            *(str(item) for item in objects), "-lgcc", "-o", str(elf),
        ],
        "LD",
    )
    run([objcopy, "-O", "binary", str(elf), str(raw)], "OBJCOPY")
    return raw.read_bytes()


def package(code: bytes, output: Path, title: str) -> None:
    data = bytearray(b"\0" * ENTRY_OFFSET)
    icons = build_icons(None, (0, 32, 64))
    expected = ENTRY_OFFSET - ICON_START
    if len(icons) != expected:
        raise SystemExit(f"icon region is {len(icons):#x}, expected {expected:#x}")
    data[ICON_START:ENTRY_OFFSET] = icons
    data.extend(code)
    while len(data) % 4:
        data.append(0)
    fields = BdaHeaderFields(
        category=4,
        file_size_minus_4=len(data) - 4,
        entry_offset=ENTRY_OFFSET,
        icon_start=ICON_START,
        icon0_size=ICON_SIZES[0],
        icon1_size=ICON_SIZES[1],
        icon2_size=ICON_SIZES[2],
        icon3_size=ICON_SIZES[3],
    )
    write_header(data, fields, title)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    report = validate_bda(output)
    if not report["ok"]:
        for error in report["errors"]:
            print(error, file=sys.stderr)
        raise SystemExit("BDA validation failed")
    print(f"[OK] {output} ({len(data)} bytes, code {len(code)} bytes)")


def main() -> None:
    parser = argparse.ArgumentParser(description="Build the reconstructed 9588 BBVM BDA")
    parser.add_argument("-o", "--output", type=Path,
                        default=PROJECT / "build" / "BB虚拟机-re.bda")
    parser.add_argument("--title", default="BB虚拟机")
    args = parser.parse_args()
    (PROJECT / "build").mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="re_bbasic_") as directory:
        package(compile_image(Path(directory)), args.output.resolve(), args.title)


if __name__ == "__main__":
    main()
