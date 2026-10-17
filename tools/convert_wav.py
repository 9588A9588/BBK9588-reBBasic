from pathlib import Path
import argparse, array, io, sys, wave

def wav_bytes(source):
    """Convert one source in host memory; no intermediate device/host WAV file."""
    with wave.open(str(source),'rb') as w:
        width, channels, rate, frames=w.getsampwidth(),w.getnchannels(),w.getframerate(),w.getnframes()
        if width not in (1,2) or channels not in (1,2) or w.getcomptype()!='NONE':
            raise ValueError('Input must be uncompressed 8/16-bit mono/stereo WAV')
        if not frames or frames>rate*600: raise ValueError('Input must be 0..10 minutes, nonempty')
        raw=w.readframes(frames)
        if len(raw)!=frames*channels*width:
            raise ValueError('Truncated WAV sample data')
    if width==2:
        values=array.array('h'); values.frombytes(raw)
        if sys.byteorder!='little': values.byteswap()
    else: values=[(x-128)*256 for x in raw]
    mono=[sum(values[i*channels:(i+1)*channels])//channels for i in range(frames)]
    count=max(1,frames*22050//rate)
    result=array.array('h')
    for i in range(count):
        numerator=i*rate; pos,frac=divmod(numerator,22050)
        a=mono[min(pos,frames-1)]; b=mono[min(pos+1,frames-1)]
        result.append((a*(22050-frac)+b*frac)//22050)
    if sys.byteorder!='little': result.byteswap()
    result_bytes=io.BytesIO()
    with wave.open(result_bytes,'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(22050); w.writeframes(result.tobytes())
    return result_bytes.getvalue()


def convert(source, output):
    if source.resolve()==output.resolve(): raise ValueError('Keep original; use another output path')
    payload=wav_bytes(source)
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_bytes(payload)
    print(output, '22050 Hz / PCM16 / mono')

if __name__=='__main__':
    ap=argparse.ArgumentParser(description='Convert a WAV asset offline for BBVM Audio')
    ap.add_argument('source',type=Path); ap.add_argument('output',type=Path)
    a=ap.parse_args(); convert(a.source,a.output)
