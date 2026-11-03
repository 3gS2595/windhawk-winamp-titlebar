# Builds a mod and installs it into Windhawk the way Windhawk's own "Compile"
# button does, on this machine or a fresh one:
#   0. installs Windhawk with winget if it's missing (it provides the compiler)
#   1. DLLs -> %ProgramData%\Windhawk\Engine\Mods\{32,64}\<id>_<version>_<random>.dll
#      (plus the libc++/libunwind runtime Windhawk's UI normally puts there)
#   2. source -> %ProgramData%\Windhawk\ModsSource\<id>.wh.cpp
#   3. registry HKLM\SOFTWARE\Windhawk\Engine\Mods\<id>: default settings for
#      any not set yet (existing ones are kept), Version, Include, Exclude,
#      then LibraryFileName, which makes running processes load the new build.
# -Remove uninstalls the mod declared in -Source instead.
# Windhawk's folders and HKLM key need admin, so this asks for UAC once.
param(
    [string]$Source = (Join-Path $PSScriptRoot 'winamp-classic-titlebar.wh.cpp'),
    [switch]$Remove,
    [switch]$NoBuild,
    [switch]$Elevated
)
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'tools\windhawk.ps1')
$Source = (Resolve-Path $Source).Path
$log = Join-Path $root 'build\deploy.log'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $Elevated) {
    if (-not $Remove -and -not (Get-WindhawkDir)) {
        Write-Host "Windhawk isn't installed - installing it with winget..."
        winget install --id RamenSoftware.Windhawk -e --accept-source-agreements --accept-package-agreements
        if (-not (Get-WindhawkDir)) {
            Write-Host "Windhawk didn't install. Get it from https://windhawk.net, then run this again."; exit 1
        }
    }
    if (-not $NoBuild -and -not $Remove) {
        & (Join-Path $root 'build.ps1') -Source $Source
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    if (-not $admin) {
        New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
        Remove-Item $log -ErrorAction SilentlyContinue
        $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
                     '-Source', "`"$Source`"", '-Elevated')
        if ($Remove) { $argList += '-Remove' }
        try {
            $p = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -WindowStyle Hidden -ArgumentList $argList
        } catch {
            Write-Host "Cancelled (UAC prompt declined)."; exit 1
        }
        if (Test-Path $log) { Get-Content $log }
        exit $p.ExitCode
    }
}

function Log($msg) {
    New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
    "[$(Get-Date -Format HH:mm:ss)] $msg" | Add-Content $log
    Write-Host $msg
}

try {
    $wh = Get-WindhawkDir
    if (-not $wh) { throw "Windhawk isn't installed" }
    $dataPath = Get-WindhawkIniValue $wh 'AppDataPath'

    $text = [IO.File]::ReadAllText($Source)
    $modId = 'local@' + @(Get-ModMeta $text 'id')[0]
    $version = @(Get-ModMeta $text 'version')[0]
    $key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$modId"

    if ($Remove) {
        # Deleting the key is what unloads it from running processes.
        Remove-Item $key -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item "HKLM:\SOFTWARE\Windhawk\Engine\ModsWritable\$modId" -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item (Join-Path $dataPath "ModsSource\$modId.wh.cpp") -ErrorAction SilentlyContinue
        Log "Removed $modId"
    } else {
        $include = (Get-ModMeta $text 'include') -join '|'
        $exclude = (Get-ModMeta $text 'exclude') -join '|'
        $dll = "{0}_{1}_{2}.dll" -f $modId, $version, (Get-Random -Minimum 100000 -Maximum 1000000)

        foreach ($t in @(@{ Arch = '32'; Target = 'i686-w64-mingw32' }, @{ Arch = '64'; Target = 'x86_64-w64-mingw32' })) {
            $modsDir = Join-Path $dataPath "Engine\Mods\$($t.Arch)"
            New-Item -ItemType Directory -Force $modsDir | Out-Null
            # The C++ runtime compiled mods link against. Windhawk's UI copies
            # these on first use; a machine that never compiled a mod lacks them.
            foreach ($lib in @(@('libc++.dll', 'libc++.whl'), @('libunwind.dll', 'libunwind.whl'),
                               @('windhawk-mod-shim.dll', 'windhawk-mod-shim.dll'))) {
                $to = Join-Path $modsDir $lib[1]
                if (-not (Test-Path $to)) {
                    Copy-Item (Join-Path $wh "Compiler\$($t.Target)\bin\$($lib[0])") $to
                    Log "Added runtime $($t.Arch)\$($lib[1])"
                }
            }
            $built = Join-Path $root "build\$($t.Arch)\$modId.dll"
            if (-not (Test-Path $built)) { throw "Missing $built - run build.ps1 first" }
            Copy-Item $built (Join-Path $modsDir $dll)
        }
        New-Item -ItemType Directory -Force (Join-Path $dataPath 'ModsSource') | Out-Null
        Copy-Item $Source (Join-Path $dataPath "ModsSource\$modId.wh.cpp")

        if (-not (Test-Path $key)) {
            New-Item $key -Force | Out-Null
            Set-ItemProperty $key Disabled 0 -Type DWord
            Set-ItemProperty $key Architecture ''
        }

        $added = Add-MissingModSettings "$key\Settings" $text
        if ($added.Count) { Log "Default settings added: $($added -join ', ')" }

        Set-ItemProperty $key Version $version
        Set-ItemProperty $key Include $include
        Set-ItemProperty $key Exclude $exclude
        Set-ItemProperty $key LibraryFileName $dll   # last: this is what triggers the reload
        Log "Installed $modId $version as $dll (include '$include', exclude '$exclude')"

        if (-not (Get-Process windhawk -ErrorAction SilentlyContinue)) {
            Start-Process (Join-Path $wh 'windhawk.exe') -ArgumentList '-tray-only'
            Log "Started Windhawk"
        }
    }

    # Delete older builds. Ones still loaded somewhere are locked and stay;
    # Windhawk clears those up later.
    foreach ($arch in '32', '64') {
        $modsDir = Join-Path $dataPath "Engine\Mods\$arch"
        if (-not (Test-Path $modsDir)) { continue }
        Get-ChildItem $modsDir -Filter "${modId}_*.dll" |
            Where-Object { $Remove -or $_.Name -ne $dll } |
            ForEach-Object { try { Remove-Item $_.FullName -ErrorAction Stop; Log "Deleted $arch\$($_.Name)" } catch { } }
    }
    exit 0
} catch {
    Log "ERROR: $_"
    exit 1
}
