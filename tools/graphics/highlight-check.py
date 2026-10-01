"""Verify transient outlines on a physical Mini, against the real C simulator."""
import argparse,hashlib,json,struct,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools/deploy'))
from emp import Link,field_record,crc32c,parse_ready,CH_SURFACE,OP_HELLO,OP_READY,OP_DESC_ACK,OP_PING,OP_PONG
from flash_usb import router_maintenance
from qualify import command,framebuffer
from report import counters
from PIL import Image,ImageChops
p=argparse.ArgumentParser();p.add_argument('--out',required=True);p.add_argument('--image',required=True);a=p.parse_args();out=Path(a.out);out.mkdir(parents=True,exist_ok=True)
r={'kind':'physical Mini; console-injected touch, no physical-finger claim','image_sha256':hashlib.sha256(Path(a.image).read_bytes()).hexdigest(),'captures':[]};url=router_maintenance(True);link=None
try:
 link=Link();link.send(OP_HELLO,struct.pack('<BBHQI',1,4,1016,0,16));m,_=link.wait_for(OP_READY,5);assert m;r['ready']=parse_ready(m['payload'])
 rev=0x700000003;bodies=[field_record(i,0,label,i%2) for i,label in enumerate(['Bypass','Mute','Gate','Monitor','Enable','Record','Solo','Output'])]
 link.send(0x20,struct.pack('<QHHI',rev,8,0,sum(map(len,bodies))),CH_SURFACE)
 for i,b in enumerate(bodies):link.send(0x21,struct.pack('<H',i)+b,CH_SURFACE)
 link.send(0x22,struct.pack('<QHI',rev,8,crc32c(b''.join(bodies))),CH_SURFACE);m,_=link.wait_for(OP_DESC_ACK,5);assert m
 command(link.c,'sim t 0',timeout=10);r['emp_before']=command(link.c,'emp',timeout=10).decode('latin1');r['id_before']=command(link.c,'id',timeout=10).decode('latin1')
 def highlight(slot,revision=rev,lease=1500):
  link.send(0x28,struct.pack('<Q B H H',revision,int(slot is not None),slot or 0,lease),CH_SURFACE);link.pump(.15)
 def capture(scene):
  framebuffer(link.c,out,scene);im=Image.open(out/(scene+'.png'));sim=Image.open(ROOT/'docs/graphics-evidence/highlights/simulator'/(scene+'.png'));diff=ImageChops.difference(im,sim);pixels=sum(x!=(0,0,0) for x in diff.get_flattened_data());assert pixels==0,(scene,pixels);r['captures'].append({'scene':scene,'different_pixels':pixels});print(scene,'matches simulator',flush=True)
 highlight(0);capture('toggles-remote');highlight(None) # EMP/1.3 compatibility
 link.send(0x29,struct.pack('<QBHHH',rev,2,0,1,1500),CH_SURFACE);link.pump(.15);capture('toggles-group')
 link.send(0x29,struct.pack('<QBH',rev,0,0),CH_SURFACE);link.pump(.15)
 r['group_ids']=[0,1]

 command(link.c,'sim t 1',timeout=10);link.pump(.1);capture('toggles-local');command(link.c,'sim t 0',timeout=10)
 for i in range(100):
  highlight(i%8,lease=1500);highlight(None)
  link.send(OP_PING,struct.pack('<IQ',i,i));m,_=link.wait_for(OP_PONG,5);assert m and struct.unpack_from('<I',m['payload'])[0]==i
 highlight(0);link.pump(1.7);capture('toggles-clear')
 highlight(0,revision=rev-1);capture('toggles-clear')
 r['update_batches']=100;r['pongs']=100;r['emp_after']=command(link.c,'emp',timeout=10).decode('latin1');r['id_after']=command(link.c,'id',timeout=10).decode('latin1')
 before=counters(r['emp_before']);after=counters(r['emp_after']);r['protocol_error_delta']={k:after[k]-before[k] for k in ['rx_decode_errors','rx_seq_gaps','tx_dropped']};assert not any(r['protocol_error_delta'].values()) and before['session_id']==after['session_id']
 before=counters(r['id_before']);after=counters(r['id_after']);r['device_error_delta']={k:after[k]-before[k] for k in ['graphics_errors','input_queue_drops','usb_tx_dropped','nmi_count']};assert not any(r['device_error_delta'].values())
 r['success']=True;print('100 highlight/clear pairs and pongs; stale revision, expiry and zero new errors passed.',flush=True)
finally:
 (out/'highlight-check.json').write_text(json.dumps(r,indent=2)+'\n')
 if link:link.close()
 if url:router_maintenance(False,url)
