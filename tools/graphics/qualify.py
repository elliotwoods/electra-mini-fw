"""Physical Mini evidence. Never substitutes simulator timings for device measurements."""
import argparse, hashlib, json, re, statistics, sys, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/deploy'))
from flash_usb import router_maintenance
from link import open_app
from PIL import Image
FIELDS=['index','submit_us','gpu_wait_us','pixel_spi_us','total_us','input_gap_ms','input_drops','accel_errors']
WORKLOADS=['value','precision','eight','colour','paging','settings-calibration','brightness']
def receive(dev, marker, timeout=1200):
    out=bytearray();end=time.monotonic()+timeout
    while time.monotonic()<end:
        chunk=dev.read(4096)
        if chunk:out.extend(chunk)
        if marker in out:return bytes(out)
    raise TimeoutError(f'No {marker!r}; received {len(out)} bytes')
def command(dev,line,marker=b'\r\n> ',timeout=1200):
    dev.drain();dev.write((line+'\r').encode());return receive(dev,marker,timeout)
def benchmark(dev,out,workload,count,run):
    raw=command(dev,f'gbench {workload:X} {count:X}',b'GBEND')
    (out/f'{WORKLOADS[workload]}-{run}.raw').write_bytes(raw)
    rows=[]
    for m in re.finditer(rb'GB ((?:[0-9A-F]{8} ){7}[0-9A-F]{8})\r\n',raw):
        rows.append(dict(zip(FIELDS,[int(x,16) for x in m[1].split()])))
    if len(rows)!=count or [x['index'] for x in rows]!=list(range(count)):
        raise ValueError(f'Incomplete benchmark: {len(rows)}/{count}')
    clock=re.search(rb'clock_cycles_per_100ms 0x([0-9A-Fa-f]+)',raw)
    if not clock or not 23500000<int(clock[1],16)<24500000:raise ValueError('Invalid 240 MHz clock qualification')
    result={'workload':WORKLOADS[workload],'run':run,'cycles_per_100ms':int(clock[1],16),'cold':rows[1] if workload==4 and len(rows)>1 else rows[0],'warmup':rows[1:31],'measured':rows[31:]}
    (out/f'{WORKLOADS[workload]}-{run}.json').write_text(json.dumps(result,indent=2)+'\n')
    print(WORKLOADS[workload],run,'max_us',max(x['total_us'] for x in rows),'gap_ms',max(x['input_gap_ms'] for x in rows),'errors',max(x['accel_errors'] for x in rows),flush=True)
    return result
def screenshot(dev,out,scene):
    command(dev,'gscene '+scene,b'GSCENE done')
    dev.drain();dev.write(b'shot\r');buf=bytearray();end=time.monotonic()+120
    while time.monotonic()<end:
        buf.extend(dev.read(4096));m=re.search(rb'SHOT ([0-9A-Fa-f]+) ([0-9A-Fa-f]+)\r?\n',buf)
        if not m:continue
        w,h=[int(x,16) for x in m.groups()];want=w*h*2
        if len(buf)-m.end()<want:continue
        raw=bytes(buf[m.end():m.end()+want]);(out/(scene+'.rgb565')).write_bytes(raw)
        pixels=[]
        for i in range(0,want,2):
            c=raw[i]|raw[i+1]<<8;pixels.append((((c>>11)&31)*255//31,((c>>5)&63)*255//63,(c&31)*255//31))
        im=Image.new('RGB',(w,h));im.putdata(pixels);im.save(out/(scene+'.png'))
        print('device capture',scene,w,h,flush=True);return
    raise TimeoutError('Incomplete screenshot; refusing a partial image')
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--out',required=True);ap.add_argument('--image');ap.add_argument('--runs',type=int,default=3);ap.add_argument('--count',type=int,default=331);ap.add_argument('--workloads',default='0,1,2,3,4,5,6');ap.add_argument('--scenes',default='');ap.add_argument('--mixed-seconds',type=int,default=0);args=ap.parse_args()
    out=Path(args.out);out.mkdir(parents=True,exist_ok=True)
    meta={'utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'transport':'physical Mini / macOS IOKit'}
    if args.image:meta['image_sha256']=hashlib.sha256(Path(args.image).read_bytes()).hexdigest()
    router=router_maintenance(True);dev=None
    try:
        dev=open_app();meta['identity']=command(dev,'id').decode('latin1');meta['emp_before']=command(dev,'emp').decode('latin1');(out/'manifest.json').write_text(json.dumps(meta,indent=2)+'\n')
        for scene in filter(None,args.scenes.split(',')):screenshot(dev,out,scene)
        results=[]
        for run in range(args.runs):
            for workload in map(int,filter(None,args.workloads.split(','))):results.append(benchmark(dev,out,workload,args.count,run))
        start=time.monotonic();n=0
        while time.monotonic()-start<args.mixed_seconds:
            results.append(benchmark(dev,out,n%7,8,100+n));n+=1
        meta['mixed_seconds']=time.monotonic()-start;meta['identity_after']=command(dev,'id').decode('latin1');meta['emp_after']=command(dev,'emp').decode('latin1')
        (out/'manifest.json').write_text(json.dumps(meta,indent=2)+'\n')
        summary={}
        for name in WORKLOADS:
            rows=[row for r in results if r['workload']==name for row in r['measured']]
            if rows:
                summary[name]={key:{'median':statistics.median(x[key] for x in rows),'p95':sorted(x[key] for x in rows)[int((len(rows)-1)*.95)],'max':max(x[key] for x in rows)} for key in FIELDS[1:]}
        (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    finally:
        if dev:dev.close()
        if router:router_maintenance(False,router)
if __name__=='__main__':main()
