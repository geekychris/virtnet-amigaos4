# Perf journey — what worked, what didn't, and why

Chronological log of every perf lever attempted, with measurements.
Written so the next attempt starts from what we already know instead
of re-treading these paths.

Current live number: **25 Mbit/s sustained, 0 retransmissions.**
Reference: `virte1000` on the same host = **~40 Mbit/s**.

## The profile snapshot to keep in mind

Fresh QEMU boot, pyperf `--raw` 10 s benchmark, `testprofile` output:

```
tx_calls        = ~21000
avg cycles/pkt  = 23731  (237 us)
  cook          = 15464  (65%)  ← 155 us
    of which:
      hook         = 12131 cycles (70% of cook = 121 us)
      other        =  3333 cycles (30% of cook =  33 us)
                     ↑ zero-fill hdr, dst-MAC broadcast fill,
                       src-MAC copy, ethertype, semaphore + opener
                       lookup, padding
  flush         =    99  ( <1%)   ← 1 us
  ring          =  1060  (   4%)  ← 11 us
  notify        =  6505  (  27%)  ← 65 us

rx_calls        = ~21000
avg cycles/pkt  = 22484  (224 us)
  hook          =  7679  ( 34%)  ← 77 us
  other         = 14805  ( 66%)  ← 148 us (semaphore + opener walk
                                             + ReplyMsg + refill)
```

Two dominant costs: **CallHookPkt (both directions, 121 µs TX + 77 µs
RX = 198 µs/packet)** and **QEMU_NOTIFY MMIO trap (65 µs/packet)**.
Everything else is under 10% each.

## Experiments and results

### 1. VIRTIO_NET_F_CSUM offload — dormant on SLIRP (`076e6fd`)

- **Hypothesis:** QEMU host-CPU can compute L4 checksums instead of
  guest computing them; the negotiated feature bit + per-packet
  `VIRTIO_NET_HDR_F_NEEDS_CSUM` flag activates it.
- **Result:** SLIRP backend doesn't set `has_vnet_hdr` so QEMU strips
  CSUM from device_features. `driver_wants=08010021 accepted=08010020`
  → bit 0 masked out. Per-packet hint code never runs.
- **Kept:** the code is committed and takes effect automatically if
  someone runs against TAP or vhost-net.

### 2. VIRTIO_F_RING_EVENT_IDX with TX-IRQ suppression (`e2e8316`)

- **Hypothesis:** Halving IRQs (suppress TX-completion, keep RX)
  should meaningfully cut CPU spent in ISR + task-wake path.
- **Result:** IRQs did halve (21K vs 42K per 10s test). Throughput
  unchanged at ~25 Mbit/s.
- **Learning:** IRQ overhead was ~5–10% of per-packet cost, not the
  bottleneck. profile above confirms: 65 µs notify + 155 µs cook
  swamps whatever the IRQ path costs.
- **Kept:** the code stays — no regression and hooks up cleanly if
  we later batch RX.

### 3. Notify suppression via `avail_event` — reverted

- **Hypothesis:** Under EVENT_IDX the device publishes `avail_event`
  = the avail_idx it wants notified about. Suppress the MMIO write
  when the device is caught up.
- **Result:** SLIRP backend never updates `avail_event`, so the
  check never fires. But the check itself is a CACHEINHIBIT read
  every TX (uint16 load from ring memory) → adds latency without
  benefit. Perf dropped ~5%.
- **Reverted.**
- **Learning:** For this backend, notify-suppression is a null
  optimization with real overhead. Might be worthwhile on vhost-net
  where avail_event gets actively updated.

### 4. RX batching (K=4 via `used_event = last + K-1`) — CATASTROPHIC

- **Hypothesis:** Amortize the 148 µs of per-RX-batch overhead
  (semaphore, opener walk, ReplyMsg, refill) over 4 packets by
  waking every 4th arrival instead of every arrival.
- **Result:** Throughput crashed to 0.29 Mbit/s.
- **Root cause:** small-window TCP deadlock. Roadshow's sndbuf =
  33580 bytes ≈ 23 packets. If fewer than K ACKs arrive before
  window fills, we never wake, Roadshow never advances window,
  sender stalls. The self-signal safety-net we added wasn't enough
  because the trigger for "process now" was still tied to the
  interrupt.
- **Reverted.**
- **Learning:** RX batching + interrupt suppression only works if
  something else guarantees liveness — either a hard timer, or
  TX-triggered RX polling (whenever we send, also drain RX). Both
  require more machinery than a single ring-tail write.

### 5. Bypass `CallHookPkt` for cooked TX — reverted

