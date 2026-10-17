"""Real rebuilt C interpreter + audio engine, deterministic platform callbacks.
This is NOT a hardware sound/GUI test. DLL is built by run_all.py.
"""
import ctypes as c
import io
import json
from pathlib import Path
import struct
import unittest
import wave

ROOT = Path(__file__).resolve().parents[1]
U = c.c_uint32
I = c.c_int32
P = c.c_void_p
FP0 = c.CFUNCTYPE(I)
OPEN = c.CFUNCTYPE(I, c.c_char_p)
CLOSE = c.CFUNCTYPE(I, I)
READ = c.CFUNCTYPE(I, I, P, U)
SEEK = c.CFUNCTYPE(I, I, I, I)
WRITE = c.CFUNCTYPE(I, P, U)
STOP = c.CFUNCTYPE(None)
TICK = c.CFUNCTYPE(U)


class AudioIO(c.Structure):
    _fields_ = [('available', FP0), ('file_open', OPEN), ('file_close', CLOSE),
                ('file_read', READ), ('file_seek', SEEK), ('pcm_open', FP0),
                ('pcm_ready', FP0), ('pcm_write', WRITE), ('pcm_stop', STOP),
                ('tick_25ms', TICK)]


class Audio(c.Structure):
    _fields_ = [('io', AudioIO)] + [(n, I) for n in 'file opened paused loop error'.split()]
    _fields_ += [(n, U) for n in (
        'data_start data_bytes left bits pending pending_offset buffer_index '
        'tone_left phase phase_step draining drain_start last_tick stall_tick '
        'volume submitted polls reads queue_samples queue_tick queue_frac').split()]
    _fields_ += [('input', c.c_ubyte * 1024), ('pcm', (c.c_int16 * 512) * 2)]


class Host(c.Structure):
    _fields_ = [(n, P) for n in 'opaque alloc free tick_ms out_port in_port'.split()]


class String(c.Structure):
    _fields_ = [('data', P), ('length', U), ('capacity', U)]


class VM(c.Structure):
    _fields_ = [('memory', P), ('memory_size', U), ('image_size', U), ('data_ptr', U),
                ('reg', U * 8), ('random_state', U), ('steps', U), ('running', I),
                ('error', I), ('image_version', I), ('host', Host), ('strings', String * 256)]


import os
DLL = c.CDLL(str(ROOT / 'build' / ('bbvm_host.dll' if os.name == 'nt' else 'libbbvm_host.so')))
DLL.test_size_vm.restype = U
DLL.test_size_audio.restype = U
assert DLL.test_size_vm() == c.sizeof(VM), (DLL.test_size_vm(), c.sizeof(VM))
assert DLL.test_size_audio() == c.sizeof(Audio), (DLL.test_size_audio(), c.sizeof(Audio))
for name, args, result in [
    ('bbvm_audio_init', [P, P], None), ('bbvm_audio_play', [P, c.c_char_p, I], I),
    ('bbvm_audio_play_resource', [P, c.c_char_p, I, I], I),
    ('bbvm_audio_tone', [P, I, I], I), ('bbvm_audio_poll', [P], None),
    ('bbvm_audio_stop', [P], None), ('bbvm_audio_volume', [P, I], I),
    ('bbvm_audio_pause', [P, I], None), ('bbvm_audio_out', [P, P, U], I),
    ('bbvm_audio_in', [P, P, U], U), ('bbvm_init', [P, P, U, P], None),
    ('bbvm_load', [P, P, U], I), ('bbvm_run', [P, U], I),
    ('bbvm_destroy', [P], None), ('bbvm_new_string', [P], U),
    ('bbvm_set_string', [P, U, c.c_char_p], I), ('bbvm_in_port', [P, U], U),
    ('bbvm_get_string', [P, U], c.c_char_p),
]:
    f = getattr(DLL, name); f.argtypes = args; f.restype = result


