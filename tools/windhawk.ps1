# Shared helpers for build.ps1 / deploy.ps1 (dot-sourced).

# Windhawk's install folder, or $null if it isn't installed.
function Get-WindhawkDir {
    $uninstall = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Windhawk'
    $dir = (Get-ItemProperty $uninstall -ErrorAction SilentlyContinue).InstallLocation
    if (-not $dir) { $dir = Join-Path $env:ProgramFiles 'Windhawk' }
    if (Test-Path (Join-Path $dir 'windhawk.ini')) { return $dir }
    return $null
}

# A value from windhawk.ini's [Storage] section (EnginePath, AppDataPath, ...).
function Get-WindhawkIniValue([string]$whDir, [string]$name) {
    $line = Get-Content (Join-Path $whDir 'windhawk.ini') | Where-Object { $_ -match "^$name=" } | Select-Object -First 1
    return [Environment]::ExpandEnvironmentVariables(($line -replace "^$name=", ''))
}

# All values of a "// @name value" metadata line in a mod's source.
function Get-ModMeta([string]$text, [string]$name) {
    @([regex]::Matches($text, "//\s*@$name\s+(.+)") | ForEach-Object { $_.Groups[1].Value.Trim() })
}

function ConvertFrom-SettingValue([string]$v) {
    if ($v -match '^"(.*)"$' -or $v -match "^'(.*)'$") { return [string]$Matches[1] }
    if ($v -match '^-?\d+$') { return [int]$v }
    if ($v -eq 'true') { return 1 }
    if ($v -eq 'false') { return 0 }
    return $v
}

# Default settings from the mod's ==WindhawkModSettings== block, as Windhawk's
# UI would write them on install: name -> int / string / list of strings.
# Handles the flat scalar + string-list form this mod uses.
function Get-ModDefaultSettings([string]$text) {
    $defaults = [ordered]@{}
    $block = [regex]::Match($text, '(?s)// ==WindhawkModSettings==\s*/\*(.*?)\*/')
    if (-not $block.Success) { return $defaults }
    $list = $null
    foreach ($line in ($block.Groups[1].Value -split "`r?`n")) {
        if ($line -match '^- ([A-Za-z0-9_]+):\s*(.*?)\s*$') {
            $key = $Matches[1]; $value = $Matches[2]
            if ($value -eq '') { $list = New-Object System.Collections.ArrayList; $defaults[$key] = $list }
            else { $list = $null; $defaults[$key] = ConvertFrom-SettingValue $value }
        } elseif ($null -ne $list -and $line -match '^\s+- (.+?)\s*$') {
            [void]$list.Add((ConvertFrom-SettingValue $Matches[1]))
        }
    }
    return $defaults
}

# Writes the mod's default for every setting not set yet under $settingsKey
# (a registry path), like Windhawk's UI does on install; existing values are
# kept. A list counts as set if any of its items is. Returns the names added.
function Add-MissingModSettings([string]$settingsKey, [string]$text) {
    if (-not (Test-Path $settingsKey)) { New-Item $settingsKey -Force | Out-Null }
    $have = @((Get-Item $settingsKey).GetValueNames())
    $haveBase = @($have | ForEach-Object { $_ -replace '\[\d+\].*$', '[0]' })
    $added = @()
    $defaults = Get-ModDefaultSettings $text
    foreach ($name in $defaults.Keys) {
        $value = $defaults[$name]
        if ($value -is [System.Collections.IList]) {
            if ($haveBase -contains ('{0}[0]' -f $name)) { continue }
            for ($i = 0; $i -lt $value.Count; $i++) {
                New-ItemProperty $settingsKey -Name ('{0}[{1}]' -f $name, $i) -Value $value[$i] -PropertyType String -Force | Out-Null
            }
        } else {
            if ($have -contains $name) { continue }
            $kind = if ($value -is [int]) { 'DWord' } else { 'String' }
            New-ItemProperty $settingsKey -Name $name -Value $value -PropertyType $kind -Force | Out-Null
        }
        $added += $name
    }
    return ,$added
}
