# Compiles the mod exactly like Windhawk's editor does (same clang, same flags),
# into .\build\32 and .\build\64. Needs no admin; deploy.ps1 does the install.
param([string]$Source = (Join-Path $PSScriptRoot 'winamp-classic-titlebar.wh.cpp'))
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'tools\windhawk.ps1')

$wh = Get-WindhawkDir
if (-not $wh) { Write-Host "Windhawk isn't installed (it provides the compiler). Run install.cmd, or install it from https://windhawk.net"; exit 1 }
$engine = Join-Path $wh (Get-WindhawkIniValue $wh 'EnginePath')
$compiler = Join-Path $wh 'Compiler'
$clang = Join-Path $compiler 'bin\clang++.exe'

$text = [IO.File]::ReadAllText($Source)
$modId = 'local@' + @(Get-ModMeta $text 'id')[0]
$version = @(Get-ModMeta $text 'version')[0]
$extra = @((@(Get-ModMeta $text 'compilerOptions')[0]) -split '\s+' | Where-Object { $_ })

# Args go through a response file: it keeps the L"..." macro quoting intact no
# matter which PowerShell version is passing arguments to clang.
function Q($s) { '"' + ($s -replace '\\', '/' -replace '"', '\"') + '"' }

$failed = $false
foreach ($t in @(@{ Arch = '32'; Target = 'i686-w64-mingw32' }, @{ Arch = '64'; Target = 'x86_64-w64-mingw32' })) {
    $outDir = Join-Path $PSScriptRoot "build\$($t.Arch)"
    New-Item -ItemType Directory -Force $outDir | Out-Null
    $out = Join-Path $outDir "$modId.dll"
    $rsp = Join-Path $outDir 'clang.rsp'
    $clangArgs = @(
        '-std=c++23', '-O2', '-shared', '-DUNICODE', '-D_UNICODE',
        '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', '-D_WIN32_IE=0x0A00', '-DNTDDI_VERSION=0x0A000008',
        '-D__USE_MINGW_ANSI_STDIO=0', '-DWH_MOD',
        (Q "-DWH_MOD_ID=L`"$modId`""), (Q "-DWH_MOD_VERSION=L`"$version`""),
        (Q (Join-Path $engine "$($t.Arch)\windhawk.lib")),
        '-x', 'c++', (Q $Source),
        '-include', 'windhawk_api.h', '-target', $t.Target,
        '-Wl,--export-all-symbols', '-o', (Q $out)
    ) + $extra
    [IO.File]::WriteAllText($rsp, ($clangArgs -join "`n"))

    Write-Host "[$($t.Arch)-bit] $modId $version"
    Push-Location $compiler
    try { & $clang "@$rsp" } finally { Pop-Location }
    if ($LASTEXITCODE -ne 0) { Write-Host "[$($t.Arch)-bit] FAILED (exit $LASTEXITCODE)"; $failed = $true }
}
if ($failed) { exit 1 }
Write-Host "Built: $(Join-Path $PSScriptRoot 'build')"
