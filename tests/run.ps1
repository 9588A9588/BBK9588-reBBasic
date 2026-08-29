$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path ([IO.Path]::GetTempPath()) "re-bbasic-host-test-$PID.exe"
$compiler = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $compiler) {
  $compiler = Get-Command clang -ErrorAction SilentlyContinue
}
if (-not $compiler) {
  throw 'Host C compiler not found; install gcc or clang'
}
try {
  & $compiler.Source -std=c99 -Wall -Wextra -Werror -O2 `
    -I (Join-Path $root 'src') `
    (Join-Path $root 'src\bbvm_core.c') `
    (Join-Path $root 'src\bbvm_runtime.c') `
    (Join-Path $root 'src\bbvm_lib.c') `
    (Join-Path $root 'tests\host_test.c') `
    -o $out
  if ($LASTEXITCODE -ne 0) { throw 'host compiler failed' }
  & $out
  if ($LASTEXITCODE -ne 0) { throw 'host tests failed' }
} finally {
  Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
}
