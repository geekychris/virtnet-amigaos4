# Prompt to start a fresh Claude session on virte1000

Paste **everything below the horizontal rule** into a new Claude
Code session started from `/Users/chris/code/claude_world/virte1000`.
It briefs Claude on the project, points at the sibling projects
whose knowledge should be inherited, and lists concrete first steps.

---

I want you to work on **virte1000** — a new AmigaOS 4.1 PPC
SANA-II network device driver targeting QEMU's Intel e1000
(`-device e1000-82540em`). The current sibling project `amiga_mcp`
uses QEMU's rtl8139 driver; it's slow and occasionally wedges. This
project builds a modern replacement.

**Your working directory is `/Users/chris/code/claude_world/virte1000`.**
Start by reading `README.md`, then `CLAUDE.md`. Both are already
written and describe the goal, the toolchain, and the sibling
projects to inherit knowledge from.

**Sibling projects you can and should read from:**

- `/Users/chris/code/claude_world/amiga_mcp/` — the devbench + MCP
  server that lets you push files and run commands on the OS4
  guest via the amiga-bridge daemon. **You will iterate on this
  driver by pushing built binaries to `DH1:` and testing there
  through the MCP tools that this devbench provides.** Read
  `amiga_mcp/CLAUDE.md` and the memory index at
  `~/.claude/projects/-Users-chris-code-claude-world-amiga-mcp/memory/MEMORY.md`
  before writing driver code. That memory index is loaded in every
  amiga_mcp session and contains a lot of hard-won knowledge about
  the emulator, the AmigaDOS shell, timing gotchas, and the SDK.

- `/Users/chris/code/claude_world/python-amigaos4/` — CPython 3.12
  cross-compiled for OS4 via the same walkero Docker toolchain we
  need. **Its `build.sh` is the template for our Makefile.** It
  also has the SDK reference downloaded and extracted (linked from
  its memory).

- `/Users/chris/code/claude_world/refs/os4-sdk/` — AmigaOS 4.1
  SDK 54.25, already downloaded and extracted. `base/Include/` has
  the C headers you need (`devices/sana2.h`, `interfaces/pci.h`,
  `proto/pci.h`). `base/Documentation/AutoDocs/` has the reference
  docs.

- `https://github.com/derfsss/VirtualSCSIDevice` — reference
  project. Same shape (OS4 device driver targeting a QEMU-emulated
  PCI device), different target (VirtIO-SCSI, not e1000). Their
  build system, PCI enumeration, IRQ hookup, and endian-swap
  patterns all translate directly. Clone into a temp dir and read.

**Concrete first-session goals** (in order):

1. **Understand the emulated e1000.** Read QEMU's `hw/net/e1000.c`
   (grab the source from a `qemu@stable` tag on GitHub) and note
   which registers are actually implemented. QEMU's e1000 is a
   subset of the real chip — this list bounds what your driver
   needs to touch.

2. **Modify the amiga_mcp QEMU launcher** to add an e1000 in
   *addition* to the existing rtl8139. **Don't remove the rtl8139**
   — the devbench uses it to reach the amiga-bridge daemon (via
   TCP hostfwd on port 2347 → guest 2345). Add a second netdev +
   e1000 device; the driver's job is to bind the second one.
   The launcher lives at
   `amiga_mcp/scripts/start-qemu-os4.sh`.

3. **Set up a Docker build.** Steal the pattern from
   `python-amigaos4/build.sh`. Image is
   `walkero/amigagccondocker:os4-gcc11-arm64` (assuming Apple
   Silicon host — check `docker version` for host arch first).
   Cross-compile flags:
   `-mcrt=newlib -mhard-float -O2 -mcpu=440 -D__PPC__ -D__USE_INLINE__ -D__USE_OLD_TIMEVAL__`.
   Link against `-lauto` for auto-opened library bases.

4. **Skeleton `virte1000.device`.** Enough to `OpenDevice` and
   return unit 0 without crashing. No I/O yet. Test path:
   ```
   docker run ... make            # build
   mcp: amiga_push_file → DH1:    # deploy
   mcp: amiga_dos_command("...")  # small C test app that
                                  # OpenDevice+CloseDevice
   ```
   If that closes cleanly you've got the shell right.

5. **PCI discovery.** Enumerate PCI, find vendor 0x8086 device
   0x100E. Get BAR0 (MMIO base). Print via `Printf` (guest CLI
   comes through the bridge). Don't touch the device yet.

6. **Bring up the descriptor rings.** RX ring, TX ring, RCTL/TCTL
   registers. See Intel 82540EM datasheet, section 13.

7. **IRQ handler.** Hook the OS4 PCI INTx via `AddIntServer`
   through the pci.library interface. Handle RX/TX completions
   and re-arm.

8. **SANA-II command dispatch.** `S2_ONLINE`, `S2_OFFLINE`,
   `CMD_READ` (buffer-fill), `CMD_WRITE` (buffer-drain),
   `S2_READORPHAN`, `S2_DEVICEQUERY`, `S2_GETSTATIONADDRESS`.
   Everything else can return `IOERR_NOCMD` at first.

9. **Copy semantics.** SANA-II uses `CopyFromBuff` / `CopyToBuff`
   hooks provided by the caller — you don't own the packet buffers,
   the network stack does. Get this right or you'll blow up
   Roadshow.

**Constraints:**

- Don't touch the `amiga_mcp` codebase unless you need to add an
  MCP tool that isn't there. If you do, ask the user first.
- Don't install to `SYS:Kickstart/` during development — a bad
  driver takes down the whole guest boot. Test with `OpenDevice`
  from a small user program in `DH1:` first. Only promote to
  Kickstart when stable.
- Assume Apple Silicon host — Docker pulls the arm64 variant of
  the walkero image. Verify with `docker image inspect
  walkero/amigagccondocker:os4-gcc11-arm64`.
- The AmigaOS clock is stored in local time; if you're testing
  anything time-sensitive, set it to `real_UTC - TZ_offset`. See
  the `os4_clock_gotcha` memory in the amiga_mcp memory dir.

**Start by asking the user which of the two paths to take first:**
1. Get a build environment working end-to-end with a no-op
   skeleton `.device` — proves the toolchain + deploy loop.
2. Study the QEMU e1000 source and Intel datasheet + write a
   design doc (`docs/DESIGN.md`) first, THEN build.

I'd default to path (1) — proves out the iteration loop before
the "hard part" of driver logic — but the user may prefer to see
a design first. Ask, don't assume.
