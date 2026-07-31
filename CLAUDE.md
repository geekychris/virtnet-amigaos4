# virte1000 — Claude project instructions

Intel e1000 SANA-II network driver for AmigaOS 4.1 PPC under QEMU
sam460ex. Sibling to `amiga_mcp` and `python-amigaos4`.

## What "done" looks like

`virte1000.device` file installed to `SYS:Kickstart/` on the OS4
guest, added to `Kicklayout` and `diskboot.config`, replaces or
coexists with the rtl8139 rebroadcaster the guest currently uses.
`bsdsocket.library` binds it. Standard OS4 network tools
(`ping`, browsing the web, `roadshow` config) work through it.
Iperf-equivalent throughput measurably higher than rtl8139.

## Toolchain

Docker image: **`walkero/amigagccondocker:os4-gcc11`** (arm64 host).
Same one `python-amigaos4/build.sh` uses. Pull once:
```
docker pull walkero/amigagccondocker:os4-gcc11-arm64
```

Cross-compile flags (from `python-amigaos4/build.sh` — proven):
```
-mcrt=newlib -mhard-float -O2 -mcpu=440 -Wall
-D__PPC__ -D__USE_INLINE__ -D__USE_OLD_TIMEVAL__
```

For a `.device`, we want a **resident-tag executable** — usually
built with `-nostartfiles -Wl,-Ttext=0x0` and a hand-written
resident tag in the entry point. Study `VirtualSCSIDevice`'s
`Makefile` + `device.c` for the exact link line.

## SDK

`/Users/chris/code/claude_world/refs/os4-sdk/base/` — see the
`os4-sdk-location` memory. The parts you need:
- `Include/devices/sana2.h` — SANA-II command constants +
  IOSana2Req struct
- `Include/interfaces/pci.h`, `Include/proto/pci.h` — PCI bus
  enumeration
- `Include/exec/*` — resident tag, library base, port/msg
- `Documentation/AutoDocs/*.doc` — SANA-II reference (search for
  "sana-II" in doc filenames)

## Testing loop

Use the `amiga_mcp` devbench MCP tools (talks to the amiga-bridge
daemon over TCP through QEMU host-forwarded port 2347):

- `amiga_push_file(local, DH1:...)` — copy build artifact into
  the guest
- `amiga_dos_command("...")` — run AmigaDOS commands
- `amiga_screenshot()` — visual check of guest state
- `amiga_last_crash` — GrimReaper capture on driver crash

Full driver test cycle:
1. `docker run ... make` in this dir → `virte1000.device`
2. `amiga_push_file` → `DH1:virte1000.device`
3. `amiga_dos_command("copy DH1:virte1000.device SYS:Kickstart/")`
4. `amiga_dos_command("reboot")` (a bad driver may need
   `--install` boot instead — see amiga_mcp docs)
5. Wait for boot; `amiga_dos_command("mount NET:")` or whatever
   the current network mount is
6. Test with `amiga_dos_command("ping 10.0.2.2")`, etc.

For **crash iteration** — while developing, don't put it in
Kickstart! Load it as a normal library via
`OpenDevice("virte1000.device", 0, req, 0)` from a small test
program. That way a crash just kills the test, not the guest.

## QEMU config

The guest's NIC is set in
`amiga_mcp/scripts/start-qemu-os4.sh`. Currently:
```
-netdev user,id=n0,hostfwd=tcp::2347-:2345
-device rtl8139,netdev=n0
```

To switch to e1000 for testing:
```
-device e1000-82540em,netdev=n0
```
or keep both by using a second netdev pair (dual-NIC guest).

**Important:** don't remove rtl8139 during development — that's
how amiga-bridge talks back to devbench. Add e1000 as a second
device.

## Related project memory

Read these before writing code (via the Read tool on the paths):
- `~/.claude/projects/-Users-chris-code-claude-world-amiga-mcp/memory/MEMORY.md`
  — index of amiga_mcp memories, especially:
  - `python_os4_port.md` — how the OS4 target is set up
  - `os4_sdk_location.md` — SDK paths
  - `local_minio_qemu.md` — QEMU networking notes
  - `amigados_shell_gotchas.md` — shell quirks that bite you

## Gotchas already known

- **AmigaDOS `;` is a comment**, not a statement separator. Never
  use it in `dos_command()` invocations.
- **`<file`** as stdin redirect **does not work** in AmigaDOS for
  many tools. Use `type file | prog` instead.
- **The Amiga clock is wall-local, not UTC.** OS4's newlib
  `time.gmtime()` interprets it as local and adds the TZ offset.
  Set the clock with `date DD-MMM-YY HH:MM:SS` where the value is
  `real_UTC - TZ_offset`. Only matters for time-signed protocols
  (SigV4/JWT/OAuth).
- **PPC is big-endian**; e1000 registers and descriptors are
  **little-endian**. Wrap all descriptor accesses with byte-swap
  primitives. Look for `stwbrx`/`lwbrx` inline asm patterns in
  VirtualSCSIDevice.

## What NOT to touch

- `amiga_mcp/` — this is the iteration harness. Only touch it
  when you need to add MCP tool support you're missing (rare).
- `python-amigaos4/` — completely orthogonal. Don't drag it in.
- Guest-side `SYS:Kickstart/` files — that's how OS4 boots.
  Test drivers as loadable `.device`s from `DH1:` first; only
  install to Kickstart when stable.
