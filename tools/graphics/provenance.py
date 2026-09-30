"""Record exact qualified source revisions and compiler/build identifiers."""
import hashlib,io,json,re,subprocess,tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];Q=ROOT/'build/qualification'
def sha(b):return hashlib.sha256(b).hexdigest()
def archive(rev):
 with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(['git','archive',rev],cwd=ROOT))) as t:
  return {m.name:t.extractfile(m).read() for m in t.getmembers() if m.isfile()}
def main():
 m=json.loads((Q/'source-manifest.json').read_text());cold=subprocess.check_output(['git','rev-parse','e6c0c97'],cwd=ROOT,text=True).strip();ra=subprocess.check_output(['git','rev-parse','87f5ee3'],cwd=ROOT,text=True).strip()
 revs={'baseline_ui':'a065a80','baseline_instrumentation':'454ffd8','ra':ra,'dave':'454ffd8','production':cold,'ra_cold':cold,'dave_cold':cold,'baseline_cold_instrumentation':cold}
 m['qualified_source_revisions']=revs;m['tooling_commit']=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip();m.pop('graphics_commit',None)
 m['revision_note']='Warm GPU builds share UI, fonts and compositor. RA additionally corrects native diagonal tie rasterization; five physical scene families match D/AVE exactly. Cold and production builds add the optional benchmark-only genuinely cold paging fixture. Baseline UI/fonts remain a065a80.'
 m['qualified_sources']={}
 for name in ['ra','dave','production']:
  m['qualified_sources'][name]={p:sha(b) for p,b in archive(revs[name]).items() if p.startswith(('src/','cmake/','third_party/','tools/font/')) or p=='CMakeLists.txt'}
 for name,folder in [('baseline','/private/tmp/electra-mini-legacy'),('baseline_cold','/private/tmp/electra-mini-legacy-cold')]:
  base=Path(folder);m['qualified_sources'][name]={str(p.relative_to(base)):sha(p.read_bytes()) for p in sorted(base.rglob('*')) if p.is_file() and (str(p.relative_to(base)).startswith(('src/','cmake/')) or p.name=='CMakeLists.txt')}
 builds={'ra':ROOT/'build/ra-bench','dave':ROOT/'build/dave','baseline':Path('/private/tmp/electra-mini-legacy/build'),'production':ROOT/'build','ra_cold':ROOT/'build/ra-cold','dave_cold':ROOT/'build/dave-cold','baseline_cold':Path('/private/tmp/electra-mini-legacy-cold/build')}
 m['compile_settings']={}
 for name,build in builds.items():
  block=re.search(r'build CMakeFiles/app.dir/src/main.c.obj:.*?(?=\nbuild |\n#)',(build/'build.ninja').read_text(),re.S).group()
  settings=dict(re.findall(r'^  (DEFINES|FLAGS|INCLUDES) = (.*)$',block,re.M));settings.update({ext+'_sha256':sha((build/('app.'+ext)).read_bytes()) for ext in ['elf','srec']});m['compile_settings'][name]=settings
 assert len({s['FLAGS'] for s in m['compile_settings'].values()})==1
 a=archive('a065a80');current=archive(cold)
 for p,h in m['preserved_baseline_normalized_sha256'].items():
  original='src/app/'+p.split('/')[-1] if p in ['src/boot/font_data.c','src/boot/text.c'] else p
  assert current[p].replace(b'\r\n',b'\n')==a[original].replace(b'\r\n',b'\n'),p
 (Q/'source-manifest.json').write_text(json.dumps(m,indent=2)+'\n')
if __name__=='__main__':main()
