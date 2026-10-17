"""Actual C stream parser/VM ports, plus host packer. No physical audio claim."""
import ctypes as c
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

from test_audio import DLL, Env, ROOT, wav
sys.path.insert(0, str(ROOT/'tools'))
from pack_audio import pack_members, pack_files


def library(*members):
    data = bytearray(16+8*len(members))
    for index, member in enumerate(members):
        data.extend(bytes((-len(data)) % 4))
        struct.pack_into('<II', data, 16+8*index, len(data), len(member))
        data.extend(member)
    struct.pack_into('<4sIII', data, 0, b'BBAL', 1, len(members), len(data))
    return bytes(data)


class LibraryTests(unittest.TestCase):
    def setUp(self): self.e = Env()
    def tearDown(self):
        DLL.bbvm_audio_stop(c.byref(self.e.audio)); DLL.bbvm_destroy(c.byref(self.e.vm))
        self.assertFalse(self.e.handles); self.assertFalse(self.e.allocations)

    def play(self, data, resource=1, loop=0):
        self.e.files[b'sound.lib'] = data
        return DLL.bbvm_audio_play_resource(c.byref(self.e.audio), b'sound.lib', resource, loop)

    def test_selected_member_exact_and_neighbor_never_played(self):
        a=struct.pack('<1024h', *range(1024))
        b=struct.pack('<1024h', *(-x for x in range(1024)))
        data=library(wav(a), wav(b))
        for index, expected in [(1,a), (2,b)]:
            with self.subTest(index=index):
                self.e.pcm.clear()
                self.assertEqual(self.play(data,index),0)
                self.e.advance(20)
                self.assertEqual(b''.join(self.e.pcm), expected)
                self.assertFalse(self.e.audio.opened)

    def test_loop_restarts_selected_member_absolute_offset(self):
        samples=struct.pack('<512h', *([1234]*512))
        data=library(wav(bytes(1024)), wav(samples), wav(bytes(1024)))
        self.assertEqual(self.play(data,2,1),0)
        self.e.advance(12)
        self.assertGreater(len(self.e.pcm),4)
        self.assertTrue(all(p==samples for p in self.e.pcm))
        self.assertTrue(self.e.audio.opened)

    def test_pcm8_member_reuses_conversion(self):
        self.assertEqual(self.play(library(wav(bytes([0,128,255,128]),bits=8))),0)
        self.e.poll()
        self.assertEqual(self.e.pcm[0][:6],struct.pack('<3h',-32768,0,32512))

    def test_correctly_padded_odd_wav_does_not_read_next_member(self):
        member=bytearray(wav(bytes([0,128,255]),bits=8))
        member.append(0);struct.pack_into('<I',member,4,len(member)-8)
        self.assertEqual(self.play(library(member,wav(bytes(1024)))),0)
        self.e.poll()
        self.assertEqual(self.e.pcm[0][:6],struct.pack('<3h',-32768,0,32512))

    def test_invalid_ids_missing_file_and_argument(self):
        data=library(wav(bytes(1024)))
        for index in [-1,0,2,65536,0x7fffffff]:
            with self.subTest(index=index): self.assertEqual(self.play(data,index),-10)
        self.assertEqual(self.play(data,1,2),-6)
        self.assertEqual(DLL.bbvm_audio_play_resource(c.byref(self.e.audio),b'missing.lib',1,0),-2)
        self.assertFalse(self.e.handles)

    def test_header_rejected_before_pcm_open(self):
        data=library(wav(bytes(1024)))
        cases=[b'',data[:15],b'JUNK'+data[4:],data[:-1]]
        for offset,value in [(4,2),(8,0),(8,65536),(8,0xffffffff),(12,0xffffffff),(12,15)]:
            bad=bytearray(data); struct.pack_into('<I',bad,offset,value); cases.append(bytes(bad))
        for bad in cases:
            with self.subTest(header=bad[:16].hex()): self.assertEqual(self.play(bad),-9)
        self.assertFalse(self.e.handles); self.assertEqual(self.e.opens,0)

    def test_index_offsets_and_lengths_bounded(self):
        data=library(wav(bytes(1024)))
        for offset,size in [(0,100),(20,100),(25,100),(0xfffffffc,100),
                            (24,0),(24,43),(24,len(data)),(24,0xffffffff)]:
            bad=bytearray(data);struct.pack_into('<II',bad,16,offset,size)
            with self.subTest(offset=offset,size=size): self.assertEqual(self.play(bytes(bad)),-9)
        self.assertFalse(self.e.handles);self.assertEqual(self.e.opens,0)

    def test_wav_cannot_escape_member_into_next_entry(self):
        first=bytearray(wav(bytes(1024)))
        struct.pack_into('<I',first,4,4096)
        self.assertEqual(self.play(library(first,wav(bytes(8192)))),-3)
        first=bytearray(wav(bytes(1024)))
        struct.pack_into('<I',first,40,2048)
        self.assertEqual(self.play(library(first,wav(bytes(8192)))),-3)
        self.assertEqual(self.e.opens,0); self.assertFalse(self.e.handles)

    def test_short_read_cleanup_and_unsupported_format(self):
        self.e.bad_read=True
        self.assertEqual(self.play(library(wav(bytes(1024)))),-5)
        self.assertFalse(self.e.handles)
        self.e.bad_read=False
        self.assertEqual(self.play(library(wav(bytes(1024),rate=44100))),-4)
        self.assertFalse(self.e.handles)

    def test_payload_short_read_closes_pcm(self):
        self.assertEqual(self.play(library(wav(bytes(4096)))),0)
        self.e.bad_read=True;self.e.advance()
        self.assertEqual(self.e.audio.error,-5)
        self.assertFalse(self.e.handles);self.assertEqual(self.e.stops,1)

    def test_pause_volume_backpressure_and_replacement(self):
        data=library(wav(struct.pack('<2048h', *([12000]*2048))))
        self.assertEqual(self.play(data,1,1),0)
        self.e.ready=0; before=len(self.e.reads);self.e.advance()
        self.assertEqual(len(self.e.reads),before)
        self.e.ready=1;DLL.bbvm_audio_pause(c.byref(self.e.audio),1)
        self.e.advance(10);self.assertFalse(self.e.pcm)
        DLL.bbvm_audio_volume(c.byref(self.e.audio),128)
        DLL.bbvm_audio_pause(c.byref(self.e.audio),0); self.e.advance()
        self.assertEqual(struct.unpack('<h',self.e.pcm[0][:2])[0],6000)
        self.assertEqual(self.e.play(wav(bytes(1024))),0)
        self.assertEqual(self.e.stops,1);self.assertEqual(self.e.opens,2)

    def test_large_library_lookup_and_streaming_memory_constant(self):
        members=[wav(bytes(32))]*65534+[wav(bytes(4*1024*1024))]
        data=library(*members)
        self.assertEqual(self.play(data,65535),0)
        self.e.poll()
        self.assertEqual(max(self.e.reads),1024)
        self.assertLess(sum(self.e.reads),4096)
        self.assertLess(c.sizeof(self.e.audio),3500)

    def test_resource_play_port_argument_forwarding(self):
        self.e.files[b'sound.lib'] = library(
            wav(bytes(1024)), wav(struct.pack('<512h', *([-2000] * 512))))
        handle = DLL.bbvm_new_string(c.byref(self.e.vm))
        self.assertEqual(DLL.bbvm_set_string(c.byref(self.e.vm), handle, b'sound.lib'), 1)
        self.e.vm.reg[7] = handle
        self.e.vm.reg[6] = 2
        self.e.vm.reg[5] = 1
        self.assertEqual(DLL.bbvm_audio_out(c.byref(self.e.audio), c.byref(self.e.vm), 103), 1)
        self.assertEqual(self.e.vm.reg[7], 0)
        self.e.advance(12)
        self.assertGreater(len(self.e.pcm), 4)
        self.assertTrue(all(struct.unpack_from('<h', block)[0] == -2000 for block in self.e.pcm))



