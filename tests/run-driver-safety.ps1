$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$build = Join-Path $repo 'work/driver-safety'
New-Item -ItemType Directory -Force $build | Out-Null
$compiler = (Get-Command clang++ -ErrorAction Stop).Source
$exe = Join-Path $build 'driver_safety_test.exe'
& $compiler -std=c++20 -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN (Join-Path $PSScriptRoot 'driver_safety_test.cpp') ('-I' + (Join-Path $repo 'thirdparty/dantto4k/src')) -lws2_32 -luser32 -o $exe
if ($LASTEXITCODE -ne 0) { throw 'Regression test compilation failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Regression tests failed' }
