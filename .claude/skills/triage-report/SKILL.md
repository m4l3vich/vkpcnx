---
name: triage-report
description: Investigate and fix a bug from a vkpcnx debug report (vkpcnx-report-*.zip, a GitHub issue with one attached, or a single vkpcnx-*.log). Use when the user hands over a report, an issue number with a report, or a crash log from the app.
---

# Triage a vkpcnx debug report

Reports come from the app's "Создать отчёт об ошибке" button (feedback dialog,
login screen `Y`, the offer after a crash) or `vkpcnx --create-report`. The
format is described in `vkpcnx/include/core/diag/report.hpp`; logs are written
by `vkpcnx/src/core/diag/log.cpp`.

## 1. Get the report

- A GitHub issue: `gh issue view N -R m4l3vich/vkpcnx --comments` and download
  the attached `.zip` (the link in the body) with `curl -L -o report.zip URL`.
- A local path: use it as is.

## 2. Summarize and symbolize

```bash
scripts/triage-report.py report.zip --fetch --extract /tmp/report
```

`--fetch` downloads `vkpcnx-<platform>-symbols` for the report's commit with
`gh` (CI artifacts, 90 days; release assets for `v*` tags, permanent) into
`~/.cache/vkpcnx-symbols/`. Without network, build the same commit and pass
`--symbols <build dir>` (needs the exact commit: the `git:` line; a `-dirty`
build can't be reproduced exactly, so treat its line numbers as approximate).
Switch frames need `aarch64-none-elf-addr2line`; the script falls back to the
`devkitpro/devkita64` Docker image.

## 3. Read it

- `report.txt`: version, commit, platform, firmware / Atmosphère / launch mode
  (Switch applet mode has much less memory), GPU, whether the previous run
  crashed.
- Each log starts with a header (same facts) then lines
  `HH:MM:SS.mmm L thread message`; L is E/W/I/D/V. Subsystems prefix their
  messages (`Manager:`, `GameServer:`, `VideoAndAudioStream:`, `rtc:`,
  `VideoDecoder:`, `InputChannel:`, `Http:`, `Watchdog:`, `Stats:`).
- `*** CRASH` sections: cause, `build:` line, `bt #N addr module+offset`. On
  Switch the section also has the fault's memory regions and registers, and
  `crash_reports/` may hold the matching Atmosphère report (the script
  resolves its addresses inside the vkpcnx module).
- `Stats:` lines every 2 s while streaming: render/decode fps, Mbit/s, RTT,
  RTP loss, decode errors, keyframe requests. Look at the minute before a
  reported stutter or freeze.
- `Watchdog: main loop blocked` = UI thread stall (deadlock or blocking call
  on the UI thread), unless the app was minimized / in the HOME menu.
- No crash section and no `Log: clean exit`: hang, kill or power loss; read the
  last lines.

Secrets and public IPs are already masked (`***`, `a.b.c.x`) — never ask the
user for the unredacted values; work from the masked ones.

## 4. Fix

Check the user's memory notes for Switch pitfalls already solved before
proposing a cause. Reproduce on desktop when possible (`tests/mock_server.py`
+ `VKPCNX_PLAY_URL`, see README); for Switch-only bugs, build with
`./scripts/build-switch.sh --docker --debug` and ask the user to run it with
`nxlink`. `vkpcnx --test-crash=segv|abort|throw` exercises the crash path
itself.

If the report lacks something needed to pin a class of bugs, add the logging
(at DEBUG unless it is rare) so the next report has it.
