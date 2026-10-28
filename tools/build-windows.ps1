$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ToolsRoot = Join-Path $ProjectRoot '.tools'
New-Item -ItemType Directory -Force -Path $ToolsRoot | Out-Null
function Get-VerifiedArchive([string]$Url, [string]$File, [string]$Hash) {
    if (-not (Test-Path -LiteralPath $File)) {
        Invoke-WebRequest -Uri $Url -OutFile $File
    }
    if ((Get-FileHash -LiteralPath $File -Algorithm SHA256).Hash -ne $Hash) {
        throw "Archive checksum mismatch: $File"
    }
}
$CompilerArchive = Join-Path $ToolsRoot 'w64devkit.exe'
Get-VerifiedArchive 'https://github.com/skeeto/w64devkit/releases/download/v2.10.1/w64devkit-x64-2.10.1.7z.exe' $CompilerArchive 'ADD1EDF984CE4D473CFEB7C52E0AB38DB73A855BBA6C7F564D5E16648EB2896A'
$Compiler = Join-Path $ToolsRoot 'w64devkit\bin\g++.exe'
if (-not (Test-Path -LiteralPath $Compiler)) {
    & $CompilerArchive -y "-o$ToolsRoot"
    if ($LASTEXITCODE -ne 0) { throw 'Compiler extraction failed' }
}
$GtestArchive = Join-Path $ToolsRoot 'gtest.zip'
$BenchmarkArchive = Join-Path $ToolsRoot 'benchmark.zip'
Get-VerifiedArchive 'https://github.com/google/googletest/archive/refs/tags/v1.16.0.zip' $GtestArchive 'A9607C9215866BD425A725610C5E0F739EEB50887A57903DF48891446CE6FB3C'
Get-VerifiedArchive 'https://github.com/google/benchmark/archive/refs/tags/v1.9.2.zip' $BenchmarkArchive 'A13CDCD3B0E3725E371F977A2855FECD651FC0236C67DA16E325B726057B15B4'
Expand-Archive -LiteralPath $GtestArchive -DestinationPath $ToolsRoot -Force
Expand-Archive -LiteralPath $BenchmarkArchive -DestinationPath $ToolsRoot -Force
$Python = Join-Path $ProjectRoot '.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $Python)) {
    python -m venv (Join-Path $ProjectRoot '.venv')
    if ($LASTEXITCODE -ne 0) { throw 'Python environment creation failed' }
}
& $Python -m pip install -r (Join-Path $ProjectRoot 'requirements-lock.txt')
if ($LASTEXITCODE -ne 0) { throw 'Python dependency installation failed' }
$env:PATH = (Join-Path $ToolsRoot 'w64devkit\bin') + ';' + (Join-Path $ProjectRoot '.venv\Scripts') + ';' + $env:PATH
$Build = Join-Path $ProjectRoot 'build\release'
& $Python -m cmake -S $ProjectRoot -B $Build -G Ninja '-DCMAKE_BUILD_TYPE=Release' "-DCMAKE_CXX_COMPILER=$Compiler" "-DPython_EXECUTABLE=$Python" "-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=$(Join-Path $ToolsRoot 'googletest-1.16.0')" "-DFETCHCONTENT_SOURCE_DIR_GOOGLEBENCHMARK=$(Join-Path $ToolsRoot 'benchmark-1.9.2')"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& $Python -m cmake --build $Build -j 4
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }
& (Join-Path $ProjectRoot '.venv\Scripts\ctest.exe') --test-dir $Build --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'C++ tests failed' }
$env:PYTHONPATH = Join-Path $ProjectRoot 'python'
& $Python (Join-Path $ProjectRoot 'tools\run_checks.py') --build $Build
if ($LASTEXITCODE -ne 0) { throw 'Workflow checks failed' }
