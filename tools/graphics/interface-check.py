"""Console/framebuffer helper for tools/verify-mini-highlights.mjs in av-frameworks.
Touch injection exercises the real firmware and USB path, not the physical touch sensor.
"""
import argparse,json,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT/'tools/deploy'))
from flash_usb import router_maintenance
from link import open_app
from qualify import command,framebuffer
p=argparse.ArgumentParser();p.add_argument('--out',required=True);p.add_argument('--mode',choices=['capture','touch'],required=True);p.add_argument('--mask',default='0');a=p.parse_args();out=Path(a.out);out.mkdir(parents=True,exist_ok=True)
print('READY',flush=True);scene=sys.stdin.readline().strip();url=router_maintenance(True);dev=None
try:
 dev=open_app()
 if a.mode=='touch':command(dev,'sim t '+a.mask,timeout=10)
 else:framebuffer(dev,out,scene)
 print(json.dumps({'scene':scene,'mode':a.mode,'mask':a.mask}),flush=True)
finally:
 if dev:dev.close()
 if url:router_maintenance(False,url)
