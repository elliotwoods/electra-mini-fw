"""Publish completed physical measurements, refusing incomplete or failed matrices."""
import hashlib,json,pathlib,re,shutil,statistics,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[2]
Q=ROOT/'build/qualification';OUT=ROOT/'docs/graphics-evidence/device'
SIZE='/Users/elliot/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-size'
BUILDS=[('baseline','baseline',pathlib.Path('/private/tmp/electra-mini-legacy/build')),('ra','ra',ROOT/'build/ra-bench'),('dave','dave-final',ROOT/'build/dave')]
WORKLOADS=['value','precision','eight','colour','paging','settings-calibration','brightness']
def counters(s):return {k:int(v,16) for k,v in re.findall(r'([a-z_]+) 0x([0-9A-Fa-f]+)',s)}
def stats(values):
 values=sorted(values)
 return {'median':statistics.median(values),'p95':values[int((len(values)-1)*.95)],'max':max(values)}
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 rows=[];memory=[];validated=[]
 for name,folder,build in BUILDS:
  source=Q/folder;meta=json.loads((source/'manifest.json').read_text())
  if meta['image_sha256']!=sha(build/'app.srec'):raise ValueError('Image changed since measurement: '+name)
  before=counters(meta['emp_before']);after=counters(meta['emp_after'])
  if before['session_id']!=after['session_id']:raise ValueError('Reset during benchmark: '+name)
  delta={k:after[k]-before[k] for k in ('rx_decode_errors','rx_seq_gaps','tx_dropped')}
  if any(delta.values()):raise ValueError('Protocol errors: '+name+' '+str(delta))
  ids=counters(meta['identity_after'])
  if any(ids[k] for k in ('input_queue_drops','graphics_errors','usb_tx_dropped','nmi_count')):raise ValueError('Failed device diagnostics: '+name)
  data=[]
  for workload in WORKLOADS:
   for run in range(3):
    d=json.loads((source/f'{workload}-{run}.json').read_text())
    if d['workload']!=workload or d['run']!=run or len(d['measured'])!=300 or len(d['warmup'])!=30:raise ValueError('Incomplete matrix: '+name)
    if [r['index'] for r in d['measured']]!=list(range(31,331)):raise ValueError('Missing sample: '+name)
    if any(r['input_drops'] or r['accel_errors'] for r in [d['cold'],*d['warmup'],*d['measured']]):raise ValueError('Failed sample: '+name)
    data.append(d)
   samples=[r for d in data if d['workload']==workload for r in d['measured']]
   row={'backend':name,'workload':workload,'measured_samples':len(samples)}
   for key in ('submit_us','gpu_wait_us','pixel_spi_us','total_us','input_gap_ms'):row[key]=stats([r[key] for r in samples])
   row['cold']=[d['cold'] for d in data if d['workload']==workload]
   row['protocol_error_delta']=delta;rows.append(row)
  size=subprocess.check_output([SIZE,'-A',str(build/'app.elf')],text=True)
  sections={k:int(v) for k,v in re.findall(r'^(\.[a-zA-Z0-9_]+)\s+(\d+)\s+\d+',size,re.M)}
  mem={'backend':name,'elf_sha256':sha(build/'app.elf'),'srec_sha256':sha(build/'app.srec'),'sections':sections,'internal_ram_bytes':sum(sections.get(k,0) for k in ('.data','.ramfunc','.bss','.stack')),'sdram_bytes':sum(sections.get(k,0) for k in ('.heap','.graphics_sdram'))}
  memory.append(mem);validated.append((name,source,size))
 mixed=json.loads((Q/'mixed/mixed.json').read_text())
 if not mixed.get('success') or mixed['elapsed_seconds']<600:raise ValueError('Incomplete live endurance run')
 OUT.mkdir(parents=True,exist_ok=True)
 for name,source,size in validated:
  dest=OUT/name;dest.mkdir(exist_ok=True)
  for p in source.iterdir():
   if p.suffix in ('.raw','.json','.png'):shutil.copy2(p,dest/p.name)
  (dest/'memory.txt').write_text(size)
 (OUT/'comparison.json').write_text(json.dumps(rows,indent=2)+'\n')
 (OUT/'memory.json').write_text(json.dumps(memory,indent=2)+'\n')
 lines=['| Renderer | Workload | median ms | p95 ms | max ms | input gap ms | cold ms (3 runs) |','|---|---|---:|---:|---:|---:|---|']
 for r in rows:
  t=r['total_us'];cold=', '.join(f"{c['total_us']/1000:.2f}" for c in r['cold'])
  lines.append(f"| {r['backend']} | {r['workload']} | {t['median']/1000:.2f} | {t['p95']/1000:.2f} | {t['max']/1000:.2f} | {r['input_gap_ms']['max']} | {cold} |")
 (OUT/'comparison.md').write_text('\n'.join(lines)+'\n')
 shutil.copytree(Q/'mixed',OUT/'mixed',dirs_exist_ok=True)
 for name in ['source-manifest.json','backend-guard.log']:shutil.copy2(Q/name,OUT/name)
 for name in ['portable-tests','graphics-tests','fonts','palette']:shutil.copy2(ROOT/'build/host'/f'{name}.log',OUT/(name+'.log'))
 (OUT/'sha256.json').write_text(json.dumps({str(p.relative_to(OUT)):sha(p) for p in sorted(OUT.rglob('*')) if p.is_file() and p.name!='sha256.json'},indent=2)+'\n')
 print('\n'.join(lines))
if __name__=='__main__':main()