- **Hypothesis:** `ios2_Data` in Roadshow's cooked-mode `CMD_WRITE`
  is a plain buffer pointer; the copy hook is just a memcpy.
  Replacing `CallHookPkt(hook, ...)` with an inline aligned
  memcpy would save the 121 µs hook cost.
- **Result:** Zero packets on the wire. TCP never connected.
  4 CMD_WRITEs total (Roadshow's initial ARP round).
- **Root cause:** Roadshow's cooked-mode TX passes a **scatter list**
  or **abstract descriptor** in `ios2_Data`, not a payload pointer.
  The hook does more than memcpy — it likely walks a fragment
  list and possibly builds the IP header on the fly. Bypassing
  the hook means we ship garbage.
- **Reverted.**
- **Learning:** True zero-copy TX for cooked mode requires
  reverse-engineering Roadshow's buffer format or getting SDK
  documentation. Not a code change; a research task.

### 6. Prefer S2_CopyFromBuff32 tag-list path over Sana2Hook — reverted

- **Hypothesis:** Roadshow's `sana2_hook` always services the
  classic `S2_CopyFromBuff` method, but the same opener typically
  registered an `S2_CopyFromBuff32` variant in its tag list. The
  32-variant might have a faster path inside Roadshow. Switch
  priority so we call the tag-list path first when a 32/16 variant
  is advertised. Also offset each `tx_pool` slot by 8 bytes so the
  payload target lands on a 32-byte boundary (S2_CopyFromBuff32's
  alignment requirement).
- **Result:** Throughput regressed to 11 Mbit/s + 18 s elapsed for
  a `-t 10` test (Roadshow retry/backoff behavior).
- **Not fully diagnosed.** Either the 32-variant isn't actually
  registered (fell through to same path, and something else broke)
  or the +8 offset breaks a QEMU alignment assumption we're not
  aware of. Reverted for stability.
- **Learning:** Priority changes in the hook dispatch are subtle;
  need per-run confirmation of which hook variant Roadshow actually
  ends up using before this optimization is worth pursuing.

## What actually landed

- `35d542b` — 32-slot per-desc TX scratch pool (0.5 → 26 Mbit/s
  massive gain; the entry-level fix).
- `97c4e6d` — Pool grown to 256 slots (retransmissions vanish).
- `076e6fd` — CSUM offload code (dormant on SLIRP, no-op on
  current setup).
- `e2e8316` — EVENT_IDX + TX-IRQ suppression (IRQ count halved,
  perf-neutral).
- `1878f0f` + `d4fbd72` — TB-cycle profiler + hook sub-slice
  measurement.

## What's next (for whoever picks this up)

### Realistic short-term levers (5-15% each, medium difficulty)

- **Batched CMD_WRITE with deferred notify.** Instead of one MMIO
  notify per packet, buffer up to K CMD_WRITE-generated avail
  entries and notify once. Requires a timer or heuristic to
  flush partial batches. Saves 65 µs × (K-1)/K per packet.

- **RX batching + hard timer.** RX batching regressed at K=4
  because TCP deadlocked. A hard 100 µs timer that forces
  vn_process_rx even without enough packets would break the
  deadlock and let the amortization kick in.

- **Faster copy-hook variant.** The Sana2Hook internally
  services S2_CopyFromBuff{16,32} — the fast paths advertised
  via `s2h_Methods`. Dispatching to those requires 16/32-byte
  aligned `to` and picking the right method by walking the
  advertised list. Attempt 6 tried this and regressed; needs
  more careful investigation of what Roadshow actually provides.

### Real zero-copy TX (big-ticket, ~25% possible, high difficulty)

- Understand Roadshow's cooked-mode buffer format (attempt 5
  ruled out the naive-pointer assumption).
- Chain two descriptors per TX: virtio_net_hdr in tx_pool,
  payload pointing directly at Roadshow's memory via DMA phys.
- Defer `ReplyMsg` until TX-completion IRQ fires (need to
  re-enable TX-completion event, add per-slot ioreq tracking).
- Requires either Roadshow SDK access or thorough disassembly
  of the copy_from_buff hook to understand the buffer layout.

### Nuclear option (~40 Mbit/s and beyond)

- Modern virtio-net-pci-modern (device 0x1041): MMIO register
  layout, MSI-X per-queue vectors, packed rings, VERSION_1
  features. Legacy is fine for the current numbers, modern
  unlocks higher ceilings.

## Meta-lesson

Every perf experiment this session either regressed or was neutral.
The **profile is the source of truth** — the CallHookPkt cost
dominates and nothing we can do at the ring level touches it.
Future perf work should target `cook` directly and be validated
against `testprofile` on every iteration.
