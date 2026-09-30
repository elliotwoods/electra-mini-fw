"""Exercise and capture the boolean switch through real EMP on a connected Mini."""
import argparse,hashlib,json,struct,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools/deploy'))
from emp import Link,field_record,crc32c,OP_DESC_ACK,CH_SURFACE,OP_PING,OP_PONG
from flash_usb import router_maintenance
from mixed import hello
from qualify import command,framebuffer
from report import counters
from PIL import Image,ImageChops
p=argparse.ArgumentParser();p.add_argument('--out',required=True);a=p.parse_args();out=Path(a.out);out.mkdir(parents=True,exist_ok=True);url=router_maintenance(True);link=None;r={'kind':'physical Mini / framebuffer readback','captures':[]}
try:
 link=Link();r['ready']=hello(link);rev=0x700000001
 bodies=[field_record(i,0,label,i%2) for i,label in enumerate(['Bypass','Mute','Gate','Monitor','Enable','Record','Solo','Output'])]
 link.send(0x20,struct.pack('<QHHI',rev,8,0,sum(map(len,bodies))),CH_SURFACE)
 for i,b in enumerate(bodies):link.send(0x21,struct.pack('<H',i)+b,CH_SURFACE)
 link.send(0x22,struct.pack('<QHI',rev,8,crc32c(b''.join(bodies))),CH_SURFACE);m,_=link.wait_for(OP_DESC_ACK,5);assert m,'Descriptor rejected'
 r['id_before']=command(link.c,'id').decode('latin1');r['emp_before']=command(link.c,'emp').decode('latin1')
 def values(focused=None,state=0):
  payload=struct.pack('<QH',rev,8)
  for k in range(8):payload+=struct.pack('<HIBB',k,0,0,state if k==focused else k%2)
  link.send(0x24,payload,CH_SURFACE);link.pump(.25)
 def capture(scene):
  time.sleep(.15);framebuffer(link.c,out,scene)
  sim=ROOT/'docs/graphics-evidence/toggles/simulator'/(scene+'.png')
  a=Image.open(out/(scene+'.png'));b=Image.open(sim);diff=ImageChops.difference(a,b)
  pixels=sum(x!=(0,0,0) for x in diff.get_flattened_data());r['captures'].append({'scene':scene,'different_pixels':pixels,'sha256':hashlib.sha256((out/(scene+'.png')).read_bytes()).hexdigest()});print(scene,'different pixels',pixels,flush=True)
 values();capture('toggles')
 for knob,tag in [(0,''),(4,'-bottom')]:
  command(link.c,'sim p '+str(knob));values(knob,0);capture('toggle-focus'+tag+'-off')
  values(knob,1);capture('toggle-focus'+tag+'-on');command(link.c,'sim p '+str(knob));values()
 link.dec.buf.clear();link.dec.need=0
 for i in range(100):
  payload=struct.pack('<QH',rev,8)+b''.join(struct.pack('<HIBB',k,0,0,(i+k)%2) for k in range(8));link.send(0x24,payload,CH_SURFACE)
  link.send(OP_PING,struct.pack('<IQ',i,i));m,_=link.wait_for(OP_PONG,5);assert m and struct.unpack_from('<I',m['payload'])[0]==i,'Lost ping';time.sleep(.03)
 values();r['update_batches']=100;r['pongs']=100
 r['id_after']=command(link.c,'id').decode('latin1');r['emp_after']=command(link.c,'emp').decode('latin1')
 before=counters(r['emp_before']);after=counters(r['emp_after']);r['protocol_error_delta']={k:after[k]-before[k] for k in ['rx_decode_errors','rx_seq_gaps','tx_dropped']};assert not any(r['protocol_error_delta'].values()) and before['session_id']==after['session_id']
 initial=counters(r['id_before']);ids=counters(r['id_after']);r['device_error_delta']={k:ids[k]-initial[k] for k in ['graphics_errors','input_queue_drops','usb_tx_dropped','nmi_count']};assert not any(r['device_error_delta'].values())
 assert not any(x['different_pixels'] for x in r['captures']),'Framebuffer differs from simulator'
 r['success']=True;print('100 eight-toggle batches, exact framebuffer parity and zero new errors passed.',flush=True)
finally:
 (out/'toggle-check.json').write_text(json.dumps(r,indent=2)+'\n')
 if link:link.close()
 if url:router_maintenance(False,url)
