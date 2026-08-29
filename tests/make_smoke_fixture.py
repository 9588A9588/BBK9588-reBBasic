from __future__ import annotations

import struct
from pathlib import Path


RP, RF, RS, RB, R0, R1, R2, R3 = range(8)
REG, REG_INDIRECT, IMMEDIATE, ABSOLUTE = range(4)
DWORD, WORD, BYTE, FLOAT, INTEGER = range(5)


class Program:
    def __init__(self) -> None:
        self.code = bytearray()
        self.fixups: list[tuple[int, str]] = []
        self.data: list[tuple[str, bytes]] = []

    def two(self, command: int, data_type: int, first_mode: int, first: int,
            second_mode: int, second: int, operation: int = 0) -> None:
        self.code.extend((command << 4 | data_type,
                          operation << 4 | first_mode << 2 | second_mode))
        self.code.extend(struct.pack("<II", first, second))

    def ld(self, register: int, value: int) -> None:
        self.two(1, DWORD, REG, register, IMMEDIATE, value)

    def ld_register(self, destination: int, source: int) -> None:
        self.two(1, DWORD, REG, destination, REG, source)

    def ld_address(self, register: int, label: str) -> None:
        start = len(self.code)
        self.ld(register, 0)
        self.fixups.append((start + 6, label))

    def inp(self, register: int, port: int) -> None:
        self.two(4, DWORD, REG, register, IMMEDIATE, port)

    def out(self, port: int, argument_mode: int = IMMEDIATE,
            argument: int = 0) -> None:
        self.two(5, DWORD, IMMEDIATE, port, argument_mode, argument)

    def add_data(self, label: str, payload: bytes) -> None:
        self.data.append((label, payload))

    def finish(self) -> bytes:
        self.code.append(0xF0)
        offsets: dict[str, int] = {}
        for label, payload in self.data:
            while len(self.code) % 4:
                self.code.append(0)
            offsets[label] = len(self.code)
            self.code.extend(payload)
        for offset, label in self.fixups:
            struct.pack_into("<I", self.code, offset, offsets[label])
        return bytes(self.code)


def words(*values: int) -> bytes:
    return struct.pack("<" + "I" * len(values), *(v & 0xFFFFFFFF for v in values))


def build() -> bytes:
    p = Program()
    p.out(17)                         # first compatible page is page 0
    p.ld_address(R3, "fill")
    p.out(23)                         # blue full-page fill
    p.ld_address(R3, "pen")
    p.out(64)
    p.ld(R1, 0); p.ld(R2, 16); p.ld(R3, 30); p.out(66)
    p.ld(R2, 220); p.ld(R3, 290); p.out(67)
    p.ld_address(R3, "circle")
    p.out(69)
    p.ld(R3, 0); p.out(21)            # present page 0

    p.inp(R0, 2)                      # allocate string handle
    p.ld_register(R3, R0)
    p.ld_address(R2, "message")
    p.inp(R1, 5)                      # copy VM-memory string to handle
    p.out(1, REG, R3)
    p.ld(R3, 15000); p.out(27)        # leave a visible test window for QA

    p.add_data("fill", words(0x00402010, 320, 240, 0, 0, 0))
    p.add_data("pen", words(0x0000FFFF, 1, 0, 0))
    p.add_data("circle", words(72, 160, 120, 0))
    p.add_data("message", b"re-bbasic VM OK\0")
    return p.finish()


def build_input() -> bytes:
    p = Program()
    p.inp(R0, 2)
    p.ld_register(R3, R0)
    p.ld_address(R2, "prompt")
    p.inp(R1, 5)
    p.out(2, REG, R3)
    p.inp(R0, 2)
    p.ld_register(R3, R0)
    p.out(11)
    p.out(1, REG, R3)
    p.ld(R3, 10000)
    p.out(27)
    p.add_data("prompt", b"Touch keyboard test: \0")
    return p.finish()


if __name__ == "__main__":
    directory = Path(__file__).resolve().parent / "fixtures"
    directory.mkdir(parents=True, exist_ok=True)
    for name, payload in (("smoke.bin", build()), ("input.bin", build_input())):
        output = directory / name
        output.write_bytes(payload)
        print(f"wrote {output} ({output.stat().st_size} bytes)")
