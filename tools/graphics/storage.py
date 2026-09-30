"""Read-only snapshot of the Mini's persistent log; boot trust entries may change on updates."""
import argparse,hashlib,json,re,struct,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools/deploy'))
from flash_usb import router_maintenance
from link import open_app

def saved_records(block):
 if len(block)!=32768:raise ValueError('Incomplete persistent log')
 records={'touch_cal':None,'ui_preferences':None}
 for off in range(0,len(block),256):
  slot=block[off:off+256];magic,version=struct.unpack_from('<II',slot)
  if magic==0x54434131 and version==1:
   words=list(struct.unpack_from('<4I',slot));check=struct.unpack_from('<I',slot,32)[0]
   if check!=(~(sum(words)+sum(slot[16:32]))&0xffffffff):continue
   records['touch_cal']={'bytes_hex':slot[:36].hex(),'sha256':hashlib.sha256(slot[:36]).hexdigest()}
  elif magic==0x55495031 and version==1:
   words=struct.unpack_from('<6I',slot)
   if not 10<=words[3]<=100 or words[5]!=(~sum(words[:5])&0xffffffff):continue
   records['ui_preferences']={'bytes_hex':slot[:24].hex(),'sha256':hashlib.sha256(slot[:24]).hexdigest()}
 return records

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--out',required=True);args=ap.parse_args();out=Path(args.out);out.mkdir(parents=True,exist_ok=True)
 url=router_maintenance(True);dev=None
 try:
  dev=open_app();dev.drain();dev.write(b'dump 118000 8000\r');buf=bytearray();end=time.monotonic()+60
  while time.monotonic()<end:
   buf.extend(dev.read(4096));m=re.search(rb'DUMP 00118000 00008000\r\n',buf)
   if m and len(buf)>=m.end()+32768:
    block=bytes(buf[m.end():m.end()+32768]);(out/'persistent-log.bin').write_bytes(block)
    (out/'saved-records.json').write_text(json.dumps(saved_records(block),indent=2)+'\n');print('Read persistent calibration/preferences without writing flash',flush=True);return
  raise TimeoutError('Incomplete persistent log read')
 finally:
  if dev:dev.close()
  if url:router_maintenance(False,url)
if __name__=='__main__':main()
