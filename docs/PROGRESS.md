# Implementation progress

Phase-by-phase checklist of what's proven live vs. what's still TODO.
Each phase corresponds to a git commit tagged `Phase 10X`.

## Phase 10a — QEMU config ✅

QEMU sam460ex config extended with virtio-net-pci alongside the
existing rtl8139 (bridge) and e1000-82540em (virte1000). Subnet
192.168.101.0/24. Verified via `virte1000`'s init log:

```
probe virtio-net legacy (1AF4:1000): FOUND at PCI 00:04.0
```

The device is transitional — QEMU exposes the legacy device ID
(0x1000). Modern (0x1041) would need capability walking; not needed
for this driver.

## Phase 10b — Legacy PCI transport plumbing ✅

`src/virtio.c` + `include/virtio.h`:
- Register offsets (VIRTIO_PCI_*)
- Status / ISR bits
- Feature bit constants
- Virtqueue struct definitions
- `VRING_TOTAL_BYTES(num)` layout macro (desc + avail + 4KB pad + used)
- I/O accessors: `vio_read8/16/32` + `vio_write8/16/32`

## Phase 10c — Init handshake ✅

Full virtio 0.9.5 init sequence:

```
FindDeviceTags(1AF4:1000): FOUND at PCI 00:04.0
virtio: io_base=00001240
virtio: reset OK, status=03
virtio: device_features=79BF8064 driver_wants=00010020 accepted=00010020
virtio queues: RX num=256, TX num=256
MAC         = 52:54:00:12:34:58
virtio link status: link=0001 (UP)
```

Fix: device-specific config on legacy virtio is guest-native (BE
on PPC), NOT LE like the standard registers.

## Phase 10d/e — Virtqueue setup + IRQ install + DRIVER_OK ✅

```
virtio vrings: rx=<ptr> phys=<page-aligned> (10246 bytes) tx=... (10246 bytes)
virtio rx_bufs: cpu=... phys=... pool=524288
virtio tx_scratch2: cpu=... phys=... (2048 bytes)
MapInterrupt: vector=48
AddIntServer: vec=48 result=OK
virtio: DRIVER_OK set, status=0F (device may now use queues)
```

- Vrings allocated with AVT_Alignment=4096 (verified: low 12 bits == 0)
- RX buffer pool 512 KB (256 × 2 KB) populated into avail ring
- IRQ vector 48 hooked, `vn_isr` reads VIRTIO_PCI_ISR

## Phase 10f/g — TX + RX paths ✅ (code) / ⚠️ (live)

Code written and verified against QEMU:
- vn_process_rx walks used ring, delivers to opener CopyToBuff, refills
- ISR signals unit task on VIRTIO_ISR_QUEUE (bit 0)
- CMD_WRITE builds virtio_net_hdr + Ethernet frame in tx_scratch2,
  pushes descriptor 0 onto avail, notifies TX queue, polls used

Live: Init completes cleanly, Roadshow binds interface at 192.168.101.15,
dispatches SANA-II sequence (SANA2HOOK → DEVICEQUERY → GETSTATIONADDRESS
→ CONFIGINTERFACE) all err=0. **NO CRASHES** (major win vs virte1000).

## Phase 10h — Endianness fix ✅

Discovered: QEMU's virtio-net-pci treats virtqueue memory as
**little-endian** regardless of guest endianness. Spec says
"guest-native" for legacy transport, but QEMU (and Linux/FreeBSD
virtio drivers on PPC BE) work around this by using LE consistently.

Added `vio_le16_get/put` + `vio_le32_get/put` helpers and rewrote
every ring access site.

## Phase 10i — Ping ⚠️ (blocked)

Current state after all above phases:

```
$ ping -c 3 192.168.101.2
PING 192.168.101.2 (192.168.101.2): 56 data bytes
--- 192.168.101.2 ping statistics ---
3 packets transmitted, 0 packets received, 100% packet loss
```

