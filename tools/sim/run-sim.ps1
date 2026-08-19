# Build and run the UI simulator on this machine, then open the result.
#
#   powershell -File tools\sim\run-sim.ps1            # the surface UI
#   powershell -File tools\sim\run-sim.ps1 specimen   # the type specimen
#
# This compiles the REAL device code — text.c, font_data.c, ui.c AND src/proto/surface.c — against a
# framebuffer instead of the RA8876. Layout, typography and colour can then be judged in a
# second, with no hardware and no flash cycle. It also separates "the drawing code is wrong"
# from "the device path is wrong", which are entirely different investigations and are
# otherwise indistinguishable from a black screen.

param([string]$Scene = "ui")

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

$vcvars = Get-ChildItem -Path 'C:\Program Files\Microsoft Visual Studio' -Filter 'vcvars64.bat' `
    -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $vcvars) { throw 'vcvars64.bat not found' }

$out = Join-Path $root 'build\sim'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$sources = @(
    "$root\tools\sim\sim_main.c",
    "$root\src\app\text.c",
    "$root\src\app\font_data.c",
    "$root\src\app\ui.c",
    "$root\src\app\ui_state.c",
    "$root\src\app\history.c",
    "$root\src\proto\surface.c",
    "$root\src\proto\diag.c",
    "$root\src\proto\frame.c",
    "$root\tools\sim\sim_wire.c"
) -join ' '

$inc = "/I`"$root\src\app`" /I`"$root\src\hal`" /I`"$root\src\proto`" /I`"$root\src\bsp`""

# /W3 rather than /W4: this is host scaffolding around device code, and MSVC objects to
# perfectly reasonable embedded idioms. The device build is the one held to /Wall -Werror.
$cmd = "call `"$($vcvars.FullName)`" >nul && cl /nologo /W3 /D_CRT_SECURE_NO_WARNINGS $inc " +
       "/Fe:`"$out\sim.exe`" /Fo:`"$out\\`" $sources"

cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

if ($Scene -eq 'choice-check') {
    & C:/Users/user/miniconda3/python.exe "$PSScriptRoot\check-choice.py" `
        "$out\sim.exe" $out
    if ($LASTEXITCODE -ne 0) { throw "Choice pixel checks failed" }
    exit 0
}

$ppm = Join-Path $out "sim.ppm"
& "$out\sim.exe" $Scene $ppm
if ($LASTEXITCODE -ne 0) { throw "sim failed" }

# PPM is trivial to write from C; PNG is what everything else can read.
$png = Join-Path $out "sim.png"
& C:/Users/user/miniconda3/python.exe -c @"
from PIL import Image
im = Image.open(r'$ppm')
im.save(r'$png')
print('wrote', r'$png', im.size)
"@
