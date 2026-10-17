"""BBAL v1: little-endian indexed WAV library, one-based stable resource IDs.

Header <4sIII: BBAL, version=1, count, exact_file_bytes.
Index[count] <II: absolute_offset, member_bytes. Members are 4-byte aligned.
The BDA reads only the header, selected index entry and streamed WAV chunks.
Names/source paths/hashes are build metadata, never needed on the device.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import struct
import tempfile
from convert_wav import wav_bytes

MAX_SIZE = 0x7fffffff  # firmware seek uses signed 32-bit offsets
MAX_COUNT = 65535


def pack_members(members, count, output):
    """Incremental atomic writer; source conversion failure leaves output intact."""
    output = Path(output)
    if not 1 <= count <= MAX_COUNT:
        raise ValueError('Audio resource count must be 1..65535')
    output.parent.mkdir(parents=True, exist_ok=True)
    records = []
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix='.bbal-',
                                         suffix='.tmp', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(bytes(16+8*count))
            for name, payload in members:
                if len(records) >= count:
                    raise ValueError('More members than declared')
                if len(payload) < 44 or payload[:4] != b'RIFF' or payload[8:12] != b'WAVE':
                    raise ValueError('Members must be complete WAV files')
                stream.write(bytes((-stream.tell()) % 4))
                offset = stream.tell()
                if len(payload) > MAX_SIZE-offset:
                    raise ValueError('Audio library exceeds signed firmware seek range')
                stream.write(payload)
                records.append(dict(id=len(records)+1, name=name, offset=offset,
                                    bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest()))
            if len(records) != count:
                raise ValueError('Fewer members than declared')
            total = stream.tell()
            stream.seek(0)
            stream.write(struct.pack('<4sIII', b'BBAL', 1, count, total))
            for record in records:
                stream.write(struct.pack('<II', record['offset'], record['bytes']))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, output)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return dict(format='BBAL', version=1, bytes=total, sounds=records)


def pack_files(sounds, output):
    """sounds: ordered list of {id, file, name?}; no implicit directory ordering."""
    if [s.get('id') for s in sounds] != list(range(1, len(sounds)+1)):
        raise ValueError('IDs must be contiguous 1..N in order; no duplicates or silent renumbering')
    paths = [Path(s['file']).resolve() for s in sounds]
    if Path(output).resolve() in paths:
        raise ValueError('Output must not overwrite a source WAV')
    return pack_members(((s.get('name', p.stem), wav_bytes(p))
                         for s, p in zip(sounds, paths)), len(sounds), output)


def main():
    parser = argparse.ArgumentParser(description='Pack WAVs into a BBAL audio resource library')
    parser.add_argument('manifest', type=Path, help='JSON: {sounds:[{id:1,name:...,file:...},...]}')
    parser.add_argument('output', type=Path)
    parser.add_argument('--index', type=Path, help='Optional host-only JSON index report')
    args = parser.parse_args()
    sounds = json.loads(args.manifest.read_text(encoding='utf-8'))['sounds']
    for sound in sounds:
        sound['file'] = str(args.manifest.parent / sound['file'])
    if args.output.resolve() == args.manifest.resolve():
        raise ValueError('Output must not overwrite its manifest')
    protected = {args.manifest.resolve(), args.output.resolve()}
    protected.update(Path(s['file']).resolve() for s in sounds)
    if args.index and args.index.resolve() in protected:
        raise ValueError('Index report must not overwrite inputs or library')
    report = pack_files(sounds, args.output)
    if args.index:
        args.index.parent.mkdir(parents=True, exist_ok=True)
        args.index.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(args.output, report['bytes'], 'bytes;', len(sounds), 'indexed sounds')


if __name__ == '__main__':
    main()
