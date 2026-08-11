# Sandboxed Install (v0.2+)

GameCrate **`gamecrate install`** runs an installer under **monitoring** (footprint scan + install report), then sandboxes the game on **Play** via LPAC.

> **v0.4.5+:** Install runs as a normal child process (UAC allowed) so Inno Setup and similar installers work. **Play** still uses AppContainer. See [SCOPE.md](SCOPE.md).

## Command

```powershell
gamecrate install `
  --id my-game `
  --name "My Game" `
  --install-dir "D:\Sandbox\MyGame" `
  --installer "D:\Downloads\setup.exe"
```

Or use the helper:

```powershell
.\tools\Install-GameSandbox.ps1 -Id my-game -Name "My Game" `
  -InstallDir "D:\Sandbox\MyGame" -Installer "D:\Downloads\setup.exe" -Launch
```

## What happens

```mermaid
sequenceDiagram
    participant User
    participant GameCrate
    participant LPAC as Windows LPAC
    participant FS as Filesystem

    User->>GameCrate: install --installer setup.exe
    GameCrate->>FS: Snapshot allowed + watch paths
    GameCrate->>GameCrate: Launch installer (monitored, UAC ok)
    Note over GameCrate,FS: Installer writes; footprint diffed after exit
    GameCrate->>FS: Snapshot again + diff
    GameCrate->>GameCrate: Detect game .exe, tighten ACLs
    GameCrate-->>User: Report + profile ready
```

### 1. Install-phase ACLs

The install directory gets **read/write/execute** so the installer can extract files. Save dir and `%LOCALAPPDATA%\GameCrate\<id>\` are also writable (for GameCrate metadata and virtual AppData).

### 2. Footprint snapshots

**Allowed roots** (writes expected):

- `--install-dir`
- Save directory
- `%LOCALAPPDATA%\GameCrate\<id>\`

**Watch paths** (writes flagged as violations):

- Startup folders
- `%APPDATA%`, `%LOCALAPPDATA%`
- Desktop, Documents
- Start Menu / Programs
- `%ProgramData%` (outside the GameCrate profile subtree)

### 3. Monitored installer launch

The installer runs as a **monitored child process** (not LPAC). Approve **UAC** if prompted. Network is **disabled by default** during install (`--network` to enable).

GameCrate auto-passes `/DIR="<install-dir>"` to Inno-compatible installers when you do not set `--installer-args`.

`--installer-args` is a trusted-caller interface. GameCrate forwards its value as an opaque command-line string; it does not parse or escape individual arguments, though it rejects an unterminated quote. Callers must correctly quote and escape any operator-supplied values, such as a destination path, before interpolating them into this string.

### 4. Post-install analysis

- Lists new/modified files under allowed roots
- Flags any new files under watch paths
- Marks **suspicious** paths (Startup, System32, Tasks, etc.)
- Auto-detects the game `.exe` (skips `setup.exe`, `unins*.exe`, etc.)
- Writes `%LOCALAPPDATA%\GameCrate\<id>\install-report.json`

### 5. ACL tightening

By default, install dir ACLs are tightened to **read/execute** after a successful install so the game can run but not self-modify. Use `--keep-install-writable` to skip this (e.g. for games that patch themselves).

## Options

| Flag | Default | Purpose |
|---|---|---|
| `--installer-args` | empty | Trusted, caller-escaped arguments for the installer |
| `--executable` | auto-detect | Skip executable detection |
| `--allow-outside-writes` | on | Do not fail on benign outside writes (default since v0.4.6) |
| `--strict-outside-writes` | off | Fail on any outside write |
| `--keep-install-writable` | off | Keep install dir writable after install |
| `--network` | off | Allow network during install |
| `--save-dir` | auto | Override save directory |
| `--no-registry` | off | Do not grant `registryRead` |
| `--lpac-com` | off | Grant COM capability |
| `--no-gpu` | off | Disable GPU capabilities |
| `--no-virtual-app-data` | off | Do not redirect AppData |

Full flag reference: [CLI.md](CLI.md).

## Exit codes

| Code | Meaning |
|---|---|
| `0` | Install succeeded, no outside writes, executable detected |
| `1` | Installer failed, outside writes detected, or no executable found |

## Viewing the report

```powershell
gamecrate show-install-report --profile my-game
```

## Malware-focused workflow

> The install report is a footprint report, not a malware scan. Empty
> `outsideWrites` and `outsideRegistryChanges` do not prove that an installer is
> safe. See [MALWARE_SCANNING.md](MALWARE_SCANNING.md).

1. **Never** install untrusted games outside GameCrate first
2. Run `gamecrate install` with network **off**
3. Review `install-report.json` — `outsideWrites` should be empty
4. If clean, enable `--network` on the profile later if the game needs online play:
   ```powershell
   # Edit profile JSON: "network": true, then:
   gamecrate grant --profile my-game
   ```
5. Launch: `gamecrate launch --profile my-game`

## Limitations (v0.3)

- Registry writes are **detected**, not fully redirected — review `outsideRegistryChanges` in install reports
- Watch-path filesystem scans can take time on large `%LOCALAPPDATA%` trees
- Installers that require admin elevation outside the sandbox may fail
- Some installers spawn external processes that are not sandboxed

See [VIRTUAL_STORAGE.md](VIRTUAL_STORAGE.md) for AppData layout and registry scanning details.