Diagnostics:
- cmdlog shows 3× CMD_WRITE ptype=0x0800 (ICMP) with dst MAC = zeros
- No S2_BROADCAST calls (Roadshow didn't ARP through us)
- teststat: state=ONLINE, irq=0, no rx delivered

Hypotheses:
1. **Roadshow's ARP isn't reaching virtnet.** Possibly a routing-
   table issue (default route via rtl8139?), or Roadshow's ARP
   uses a path that OS4 exec rejects (same `IOERR_UNITBUSY` trap
   that bit virte1000).
2. **QEMU IRQ delivery.** Zero IRQs after 3 pings means either
   TX-complete IRQ isn't firing (device quirk) or nothing was
   actually DMA'd. Test with an explicit DBG_FIRE_IRQ once ISR
   can be triggered.
3. **QEMU dropping our TX packets.** If descriptor addr_lo is
   wrong (e.g., we're publishing a wrong physical address), QEMU
   silently drops. Use `-object filter-dump,id=n2-dump,netdev=n2,
   file=/tmp/n2.pcap` to capture the netdev traffic and see.

## What's inherited from virte1000

The SANA-II layer — `_manager_BeginIO` fast-path for DBG commands,
`vn_dispatch_ioreq` switch, opener list, `sana2_hook` handling,
`vn_invoke_copy_to/from`, DBG_STATUS/DBG_CMDLOG reads, unit task
lifecycle — is copied verbatim from virte1000. Everything at the
SANA-II layer works cleanly here (no crashes, all cmd err=0 in
cmdlog). The remaining pin is purely on the virtio side.

---

## Phase 10j — Ring memory visibility ✅

Multi-subphase saga. Symptom: driver Init completed cleanly, `state=ONLINE`,
Roadshow dispatched `CMD_WRITE`, our code claimed success (`io_Error=0`),
but QEMU's virtio trace showed `virtio_queue_notify` firing with **zero**
`virtqueue_pop` events, and n2 pcap stayed empty.

- **10j-6/7** — flipped ring endianness to BE (guest-native). No help.
- **10j-11** — tried `AVT_Lock=TRUE` for pinned physical mapping. No help.
- **10j-12** — piggybacked `tx_scratch2` inside `tx_vring` alloc to
  guarantee same DMA region. Helped later once other fixes landed.
- **10j-14/15** — swapped `AllocVecTags` for plain `AllocMem+MEMF_KICK`.
  DMA visibility improved but still failed.
- **10j-17** — added explicit `dcbf`-per-cacheline flushes on TX payload
  + descriptor + avail ring. Necessary but not sufficient.
- **10j-18** — reverted to LE ring writes via `stwbrx`/`sthbrx`. This
  was ALSO wrong on QEMU 11 for this target, but it was the last
  guess before we found the real cause months later. See Phase 13.

## Phase 10k — CACHEINHIBIT for vrings ✅

`IMMU->SetMemoryAttrs(CACHEINHIBIT|GUARDED|COHERENT|READ_WRITE)` on
`rx_vring` / `tx_vring` bypassed the cache entirely for ring memory,
so avail-idx updates were guaranteed visible to QEMU without any
`dcbf` ceremony. Combined with the 10j fixes this got single-packet
TX working.

## Phase 11 — Ping ✅

End-to-end ARP + ICMP round-trip through virtnet.

## Phase 12 — Perf attempt (incomplete) ⚠️

Committed as "37.63 Mbit/sec via pyperf" but on retest that number
was not reproducible — see Phase 13 for the real story. Phase 12
did land two lasting fixes:

- Always-signal-ISR (unit task wakes on every IRQ entry regardless
  of what VIRTIO_PCI_ISR reads — this platform's INTx delivery
  races the ISR bit set)
- CallHookPkt-always for the SANA-II copy hooks (ported from
  Bill Borsari's virte1000 fix)

## Phase 13 — Real perf unblock ✅

Three separate bugs found via QEMU monitor's `info virtio-status`
and `info virtio-queue-element`. Each documented in its own commit:

### 13a — S2_DEVICEQUERY HardwareType (`6bf6277`)

The dispatch handler capped `supply` at 24 bytes, stopping short
of the `HardwareType` field at pack(2) offset 26. `ShowNetStatus`
therefore reported virtnet's type as `Unknown (0)` and Roadshow
refused to install a connected route for it. Bumped supply to 34;
the zero-fill loop got `volatile` so `-O2` couldn't turn it into
`memset()` (resident-tag `.device` can't link newlib's memset).

### 13b — Ring endianness + `vring_desc.addr` field order (`e01eae5`)

QEMU 11's `info virtio-status` for our virtio-net-pci reports
`endianness: big` — legacy virtio on a BE PPC guest uses guest-
native endianness, exactly as the spec says. Phase 10j-18's flip
to byteswapped LE was wrong for this QEMU. Reverted to plain
BE-native `*p = val` accessors.

But that alone still gave zero-content frames on the wire.
`info virtio-queue-element` decoded a descriptor pointing at
`0x3F33E820_00000000` — way past guest RAM. Root cause: our
`vring_desc` struct split the spec's single 64-bit `addr` into
`addr_lo` then `addr_hi`. QEMU reads the whole thing as one BE
64-bit load, so our low-word ended up in the *high* half of the
reconstructed address. Reordering to `addr_hi` first put the low
word back in the low bytes and QEMU decoded the address correctly.

### 13c — Per-slot TX scratch pool (`35d542b`, `97c4e6d`)

With TX unblocked, throughput was 0.5 Mbit/sec and every test
ended in `send: Broken pipe` from server-side RST. Pcap showed
regular packet loss every ~5000 bytes, then heavy retransmissions,
then the server giving up. Root cause: our single `tx_scratch2`
buffer served every `CMD_WRITE`; the completion poll before reuse
frequently timed out with QEMU still DMA-reading the previous
packet, so the next `CMD_WRITE` overwrote its payload mid-flight
and pushed corrupt bytes on the wire.

- 32-slot pool: throughput jumped to 26.6 Mbit/s but pcap still
  showed 25% retransmissions (Roadshow's dispatch rate exceeded
  the 32-slot wrap-back window).
- 256-slot pool (one per descriptor): 25.3 Mbit/s sustained,
  **zero retransmissions**, clean TCP close, pyperf reports the
  full byte count.

The completion poll is gone entirely. Reply-and-forget after the
notify: each in-flight descriptor carries its own buffer, so no
race is possible.

## Current live perf

Fresh QEMU boot, first perf test:

```
$ pyperf-driven iperf3 client (--raw), 10 s
[stats] 485 calls, 0 short writes, avg 65536 bytes/call
=== raw summary ===
bytes:      31784960
elapsed:    10.04 s
throughput: 25.32 Mbit/sec  (3.02 MB/sec)

$ tcpdump wire analysis
TX packets:   21774
RX ACKs:      21774  (perfect 1:1)
Retransmits:  0
```

Baseline reference: `virte1000` (Bill Borsari's e1000 driver, same
sam460ex host) hits ~40 Mbit/sec on the identical harness. Virtnet
is ~35% behind that; closing the gap is future work (candidate
levers listed at the bottom of README).
