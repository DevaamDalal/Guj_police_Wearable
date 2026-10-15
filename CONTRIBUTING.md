# Contributing — Spandan Wearable

## Team & Roles

| Role | Who | Responsibilities |
|------|-----|------------------|
| **User / Hardware Tester** | Devaam Dalal ([@DevaamDalal](https://github.com/DevaamDalal)) | Flashes firmware, runs on-device tests, captures serial/SD logs, relays between AI agents |
| **Project Architect & Git Manager** | Antigravity (AI — Google DeepMind) | Maintains the repo, commits/pushes code, tracks roadmap, writes context documents, gives instructions to the firmware coder |
| **Firmware Coder** | Claude (AI — Anthropic) | Writes and modifies all ESP32 firmware (.ino, .c, .cpp, .h), delivers complete files + commit message + status report |

## Workflow

```
Antigravity  ──(task + context)──►  Devaam  ──(paste)──►  Claude
Claude       ──(code + commit msg + status report)──►  Devaam  ──(files)──►  Antigravity
Antigravity  ──(git add, commit, tag, push)──►  GitHub
```

1. **Antigravity** decides the next task from the roadmap, writes a prompt with full context
2. **Devaam** pastes it into a new Claude chat session
3. **Claude** writes the code, tests nothing (no hardware), delivers a zip with:
   - Modified source files (complete, not diffs)
   - `COMMIT_MESSAGE.txt`
   - `STATUS_REPORT_FOR_ANTIGRAVITY.md`
4. **Devaam** downloads Claude's zip, brings it back to Antigravity
5. **Antigravity** extracts, replaces files in the repo, verifies file list, commits, tags, pushes
6. **Devaam** pulls, flashes, tests on device, reports results

## Git Rules (Strict)

- **Never** use `--force` or rewrite history
- If push fails (permissions, conflicts), stop and report the exact error
- If branch is behind remote, `git pull --rebase`, report what changed, then push
- One commit per stage/task — no squashing after push
- Tags follow the pattern: `stage{N}-{label}` (e.g., `stage4c-local`, `stage6-v0-ble`)
- Commit messages: first line is a summary, body has bullet points of what changed

## Branch Strategy

- `main` — single branch, linear history
- No feature branches (project is small, one task at a time)
- Tags mark each milestone

## Code Conventions

- Arduino sketch folder name must match `.ino` filename: `Guj_police_Wearable/Guj_police_Wearable.ino`
- BSP files use `_bsp` suffix (e.g., `lcd_bsp.c`, `touch_bsp.cpp`)
- Generated font files (`ui_font_*.c`) — do not hand-edit
- All `#define` constants at the top of `.ino` — no magic numbers in function bodies
- FreeRTOS mutexes documented in `docs/ARCHITECTURE.md`
