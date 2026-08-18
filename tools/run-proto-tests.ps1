# Build and run the EMP/1 codec tests on this machine.
#
# The codec is deliberately free of I/O, allocation and statics, so the same source that runs on
# the device compiles and runs here. Catching a wire-format regression should not require
# plugging anything in.
#
#   powershell -File tools\run-proto-tests.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$vcvars = Get-ChildItem -Path 'C:\Program Files\Microsoft Visual Studio' -Filter 'vcvars64.bat' `
    -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $vcvars) { throw 'vcvars64.bat not found; cannot build the host tests' }

$out = Join-Path $root 'build\host'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$sources = @(
    (Join-Path $root 'src\proto\diag.c'),
    (Join-Path $root 'src\proto\frame.c'),
    (Join-Path $root 'src\proto\surface.c'),
    (Join-Path $root 'src\app\ui_state.c'),
    (Join-Path $root 'src\app\history.c'),
    (Join-Path $root 'tools\sim\sim_wire.c'),
    (Join-Path $root 'tests\proto_tests.c'),
    (Join-Path $root 'tests\ui_state_tests.c'),
    (Join-Path $root 'tests\desc_tests.c'),
    (Join-Path $root 'tests\host_main.c')
) -join ' '

# /W4 /WX so a warning in the codec fails the build. This code runs on a device with no debugger
# attached; an implicit conversion here is not a style question.
$cmd = "call `"$($vcvars.FullName)`" >nul && cl /nologo /W4 /WX /I`"$root\src\proto`" /I`"$root\src\app`" /I`"$root\tools\sim`" " +
       "/Fe:`"$out\proto_tests.exe`" /Fo:`"$out\\`" $sources"

cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }

& "$out\proto_tests.exe"
if ($LASTEXITCODE -ne 0) { throw "tests failed ($LASTEXITCODE)" }
