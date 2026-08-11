# Threat Model

## Assets to protect

1. User files outside the game (Documents, Desktop, projects, credentials)
2. Other installed games and their save data
3. System integrity (registry, services, startup entries)
4. Network identity and adjacent machines on the LAN

## Trust boundaries

```
[ Host Windows ] ── trust boundary ── [ GameCrate LPAC ] ── [ Game process tree ]
```

The game and all its child processes are **untrusted**. The launcher and profile definitions are **trusted** (run with user consent, ideally code-signed in production).

## Attacker capabilities assumed

- Arbitrary code execution inside the sandboxed game process
- Ability to invoke any Win32 API allowed by the token and ACLs
- Social engineering to convince the user to widen profile grants

## Controls

| Control | Mechanism |
|---|---|
| Default deny filesystem | LPAC + no ACE on user paths |
| Explicit allow list | Per-profile ACL grants on install/save dirs |
| Network opt-in | `internetClient` not granted unless profile enables it |
| Registry opt-in | `registryRead` required for LPAC registry access |
| Process isolation | AppContainer object namespace — cannot open handles to non-container processes |
| Identity separation | Per-game AppContainer SID — cannot impersonate user for other apps' resources |

## Residual risks

### Anti-cheat and DRM

Kernel anti-cheat installs drivers that expect full system access. These will fail or refuse to run inside GameCrate. This is intentional — you cannot simultaneously sandbox a game and grant anti-cheat kernel visibility.

### Shared library probing

Games can read world-readable system files (`C:\Windows\System32\*.dll`). This is required for execution. Sensitive data should not live in world-readable paths.

### Capability sprawl

Over-granting capabilities (`internetClient` + broad `readablePaths`) weakens isolation. Profiles should use minimal grants and be reviewed.

### Installer supply chain

The install report is footprint monitoring, not malware detection; an empty
report does not establish that an installer is safe. Installers that elevate or
spawn external processes may also escape the monitored process tree. Only
install from trusted sources, and evaluate unknown software in a disposable
Windows VM or Windows Sandbox. After install, tighten write access if the game
does not self-update. See [MALWARE_SCANNING.md](MALWARE_SCANNING.md).

### Sandbox escapes

AppContainer escapes are rare but historically possible (patch Windows promptly). A kernel minifilter (v1.0 roadmap) adds defense in depth.

## Security recommendations for operators

1. Create one profile per game — never share profiles across titles.
2. Keep `network: false` for offline single-player games.
3. Point saves to `%LOCALAPPDATA%\GameCrate\<profile>\saves`, not `Documents`.
4. Review `readablePaths` — never add `C:\Users` or drive roots.
5. Run GameCrate launcher from a standard user account, not Administrator.
