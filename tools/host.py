"""Portable host tests and simulator: python3 tools/host.py test|sim [scene]."""
from pathlib import Path
import subprocess
import sys
r = Path(__file__).resolve().parents[1]
out = r / 'build' / 'host'; out.mkdir(parents=True, exist_ok=True)
mode = sys.argv[1] if len(sys.argv) > 1 else 'test'
if mode not in ('test','sim','graphics'): raise SystemExit('expected test, sim or graphics')
common = ['src/proto/diag.c', 'src/proto/frame.c', 'src/proto/surface.c',
          'src/app/ui_state.c', 'src/app/history.c', 'tools/sim/sim_wire.c']
if mode == 'test':
    sources = common + ['src/proto/txq.c', 'src/app/touch_cal.c'] + ['tests/'+x+'.c' for x in
        ['proto_tests','ui_state_tests','touch_cal_tests','desc_tests','txq_tests','host_main']]
else:
    sources = common + [('tests/graphics_tests.c' if mode == 'graphics' else 'tools/sim/sim_main.c'), 'src/app/text.c', 'src/app/font_data.c', 'src/app/ui.c', 'src/app/graphics.c', 'tools/sim/sim_hal.c']
exe = out / mode
subprocess.run(['cc', '-std=c11', '-fsanitize=address,undefined', '-g', '-Wall', '-Wextra', '-Werror=implicit-function-declaration',
                '-Wno-unused-parameter', '-Wno-missing-field-initializers'] +
               ['-I'+str(r/p) for p in ['src/proto','src/app','src/hal','src/common','src/bsp','tools/sim']] +
               [str(r/p) for p in sources] + ['-lm','-o',str(exe)], check=True)
if mode in ('test','graphics'): subprocess.run([str(exe)], check=True)
else:
    ppm = out/'sim.ppm'
    subprocess.run([str(exe), sys.argv[2] if len(sys.argv)>2 else 'ui', str(ppm)], check=True)
    from PIL import Image
    Image.open(ppm).save(out/'sim.png')
    print(out/'sim.png')
