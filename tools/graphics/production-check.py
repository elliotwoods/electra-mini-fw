"""Read-only production identity, reconnect and EMP integrity checks."""
import argparse,json,struct,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools/deploy'))
from emp import Link,OP_PING,OP_PONG,OP_ECHO,OP_ECHO_REPLY
from mixed import hello
from qualify import command,framebuffer
from flash_usb import router_maintenance
from report import counters
p=argparse.ArgumentParser();p.add_argument('--out',required=True);a=p.parse_args();out=Path(a.out);out.mkdir(parents=True,exist_ok=True);url=router_maintenance(True);link=None;r={}
try:
 link=Link();r['ready']=hello(link);r['id_before']=command(link.c,'id').decode('latin1');r['emp_before']=command(link.c,'emp').decode('latin1');r['persistent_boot_record']=command(link.c,'pers',b'> ',timeout=10).decode('latin1')
 assert 'record MATCHES this image' in r['persistent_boot_record'] and counters(r['persistent_boot_record'])['proven']==0x4F4B4159,'Image not proven'
 link.dec.buf.clear();link.dec.need=0;r['pings']=[];r['echo_bytes']=[]
 for i in range(5):
  link.send(OP_PING,struct.pack('<IQ',i,1000+i));m,dt=link.wait_for(OP_PONG,5)
  assert m and struct.unpack_from('<IQ',m['payload'])==(i,1000+i),'Ping mismatch'
  r['pings'].append(dt*1000)
 for n in [16,512,900]:
  blob=bytes((i*7+n)&255 for i in range(n));link.send(OP_ECHO,blob);m,_=link.wait_for(OP_ECHO_REPLY,5);assert m and m['payload']==blob,'Echo mismatch';r['echo_bytes'].append(n)
 seq=link.seq;link.close();link=Link();link.seq=seq;r['reconnected_ready']=hello(link);assert r['reconnected_ready']['session_id']==r['ready']['session_id'],'Reset on reconnect'
 r['id_after']=command(link.c,'id').decode('latin1');r['emp_after']=command(link.c,'emp').decode('latin1')
 before=counters(r['emp_before']);after=counters(r['emp_after']);r['protocol_error_delta']={k:after[k]-before[k] for k in ['rx_decode_errors','rx_seq_gaps','tx_dropped']};assert not any(r['protocol_error_delta'].values())
 ids=counters(r['id_after']);assert ids['usb_tx_dropped']==counters(r['id_before'])['usb_tx_dropped'];assert not any(ids[k] for k in ['graphics_errors','input_queue_drops','nmi_count'])
 framebuffer(link.c,out,'production');r['success']=True;print('Production identity, five pings, three exact echoes, reconnect and zero diagnostics passed.')
finally:
 (out/'production-check.json').write_text(json.dumps(r,indent=2)+'\n')
 if link:link.close()
 if url:router_maintenance(False,url)
