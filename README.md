# Winamp Classic Title Bar (Windhawk mod)

Replaces standard window title bars with a thin Winamp/MilkDrop-style caption:
centered uppercase title, engraved grooves, beveled gradient, and small
minimize / maximize / close buttons that the mod runs itself.

## Install on a new PC

1. Get this folder onto the PC (clone or copy it).
2. Double-click **`install.cmd`** and approve the UAC prompt.

That's all. If Windhawk is missing it is installed first (via winget), the mod
is compiled with Windhawk's bundled compiler, installed, and given its default
settings (18 px bar, the excluded-app list). No Windhawk UI steps are needed.

## After editing the mod

Run `install.cmd` again (or `.\deploy.ps1`). Running apps switch to the new
build on the spot; your settings are kept, and only settings that are new in
the source get their defaults.

## Files

| File | What it is |
| --- | --- |
| `winamp-classic-titlebar.wh.cpp` | The mod. Settings and their defaults are in its `WindhawkModSettings` block. |
| `install.cmd` | Double-click wrapper for `deploy.ps1` (works on PCs that block PowerShell scripts). |
| `deploy.ps1` | Builds and installs; `-Remove` uninstalls; `-NoBuild` installs the last build. |
| `build.ps1` | Compiles into `build\32` and `build\64` exactly like Windhawk's editor (no admin). |
| `tools\windhawk.ps1` | Shared helpers: finding Windhawk, reading mod metadata and default settings. |
| `tools\unload-guard.wh.cpp` | Only for upgrading a PC still running a build older than 0.11 (see below). |

## Upgrading from a build older than 0.11

Builds before 0.11 crash apps when Windhawk unloads them (they subclassed
invisible message-only windows and could leave comctl32 unloaded under live
windows). 0.11+ unloads safely. To replace a pre-0.11 build without crashes,
install `tools\unload-guard.wh.cpp` first (`.\deploy.ps1 -Source tools\unload-guard.wh.cpp`),
then deploy the mod, then remove the guard (`... -Remove`). A PC that never had
the mod installed doesn't need any of this.
