"""Build the preserved renderer with the same current benchmark/service instrumentation."""
import io, pathlib, subprocess, tarfile, sys
root=pathlib.Path(__file__).resolve().parents[2]
dest=pathlib.Path(sys.argv[1] if len(sys.argv)>1 else '/private/tmp/electra-mini-legacy')
dest.mkdir(parents=True,exist_ok=True)
with tarfile.open(fileobj=io.BytesIO(subprocess.check_output(['git','archive','a065a80'],cwd=root))) as t:t.extractall(dest,filter='data')
for name in ['CMakeLists.txt','src/main.c','src/hal/ra8876.c','src/hal/spi.c','src/proto/diag.h','src/startup/sections.ld','src/app/graphics.h','src/app/graphics_backend.h','src/boot/text.c','src/boot/font_data.c']:
    p=dest/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes((root/name).read_bytes())
(dest/'src/app/graphics.c').write_text('#include "graphics.h"\nuint64_t gfx_gpu_cycles,gfx_spi_cycles;\nuint32_t gfx_accel_errors;\nvoid gfx_cache_reset(void){}\n')
subprocess.run(['cmake','-S',str(dest),'-B',str(dest/'build'),'-G','Ninja','-DCMAKE_TOOLCHAIN_FILE='+str(dest/'cmake/arm-none-eabi.cmake'),'-DGRAPHICS_BENCHMARK=ON'],check=True)
subprocess.run(['cmake','--build',str(dest/'build')],check=True)
