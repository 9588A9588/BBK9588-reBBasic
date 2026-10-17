from pathlib import Path
import importlib.util
import io
import struct
import tempfile
import unittest
import wave

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('convert_wav',ROOT/'tools/convert_wav.py')
module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)

def wav(data,width=2,channels=1,rate=22050):
    b=io.BytesIO()
    with wave.open(b,'wb') as w:
        w.setnchannels(channels); w.setsampwidth(width); w.setframerate(rate); w.writeframes(data)
    return b.getvalue()

class WavToolsTests(unittest.TestCase):
    def run_conversion(self,data):
        with tempfile.TemporaryDirectory() as d:
            src=Path(d)/'source.wav'; out=Path(d)/'converted.wav'; src.write_bytes(data)
            module.convert(src,out)
            self.assertEqual(src.read_bytes(),data)
            with wave.open(str(out),'rb') as w:
                self.assertEqual((w.getnchannels(),w.getsampwidth(),w.getframerate()),(1,2,22050))
                return w.readframes(w.getnframes())
    def test_native_identity(self):
        samples=struct.pack('<4h',-32768,-1,0,32767)
        self.assertEqual(self.run_conversion(wav(samples)),samples)
    def test_stereo_downsample(self):
        data=struct.pack('<200h',*([10000,20000]*100))
        converted=self.run_conversion(wav(data,channels=2,rate=44100))
        self.assertEqual(converted,struct.pack('<50h',*([15000]*50)))
    def test_8bit_conversion(self):
        self.assertEqual(self.run_conversion(wav(bytes([0,128,255]),width=1)),
                         struct.pack('<3h',-32768,0,32512))
    def test_single_high_rate_frame(self):
        self.assertEqual(self.run_conversion(wav(struct.pack('<h',42),rate=44100)),struct.pack('<h',42))
    def test_truncation_does_not_write_output(self):
        with tempfile.TemporaryDirectory() as d:
            src=Path(d)/'bad.wav'; out=Path(d)/'out.wav'
            src.write_bytes(wav(bytes(32))[:-1])
            with self.assertRaisesRegex(ValueError,'Truncated'): module.convert(src,out)
            self.assertFalse(out.exists())
    def test_no_source_overwrite(self):
        with tempfile.TemporaryDirectory() as d:
            src=Path(d)/'keep.wav'; data=wav(bytes(32)); src.write_bytes(data)
            with self.assertRaises(ValueError): module.convert(src,src)
            self.assertEqual(src.read_bytes(),data)
    def test_24bit_rejected(self):
        with self.assertRaises(ValueError): self.run_conversion(wav(bytes(30),width=3))

if __name__=='__main__': unittest.main(verbosity=2)
