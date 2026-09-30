"""Ten-minute real EMP stream on a live renderer, with echo, ping and reconnect checks."""
import argparse,json,re,struct,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools/deploy'))
from flash_usb import router_maintenance
from emp import Link,field_record,crc32c,parse_ready,parse_heartbeat,parse_edit,OP_HELLO,OP_READY,OP_PING,OP_PONG,OP_ECHO,OP_ECHO_REPLY,OP_HEARTBEAT,OP_DESC_ACK,CH_SURFACE
from qualify import command

def hello(link):
 link.send(OP_HELLO,struct.pack('<BBHQI',1,2,1016,0,16));m,_=link.wait_for(OP_READY,5)
 if not m:raise RuntimeError('No READY')
 return parse_ready(m['payload'])
def descriptor(link,rev):
 bodies=[field_record(k,1,'Mixed control '+str(k),.5,lo=-1,hi=1,step=.001,precision=3) for k in range(12)]
 link.send(0x20,struct.pack('<QHHI',rev,len(bodies),0,sum(map(len,bodies))),CH_SURFACE)
 for i,b in enumerate(bodies):link.send(0x21,struct.pack('<H',i)+b,CH_SURFACE)
 link.send(0x22,struct.pack('<QHI',rev,len(bodies),crc32c(b''.join(bodies))),CH_SURFACE)
 m,_=link.wait_for(OP_DESC_ACK,5)
 if not m:raise RuntimeError('Descriptor not accepted')
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--out',required=True);ap.add_argument('--seconds',type=int,default=600);args=ap.parse_args();out=Path(args.out);out.mkdir(parents=True,exist_ok=True)
 url=router_maintenance(True);link=None;result={'updates':0,'pongs':0,'echoes':0,'heartbeats':[],'input_messages':[],'diagnostics':[],'reconnects':0,'ping_latency_ms':[]}
 try:
  link=Link();command(link.c,'gscene live',b'GSCENE done');result['ready']=hello(link);rev=0xBEEF0030;descriptor(link,rev)
  result['id_before']=command(link.c,'id').decode('latin1');result['emp_before']=command(link.c,'emp').decode('latin1')
  # Reading console counters also consumes protocol frames, so start the decoder cleanly.
  link.dec.buf.clear();link.dec.need=0
  start=time.monotonic();next_ping=start;ping_id=0;pending={};last_reconnect=start;last_page=-1;acks={};echo_expected={}
  def process(messages):
   for m in messages:
    if 'error' in m:raise RuntimeError(m)
    op=m['opcode'];p=m['payload']
    if op==OP_PONG:
     pid=struct.unpack_from('<I',p)[0];sent=pending.pop(pid,None)
     if sent is not None:result['ping_latency_ms'].append((time.monotonic()-sent)*1000)
     result['pongs']+=1
    elif op==OP_ECHO_REPLY:
     if p not in echo_expected:raise RuntimeError('Unexpected or corrupted echo')
     echo_expected.pop(p);result['echoes']+=1
    elif op==OP_HEARTBEAT:
     hb=parse_heartbeat(p)
     if hb['session_id']!=result['ready']['session_id']:raise RuntimeError('Device reset during mixed run')
     result['heartbeats'].append(hb)
    elif op==6:result['diagnostics'].append(p.hex())
    elif op in (0x50,0x54,0x58,0x56):
     result['input_messages'].append({'opcode':op,'payload':p.hex()})
     if op==0x50:
      edit=parse_edit(p);acks[edit['id']]=edit['edit_seq']
  while time.monotonic()-start<args.seconds:
   now=time.monotonic();step=result['updates'];n=1 if step%3==0 else 8
   payload=struct.pack('<QH',rev,n)
   for k in range(n):payload+=struct.pack('<HIBd',k,acks.get(k,0),1,((step+k)%190-95)/100)
   link.send(0x24,payload,CH_SURFACE);result['updates']+=1
   page=int((now-start)//12)%2
   if page!=last_page:link.send(0x25,struct.pack('<QH',rev,8 if page else 0),CH_SURFACE);last_page=page
   if now>=next_ping:
    ping_id+=1;link.send(OP_PING,struct.pack('<IQ',ping_id,int(time.time()*1e6)));pending[ping_id]=now
    blob=bytes([ping_id%256])*64;echo_expected[blob]=now;link.send(OP_ECHO,blob);next_ping=now+1
   process(link.pump(.03))
   if pending and now-min(pending.values())>5:raise RuntimeError('Lost ping / stalled device')
   if echo_expected and now-min(echo_expected.values())>5:raise RuntimeError('Lost echo reply')
   if now-last_reconnect>120 and not pending and not echo_expected:
    sequence=link.seq;link.close();link=Link();link.seq=sequence;ready=hello(link)
    if ready['session_id']!=result['ready']['session_id']:raise RuntimeError('Device reset during mixed run')
    pending.clear();last_reconnect=now;result['reconnects']+=1
   if step%250==0:print('mixed elapsed',round(now-start),'updates',step,'pongs',result['pongs'],flush=True)
  result['elapsed_seconds']=time.monotonic()-start
  drain_start=time.monotonic()
  while (pending or echo_expected) and time.monotonic()-drain_start<5:process(link.pump(.03))
  result['reply_drain_seconds']=time.monotonic()-drain_start
  result['requests']=ping_id
  if pending or echo_expected or result['pongs']!=ping_id or result['echoes']!=ping_id:raise RuntimeError('Missing final ping/echo replies')
  result['pending_pings_at_end']=len(pending);result['pending_echoes_at_end']=len(echo_expected)
  result['id_after']=command(link.c,'id').decode('latin1');result['emp_after']=command(link.c,'emp').decode('latin1')
  def counters(s):return {k:int(v,16) for k,v in re.findall(r'([a-z_]+) 0x([0-9A-Fa-f]+)',s)}
  before=counters(result['emp_before']);after=counters(result['emp_after'])
  result['protocol_error_delta']={k:after[k]-before[k] for k in ('rx_decode_errors','rx_seq_gaps','tx_dropped')}
  if any(result['protocol_error_delta'].values()):raise RuntimeError('New protocol errors')
  ids=counters(result['id_after'])
  if any(ids[k] for k in ('input_queue_drops','graphics_errors','usb_tx_dropped','nmi_count')):raise RuntimeError('Device diagnostics failed')
  if result['diagnostics']:raise RuntimeError('Device emitted a diagnostic')
  result['success']=True
 finally:
  (out/'mixed.json').write_text(json.dumps(result,indent=2)+'\n')
  if link:link.close()
  if url:router_maintenance(False,url)
if __name__=='__main__':main()