def wav(data, bits=16, rate=22050, channels=1):
    stream = io.BytesIO()
    with wave.open(stream, 'wb') as w:
        w.setnchannels(channels); w.setsampwidth(bits // 8)
        w.setframerate(rate); w.writeframes(data)
    return stream.getvalue()


class Env:
    def __init__(self):
        self.audio = Audio(); self.vm = VM()
        self.tick = 0; self.support = 1; self.ready = 1
        self.files = {}; self.handles = {}; self.pos = 0
        self.pcm = []; self.opens = 0; self.stops = 0; self.reads = []
        self.write_limit = None; self.write_result = None
        self.bad_read = False; self.open_result = 1; self.events = []
        self.allocations = {}; self.retired = []
        self.io = AudioIO(FP0(lambda: self.support), OPEN(self.fopen), CLOSE(self.fclose),
                          READ(self.fread), SEEK(self.seek), FP0(self.pcm_open),
                          FP0(lambda: self.ready), WRITE(self.write), STOP(self.stop),
                          TICK(lambda: self.tick))
        DLL.bbvm_audio_init(c.byref(self.audio), c.byref(self.io))
        self.memory = (c.c_ubyte * (2 * 1024 * 1024))()
        self.callbacks = [c.CFUNCTYPE(P, P, U)(self.alloc),
                          c.CFUNCTYPE(None, P, P)(self.free),
                          c.CFUNCTYPE(U, P)(lambda _: self.tick * 25),
                          c.CFUNCTYPE(I, P, P, U, U)(self.out),
                          c.CFUNCTYPE(U, P, P, U)(self.inp)]
        self.host = Host(None, *(c.cast(f, P).value for f in self.callbacks))
        DLL.bbvm_init(c.byref(self.vm), self.memory, len(self.memory), c.byref(self.host))

    def alloc(self, _, size):
        b = c.create_string_buffer(size); address = c.addressof(b)
        self.allocations[address] = b; return address

    def free(self, _, address):
        b = self.allocations.pop(address)
        c.memset(address, 0xdd, len(b)); self.retired.append(b)

    def fopen(self, name):
        matches = [v for k, v in self.files.items() if k.lower() == name.lower()]
        if not matches: return 0
        self.handles[1] = matches[0]; self.pos = 0; return 1

    def fclose(self, fd):
        self.handles.pop(fd, None); return 0

    def fread(self, fd, buffer, n):
        self.reads.append(n)
        if self.bad_read: return 0
        b = self.handles[fd][self.pos:self.pos + n]
        c.memmove(buffer, b, len(b)); self.pos += len(b); return len(b)

    def seek(self, fd, off, origin):
        pos = off + (self.pos if origin == 1 else len(self.handles[fd]) if origin == 2 else 0)
        if not 0 <= pos <= len(self.handles[fd]): return -1
        self.pos = pos; return pos

    def pcm_open(self):
        self.opens += 1; return self.open_result

    def stop(self):
        self.stops += 1

    def write(self, buffer, n):
        if self.write_result is not None: return self.write_result
        if self.write_limit is not None: n = min(n, self.write_limit)
        self.pcm.append(c.string_at(buffer, n)); return n

    def out(self, _, vm, port, arg):
        if 96 <= port <= 103:
            return DLL.bbvm_audio_out(c.byref(self.audio), vm, port)
        if port == 27:
            self.tick += max(1, (self.vm.reg[7] + 24) // 25)
            self.poll(); return 1
        if 0 <= port <= 5: return 1
        if port == 46: self.vm.reg[7] = 0; return 1
        self.events.append(('unhandled_out', port)); return 0

    def inp(self, _, vm, port):
        return DLL.bbvm_audio_in(c.byref(self.audio), vm, port)

    def play(self, b, loop=0):
        self.files[b'demo.wav'] = b
        return DLL.bbvm_audio_play(c.byref(self.audio), b'demo.wav', loop)

    def tone(self, hz=880, ms=100):
        return DLL.bbvm_audio_tone(c.byref(self.audio), hz, ms)

    def poll(self): DLL.bbvm_audio_poll(c.byref(self.audio))

    def advance(self, n=1):
        for _ in range(n): self.tick = (self.tick + 1) & 0xffffffff; self.poll()

    def run_bin(self, image):
        buf = c.create_string_buffer(image)
        assert DLL.bbvm_load(c.byref(self.vm), buf, len(image))
        for _ in range(2000):
            DLL.bbvm_run(c.byref(self.vm), 256)
            self.advance()
            if not self.vm.running: break
        return self.vm.error, self.vm.running


class AudioTests(unittest.TestCase):
    def setUp(self): self.e = Env()
    def tearDown(self):
        DLL.bbvm_audio_stop(c.byref(self.e.audio)); DLL.bbvm_destroy(c.byref(self.e.vm))
        self.assertFalse(self.e.handles); self.assertFalse(self.e.allocations)

    def test_tone_and_drain(self):
        e = self.e; self.assertEqual(e.tone(), 0); e.poll()
        self.assertLessEqual(sum(map(len, e.pcm)), 2048)
        self.assertTrue(any(e.pcm[0])); e.advance(20)
        self.assertEqual((e.audio.opened, e.stops), (0, 1))

    def test_wav16_exact(self):
        e = self.e; data = struct.pack('<1024h', *range(1024))
        self.assertEqual(e.play(wav(data)), 0); e.poll()
        self.assertEqual(b''.join(e.pcm), data)
        e.advance(10); self.assertFalse(e.audio.opened)

    def test_wav8_unsigned_conversion(self):
        e = self.e; data = bytes(range(256)) * 2
        self.assertEqual(e.play(wav(data, bits=8)), 0); e.poll()
        self.assertEqual(e.pcm[0], struct.pack('<512h', *[(v-128)*256 for v in data]))

    def test_volume(self):
        e = self.e; DLL.bbvm_audio_volume(c.byref(e.audio), 128)
        self.assertEqual(e.play(wav(struct.pack('<512h', *([12000]*512)))), 0)
        e.poll(); self.assertEqual(struct.unpack_from('<h', e.pcm[0])[0], 6000)

    def test_muted_stream(self):
        e = self.e; DLL.bbvm_audio_volume(c.byref(e.audio), 0); e.tone(); e.poll()
        self.assertFalse(any(e.pcm[0]))

    def test_invalid_formats(self):
        for kw in [dict(rate=44100), dict(channels=2), dict(bits=24)]:
            with self.subTest(kw=kw):
                self.assertEqual(self.e.play(wav(bytes(3072), **kw)), -4)
                self.assertFalse(self.e.handles); self.assertFalse(self.e.audio.opened)

    def test_truncated_and_chunk_overflow(self):
        for data in [b'not wav', wav(bytes(1024))[:-1],
                     wav(bytes(1024))[:40] + b'\xff'*4 + bytes(1024)]:
            with self.subTest(length=len(data)): self.assertEqual(self.e.play(data), -3)

    def test_odd_padding_chunk(self):
        base = wav(bytes(1024)); b = base[:12]+b'JUNK'+struct.pack('<I',1)+b'x\0'+base[12:]
        b = b[:4]+struct.pack('<I',len(b)-8)+b[8:]
        self.assertEqual(self.e.play(b), 0)

    def test_missing_data(self):
        b = wav(bytes(1024)).replace(b'data', b'JUNK', 1)
        self.assertEqual(self.e.play(b), -3)

    def test_backpressure_no_io(self):
        e = self.e; e.play(wav(bytes(4096))); before = len(e.reads)
        e.ready = 0; e.poll(); e.advance(3)
        self.assertEqual(len(e.reads), before); self.assertFalse(e.pcm)

    def test_per_tick_deduplication(self):
        e = self.e; e.play(wav(bytes(16384))); e.poll(); n = len(e.pcm)
        for _ in range(20): e.poll()
        self.assertEqual(len(e.pcm), n); self.assertEqual(n, 2)

    def test_short_writes_no_skip(self):
        e = self.e; e.write_limit = 256
        data = struct.pack('<1024h', *range(1024))
        self.assertEqual(e.play(wav(data)), 0); e.advance(20)
        self.assertEqual(b''.join(e.pcm), data)

    def test_write_failures(self):
        for result in [-1, 3, 1026]:
            with self.subTest(result=result):
                e = self.e; e.play(wav(bytes(1024))); e.write_result = result; e.advance()
                self.assertEqual(e.audio.error, -5); self.assertFalse(e.audio.opened)

    def test_timeout_ready_and_zero_write(self):
        for mode in ['ready', 'write']:
            with self.subTest(mode=mode):
                e = self.e; e.ready = 1; e.write_result = None
                e.play(wav(bytes(4096)))
                if mode == 'ready': e.ready = 0
                else: e.write_result = 0
                e.advance(202); self.assertEqual(e.audio.error, -8)

    def test_negative_ready(self):
        e = self.e; e.tone(); e.ready = -1; e.advance()
        self.assertEqual(e.audio.error, -5)

    def test_read_error(self):
        e = self.e; e.play(wav(bytes(1024))); e.bad_read = True; e.advance()
        self.assertEqual(e.audio.error, -5)

    def test_loop_does_not_reopen_pcm(self):
        e = self.e; self.assertEqual(e.play(wav(bytes(1024)), 1), 0); e.advance(20)
        self.assertTrue(e.audio.opened); self.assertEqual(e.opens, 1)

    def test_pause_resume_no_stall_timeout(self):
        e = self.e; e.tone(ms=1000); e.poll()
        DLL.bbvm_audio_pause(c.byref(e.audio), 1); n = len(e.pcm); e.advance(400)
        self.assertEqual(len(e.pcm), n)
        DLL.bbvm_audio_pause(c.byref(e.audio), 0); e.poll()
        self.assertEqual(e.audio.error, 0); self.assertGreater(len(e.pcm), n)

    def test_mix_saturation(self):
        e = self.e; e.play(wav(struct.pack('<512h', *([32000]*512))))
        e.tone(); e.poll(); samples = struct.unpack('<512h', e.pcm[0])
        self.assertEqual(max(samples), 32767); self.assertGreaterEqual(min(samples), 26000)

    def test_argument_limits(self):
        e = self.e
        for hz, ms in [(39,100),(10001,100),(440,0),(440,60001)]:
            self.assertEqual(e.tone(hz,ms), -6)
        for v in [-1,257]: self.assertEqual(DLL.bbvm_audio_volume(c.byref(e.audio),v),-6)
        for path in [b'',b'a'*256]:
            self.assertEqual(DLL.bbvm_audio_play(c.byref(e.audio),path,0),-1)
        self.assertEqual(DLL.bbvm_audio_play(c.byref(e.audio),b'a',2),-6)

    def test_unsupported_abi(self):
        e = self.e; e.support = 0
        self.assertEqual(e.tone(), -7); self.assertFalse(e.opens)

    def test_open_failure_cleanup(self):
        e = self.e; e.open_result = 0
        self.assertEqual(e.play(wav(bytes(1024))), -7); self.assertFalse(e.handles)

    def test_tick_wrap(self):
        e = self.e; e.tick = 0xfffffffe; e.tone(); e.advance(20)
        self.assertFalse(e.audio.opened); self.assertEqual(e.audio.error, 0)

    def test_large_wav_streamed(self):
        e = self.e; e.play(wav(bytes(4*1024*1024))); e.poll()
        self.assertEqual(max(e.reads), 1024); self.assertLess(sum(e.reads), 4096)
        self.assertLess(c.sizeof(e.audio), 3500)

    def test_stop_idempotent(self):
        e = self.e; e.tone(); DLL.bbvm_audio_stop(c.byref(e.audio))
        DLL.bbvm_audio_stop(c.byref(e.audio)); self.assertEqual(e.stops,1)

    def test_bit_ports(self):
        e = self.e; e.vm.reg[7] = 0x80000005; e.vm.reg[6] = 33
        expected = [10,0x40000002,0x7ffffffa,1,0x80000025,0x80000024]
        for port, value in zip(range(40,46), expected):
            self.assertEqual(DLL.bbvm_in_port(c.byref(e.vm),port),value)

    def test_string_end_boundary(self):
        e = self.e; e.memory[-1] = 65
        self.assertEqual(DLL.bbvm_get_string(c.byref(e.vm),len(e.memory)-1),b'')
        self.assertEqual(e.vm.error,2)

    def test_append_self_across_realloc(self):
        e = self.e; h = DLL.bbvm_new_string(c.byref(e.vm)); text = b'AbcD'*7
        self.assertEqual(DLL.bbvm_set_string(c.byref(e.vm),h,text),1)
        e.vm.reg[7] = h; e.vm.reg[6] = h
        DLL.bbvm_in_port(c.byref(e.vm),6)
        self.assertEqual(DLL.bbvm_get_string(c.byref(e.vm),h),text*2)

    def test_alu_wrap_and_div_overflow(self):
        for op,a,b,result in [(0,0x7fffffff,1,0x80000000),(1,0x80000000,1,0x7fffffff),
                               (2,0xffffffff,2,0xfffffffe),(3,0x80000000,0xffffffff,0x80000000),
                               (4,0x80000000,0xffffffff,0)]:
            with self.subTest(op=op):
                image = b'\x10\x02'+struct.pack('<II',7,a)
                image += bytes([0xb0,(op<<4)|2])+struct.pack('<II',7,b)+b'\xf0'
                self.assertEqual(self.e.run_bin(image),(0,0)); self.assertEqual(self.e.vm.reg[7],result)

    def test_word_load(self):
        image=b'\x11\x02'+struct.pack('<II',7,0x1234)+b'\xf0'
        self.assertEqual(self.e.run_bin(image),(0,0)); self.assertEqual(self.e.vm.reg[7],0x1234)

    def test_reject_oversize_before_copy(self):
        e=self.e; small=c.create_string_buffer(16)
        self.assertEqual(DLL.bbvm_load(c.byref(e.vm),small,0xfffffff8),0)
        self.assertEqual(e.vm.error,1)

    def test_audio_port_version_and_unknown(self):
        e=self.e; self.assertEqual(DLL.bbvm_in_port(c.byref(e.vm),98),101)
        DLL.bbvm_in_port(c.byref(e.vm),12345); self.assertEqual(e.vm.error,6)

    def test_vm_tone_port_bytecode(self):
        # LD r3,880; LD r2,100; OUT 100,0; END. IDs are VM ABI.
        image = b'\x10\x02' + struct.pack('<II', 7, 880)
        image += b'\x10\x02' + struct.pack('<II', 6, 100)
        image += b'\x50\x0a' + struct.pack('<II', 100, 0) + b'\xf0'
        self.assertEqual(self.e.run_bin(image), (0, 0))
        self.assertTrue(self.e.pcm)

    def test_vm_audio_version_bytecode(self):
        image = b'\x40\x02' + struct.pack('<II', 7, 98) + b'\xf0'
        self.assertEqual(self.e.run_bin(image), (0, 0))
        self.assertEqual(self.e.vm.reg[7], 101)

    def test_large_and_nonfinite_sin_bounded(self):
        e=self.e
        for bits in [0x7f7fffff,0xff7fffff,0x7f800000,0x7fc00000]:
            e.vm.reg[7]=bits
            value=DLL.bbvm_in_port(c.byref(e.vm),16)
            number=struct.unpack('<f',struct.pack('<I',value))[0]
            self.assertLessEqual(abs(number),1.01)


if __name__=='__main__':
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(AudioTests)
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    (ROOT/'build/audio_test_report.json').write_text(json.dumps(dict(
        tests=result.testsRun, failures=len(result.failures), errors=len(result.errors),
        hardware_verified=False, engine_size=c.sizeof(Audio)),indent=2),encoding='utf-8')
    raise SystemExit(not result.wasSuccessful())
