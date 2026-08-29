from __future__ import annotations

import argparse
import struct
from pathlib import Path


LENGTHS = (1, 10, 5, 5, 10, 10, 5, 6, 5, 1, 10, 10, 0, 0, 0, 1)
OPCODES = ("NOP", "LD", "PUSH", "POP", "IN", "OUT", "JMP", "JPC",
           "CALL", "RET", "CMP", "CAL", "?C", "?D", "?E", "EXIT")
TYPES = ("DWORD", "WORD", "BYTE", "FLOAT", "INT")
CAL_OPS = ("ADD", "SUB", "MUL", "DIV", "MOD")
FLAGS = ("?0", "Z", "B", "BE", "A", "AE", "NZ")
REGS = ("rp", "rf", "rs", "rb", "r0", "r1", "r2", "r3")


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def operand(mode: int, value: int) -> str:
    if mode == 0:
        return REGS[value] if value < len(REGS) else f"reg({value})"
    if mode == 1:
        name = REGS[value] if value < len(REGS) else str(value)
        return f"[{name}]"
    if mode == 2:
        return f"0x{value:08x}"
    return f"[0x{value:08x}]"


def disassemble(data: bytes, start: int, end: int) -> None:
    ip = start
    while ip < end and ip < len(data):
        command = data[ip] >> 4
        length = LENGTHS[command]
        raw = data[ip:ip + max(length, 1)]
        raw_text = " ".join(f"{byte:02x}" for byte in raw)
        text = OPCODES[command]
        if length == 0 or ip + length > len(data):
            print(f"{ip:08x}  {raw_text:<29}  INVALID")
            break
        if length == 5:
            text += " " + operand(data[ip] & 0x0F, u32(data, ip + 1))
        elif length == 6:
            flag = data[ip] & 0x0F
            flag_name = FLAGS[flag] if flag < len(FLAGS) else f"?{flag}"
            text += f".{flag_name} " + operand(data[ip + 1], u32(data, ip + 2))
        elif length == 10:
            type_id = data[ip] & 0x0F
            operation = data[ip + 1] >> 4
            first_mode = (data[ip + 1] & 0x0F) >> 2
            second_mode = data[ip + 1] & 3
            first = operand(first_mode, u32(data, ip + 2))
            second = operand(second_mode, u32(data, ip + 6))
            suffix = TYPES[type_id] if type_id < len(TYPES) else f"TYPE{type_id}"
            if command == 0xB:
                suffix += "." + (CAL_OPS[operation] if operation < len(CAL_OPS)
                                  else f"OP{operation}")
            text += f".{suffix} {first}, {second}"
        print(f"{ip:08x}  {raw_text:<29}  {text}")
        ip += length


def main() -> None:
    parser = argparse.ArgumentParser(description="Disassemble BBASIC VM bytecode")
    parser.add_argument("binary", type=Path)
    parser.add_argument("--start", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--end", type=lambda value: int(value, 0))
    args = parser.parse_args()
    data = args.binary.read_bytes()
    if data[:3] == b"BBE":
        data = data[16:]
    end = len(data) if args.end is None else min(args.end, len(data))
    disassemble(data, args.start, end)


if __name__ == "__main__":
    main()
