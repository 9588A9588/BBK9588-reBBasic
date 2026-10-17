$ErrorActionPreference = 'Stop'
& python (Join-Path $PSScriptRoot 'run_all.py')
if ($LASTEXITCODE -ne 0) { throw 'host regression tests failed' }