class PackerTests(unittest.TestCase):
    def test_binary_layout_stable_ids_and_alignment(self):
        members=[('odd',wav(bytes([128,0,255]),bits=8)),('normal',wav(bytes(1024)))]
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'sound.lib'
            report=pack_members(iter(members),2,path);data=path.read_bytes()
            self.assertEqual(struct.unpack_from('<4sIII',data),(b'BBAL',1,2,len(data)))
            for index,(_,payload) in enumerate(members):
                offset,size=struct.unpack_from('<II',data,16+index*8)
                self.assertEqual(offset%4,0);self.assertEqual(data[offset:offset+size],payload)
                self.assertEqual(report['sounds'][index]['id'],index+1)

    def test_atomic_failure_and_declared_count(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'keep.lib';path.write_bytes(b'original')
            for members,count in [([],0),([],1),([('bad',b'broken')],1),
                                   ([('a',wav(bytes(16))),('b',b'bad')],1)]:
                with self.assertRaises(ValueError): pack_members(iter(members),count,path)
                self.assertEqual(path.read_bytes(),b'original')
            self.assertEqual([p.name for p in Path(directory).iterdir()],['keep.lib'])

    def test_file_manifest_rejects_overwrite_and_duplicate_ids(self):
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/'source.wav';original=wav(bytes(1024));source.write_bytes(original)
            with self.assertRaises(ValueError):pack_files([dict(id=1,file=source)],source)
            with self.assertRaises(ValueError):pack_files([dict(id=2,file=source)],source.with_suffix('.lib'))
            with self.assertRaises(ValueError):
                pack_files([dict(id=1,file=source),dict(id=1,file=source)],source.with_suffix('.lib'))
            self.assertEqual(source.read_bytes(),original)

    def test_files_normalized_directly_into_library_and_cli_metadata(self):
        import subprocess
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=root/'原音.wav'
            original=wav(struct.pack('<1024h',*range(1024)),rate=44100,channels=2)
            source.write_bytes(original)
            manifest=root/'sounds.json'
            manifest.write_text(json.dumps(dict(sounds=[dict(id=1,name='确认',file=source.name)])),encoding='utf-8')
            output=root/'sound.lib';index=root/'index.json'
            result=subprocess.run([sys.executable,str(ROOT/'tools/pack_audio.py'),str(manifest),
                                   str(output),'--index',str(index)],capture_output=True,timeout=20)
            self.assertEqual(result.returncode,0,result.stderr)
            data=output.read_bytes();offset,size=struct.unpack_from('<II',data,16)
            self.assertEqual(data[offset:offset+4],b'RIFF')
            self.assertEqual(struct.unpack_from('<I',data,offset+24)[0],22050)
            self.assertEqual(struct.unpack_from('<H',data,offset+22)[0],1)
            self.assertEqual(size,44+512)
            self.assertEqual(json.loads(index.read_text(encoding='utf-8'))['sounds'][0]['name'],'确认')
            self.assertEqual(source.read_bytes(),original)
            self.assertEqual(len(list(root.glob('*.wav'))),1,'No loose converted WAV files')


if __name__ == '__main__':
    suite=unittest.TestSuite(
        unittest.defaultTestLoader.loadTestsFromTestCase(test_class)
        for test_class in [LibraryTests,PackerTests])
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    (ROOT/'build/audio_library_report.json').write_text(json.dumps(dict(
        tests=result.testsRun,failures=len(result.failures),errors=len(result.errors),
        hardware_verified=False,format='BBAL v1'),indent=2),encoding='utf-8')
    raise SystemExit(not result.wasSuccessful())
