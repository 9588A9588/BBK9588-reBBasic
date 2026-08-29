$ErrorActionPreference = 'Stop'
$project = $PSScriptRoot
$repo = Split-Path -Parent $project

Push-Location $repo
try {
    & python (Join-Path $project 'tools\build_bda.py') @args
    if ($LASTEXITCODE -ne 0) { throw 'BDA build failed' }
} finally {
    Pop-Location
}
