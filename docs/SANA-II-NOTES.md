# SANA-II Rev 7 — implementer's notes for virte1000

Working reference distilled from the AmigaOS wiki spec and cross-referenced
against the OS4 SDK header we're actually going to compile against.

Primary sources:
- `[wiki]` = https://wiki.amigaos.net/wiki/SANA-II_Revision_7 (the whole
  spec is on a single very long page; anchors are `#S2_XXX`)
- `[sana2.h:N]` = `refs/os4-sdk/base/Include/include_h/devices/sana2.h`
  line N

If a claim doesn't have a cite next to it, it's a design decision I've made
for virte1000 based on the spec, not something the spec pins down.

Anything I couldn't confirm is in section 10 (Open questions). Do not
guess-fill it later — go read a shipping OS4 driver source instead.

---

## 1. Open protocol

### 1.1 The caller's side

A protocol stack (Roadshow, AmiTCP, a raw test program, ...) opens the
device roughly like:

```c
struct IOSana2Req *req = (struct IOSana2Req *)
    AllocSysObject(ASOT_IOREQUEST, ...);

struct TagItem bm_tags[] = {
    { S2_CopyToBuff,   (ULONG)my_copy_to_buff_hook },
    { S2_CopyFromBuff, (ULONG)my_copy_from_buff_hook },
    { S2_PacketFilter, (ULONG)my_filter_hook },  /* optional */
    { S2_Log,          (ULONG)my_log_hook },     /* optional */
    { TAG_END, 0 }
};

req->ios2_BufferManagement = (APTR)bm_tags;      /* IN: taglist ptr */

OpenDevice("virte1000.device", unit, (struct IORequest *)req, flags);

/* On success, ios2_BufferManagement now holds a driver-private
 * "magic cookie". The caller MUST use this req (or copy the cookie
 * into every future req it sends to this unit). */
```

The caller loads the tag list into `ios2_BufferManagement` **before**
calling `OpenDevice()`. On successful open the driver overwrites that
field with a driver-private "magic cookie" — the opaque pointer that
tags every IO request with the identity of its opener. `[wiki: "The
driver overwrites `ios2_BufferManagement` with a magic cookie"]`

### 1.2 Recognised OpenDevice tags

From `[sana2.h:112-125]`:

| Tag                    | Value              | Required? | Notes                                        |
|------------------------|--------------------|-----------|----------------------------------------------|
| `S2_CopyToBuff`        | `S2_Dummy + 1`     | **YES**   | Copy driver→opener (RX)                      |
| `S2_CopyFromBuff`      | `S2_Dummy + 2`     | **YES**   | Copy opener→driver (TX)                      |
| `S2_PacketFilter`      | `S2_Dummy + 3`     | no        | Called before CopyToBuff on RX               |
| `S2_CopyToBuff16`      | `S2_Dummy + 4`     | no        | Optimised 16-bit-aligned copy                |
| `S2_CopyFromBuff16`    | `S2_Dummy + 5`     | no        | "                                            |
| `S2_CopyToBuff32`      | `S2_Dummy + 6`     | no        | Optimised 32-bit-aligned copy                |
| `S2_CopyFromBuff32`    | `S2_Dummy + 7`     | no        | "                                            |
| `S2_DMACopyToBuff32`   | `S2_Dummy + 8`     | no        | Returns physaddr if opener buffer is DMA-OK  |
| `S2_DMACopyFromBuff32` | `S2_Dummy + 9`     | no        | "                                            |
| `S2_DMACopyToBuff64`   | `S2_Dummy + 10`    | no        | 64-bit DMA variant                           |
| `S2_DMACopyFromBuff64` | `S2_Dummy + 11`    | no        | "                                            |
| `S2_Log`               | `S2_Dummy + 12`    | no        | Log callback for driver diagnostics          |

`S2_Dummy = TAG_USER + 0xB0000` `[sana2.h:112]`.

### 1.3 What virte1000's Open method must do

1. Look up the unit; if it doesn't exist, fail with `IOERR_OPENFAIL`.
2. Enforce `SANA2OPB_MINE` semantics: if any opener already holds the
   unit with `SANA2OPF_MINE`, fail with `IOERR_UNITBUSY`. If this open
   requests `SANA2OPF_MINE` and there are already openers, same.
3. Enforce `SANA2OPB_PROM` semantics: promiscuous requires
   `SANA2OPF_MINE`. `[wiki: "Promiscuous mode requires exclusive
   opening of the device."]` If PROM without MINE, fail.
4. Walk the tag list in `ios2_BufferManagement`. Both `S2_CopyToBuff`
   and `S2_CopyFromBuff` are **mandatory**; missing either → fail with
   `IOERR_OPENFAIL` and set `ios2_WireError = S2WERR_FUNCTIONS_MISSING`
   `[sana2.h:460]`. `[wiki: "The device will fail to open if the driver
   user does not supply all of the required functions."]`
5. Allocate a `V1000Opener` struct (see §8), copy the resolved hook
   pointers into it, initialise its lists, link it into the unit's
   opener list.
6. Overwrite `ios2_BufferManagement` with a pointer to the `V1000Opener`
   struct. That's the "magic cookie".
7. Set `io_Error = 0`, `io_Unit = &unit->pub`, return normally.

Tags with `ti_Data == NULL` must be treated as "not supplied"
`[wiki: "Buffer management tags with a NULL value must be treated as
not specified."]`.

### 1.4 Per-opener vs shared state

Live on the **opener**:
- Copy hooks (each opener supplies its own)
- Log hook, packet filter hook
- Tracked packet-type list + per-type stat counters (from
  `S2_TRACKTYPE`)
- Pending `CMD_READ` queue, keyed by packet type
- Pending `S2_READORPHAN` queue
- Pending `S2_ONEVENT` queue
- `SANA2OPF_MINE` / `SANA2OPF_PROM` flags

Live on the **unit**:
- Hardware state (BARs, MSI, ring buffers, MAC, MII PHY)
- Global `Sana2DeviceStats` counters
- Global multicast address table (with per-address refcount)
- The station address itself
- Online/offline flag
- Reconfiguration count

The refcount on multicast addresses is critical: two openers subscribing
to the same IPv6 solicited-node multicast must both see it, and only
when the last opener drops it may the driver remove it from the e1000
MTA. `[wiki: "The driver...maintains enable count per address"]`

### 1.5 CloseDevice

Cancel every pending IO on this opener's queues with `IOERR_ABORTED`.
Decrement multicast refcounts for anything this opener had subscribed
to; program the e1000 MTA if any counts dropped to zero. If this was
the last opener and the unit auto-offlines on empty, take it offline.
Unlink and free the `V1000Opener`.

---

## 2. CopyFromBuff / CopyToBuff hook signatures and semantics

**This is the section that matters most. On PPC OS4 the classic 68k
register-based signature is dead — the spec explicitly forbids it.**

### 2.1 The mandatory PPC calling convention

`[wiki: "the driver must invoke the Hooks via utility.library/
CallHookPkt. It must never invoke the hook functions through local
assembly language stubs or the amiga.lib/CallHook and amiga.lib/
CallHookA functions."]`

The opener supplies a `struct Hook *` (from `<utility/hooks.h>`), NOT
a raw function pointer with a register calling convention. Every tag
value pointer for `S2_CopyToBuff`, `S2_CopyFromBuff`, `S2_PacketFilter`,
`S2_Log`, and each 16/32/DMA variant is a `struct Hook *`.

The hook is invoked via:

```c
BOOL ok = (BOOL)IUtility->CallHookPkt(hook, sana2req, &copyhookmsg);
```

The hook C-level entry point (what `h_Entry` points to, or what
`HookEntry` from amiga.lib dispatches to via `h_SubEntry`) is a
standard OS4 hook function:

```c
ULONG my_copy_to_buff(struct Hook *hook,
                      struct IOSana2Req *req,
                      struct SANA2CopyHookMsg *msg);
```

Return: non-zero = success, zero = failure `[wiki: "the copying hooks
return a boolean (success/failure)"]`. For DMA variants: return NULL =
"can't DMA this buffer, fall back to CPU copy"; return non-NULL = the
buffer address suitable for DMA. `[wiki: "For DMA related hooks, a
FALSE return value is equivalent to a NULL pointer."]`

### 2.2 The hook message

From `[sana2.h:333-341]`:

```c
struct SANA2CopyHookMsg {
    ULONG schm_Method;   /* which copy variant is being invoked */
    ULONG schm_MsgSize;  /* sizeof(*this) */
    APTR  schm_To;       /* destination                          */
    APTR  schm_From;     /* source                               */
    ULONG schm_Size;     /* byte count                           */
};
```

`schm_Method` = the tag ID that resolved this hook — one of
`S2_CopyToBuff`, `S2_CopyFromBuff`, `S2_CopyToBuff16`, ...,
`S2_DMACopyFromBuff64`. Openers whose hook services multiple methods
(e.g. one hook for all four "to-buff" variants) key off this.

`schm_MsgSize` = `sizeof(struct SANA2CopyHookMsg)` — 20 bytes on a
32-bit target. Openers MAY check this and reject shorter messages.

### 2.3 Direction — which way does data flow?

The spec is unambiguous but the names are a UX disaster. Verified from
`[wiki]`:

- **`CopyToBuff`** = "copy to the opener's buffer" = **RX path**.
  Wire → driver descriptor → *(CopyToBuff)* → opener's abstract data.
  `schm_From` = driver-owned RX descriptor payload.
  `schm_To`   = the opener's abstract buffer (`ios2_Data`).

- **`CopyFromBuff`** = "copy from the opener's buffer" = **TX path**.
  Opener's abstract data → *(CopyFromBuff)* → driver descriptor → wire.
  `schm_From` = the opener's abstract buffer (`ios2_Data`).
  `schm_To`   = driver-owned TX descriptor payload.

Mnemonic: the "buff" in the name is the **driver's** DMA descriptor
buffer. "To buff" = filling the driver's buffer for a receive that will
end up in the opener's data area (wait — no, that's the wrong way).
Give up on the mnemonic; just use the table.

I've triple-checked this against the wiki text: *"The data copied (via
a call to the requestor-provided CopyToBuffer function) into ios2_Data
is normally the Data Link Layer packet data only"* — so CopyToBuff
writes into `ios2_Data`, i.e. into the opener's buffer, on RX.
Confirmed. `[wiki]`

### 2.4 The variants

| Variant                | Alignment / geometry               | Use when                                       |
|------------------------|-------------------------------------|------------------------------------------------|
| `S2_CopyToBuff`        | unaligned, byte-granular            | fallback, always available                     |
| `S2_CopyFromBuff`      | "                                   | "                                              |
| `S2_CopyToBuff16`      | opener guarantees 16-bit alignment  | driver can use halfword loads/stores           |
| `S2_CopyFromBuff16`    | "                                   | "                                              |
| `S2_CopyToBuff32`      | opener guarantees 32-bit alignment  | driver can use word loads/stores               |
| `S2_CopyFromBuff32`    | "                                   | "                                              |
| `S2_DMACopyToBuff32`   | asks: "may I DMA into this?"        | zero-copy RX — see below                       |
| `S2_DMACopyFromBuff32` | asks: "may I DMA from this?"        | zero-copy TX — see below                       |
| `S2_DMACopyToBuff64`   | 64-bit-aligned DMA                  | same, wider                                    |
| `S2_DMACopyFromBuff64` | "                                   | "                                              |

For virte1000, the 16/32-bit variants aren't very interesting — copies
are through PPC 440 cache and we already touch data with word loads.
The DMA variants **are** interesting but I don't think Roadshow
implements them (§10, open question).

### 2.5 DMA variants — semantics

For a DMA "to buff" variant on RX: before completing the RX, driver
calls the DMA hook asking "if I DMA the packet directly into your
abstract buffer, will that be a valid physical address for me?" Opener
returns either:
- `NULL` → "no, use the CPU copy path", driver falls back to
  `S2_CopyToBuff`.
- non-NULL address → this is the (physically contiguous, correctly
  aligned) buffer the driver may DMA into. The opener is promising to
  keep this pinned until the request completes.

Symmetric for "from buff" on TX.

virte1000 will **not** use DMA variants in v1. The 82540EM does DMA
into descriptor-referenced buffers we own; bouncing through
CopyToBuff / CopyFromBuff (a straightforward memcpy inside a hook
call) is fine for a first version. Revisit for perf later.

### 2.6 What if the hook returns "failure"?

`[wiki: "S2WERR_BUFF_ERROR — buff mgt func returned error"]`
`[sana2.h:442]`. Driver reports:
- `io_Error = S2ERR_SOFTWARE` (or `S2ERR_NO_RESOURCES` for DMA fail)
- `ios2_WireError = S2WERR_BUFF_ERROR`

and signals `S2EVENT_BUFF` `[sana2.h:474]` to any queued
`S2_ONEVENT` requests. Then either drops the packet (RX) or fails the
CMD_WRITE (TX).

### 2.7 Interrupt-context rules

Copy hooks may be called from interrupt context on RX (there's no
promise they won't be — the driver's RX ISR can complete a CMD_READ
directly). The spec does not forbid it. `[wiki]`

The `S2_Log` hook, in contrast, explicitly **must not** be called from
interrupt context, because it may need to allocate memory or call
locale.library. `[wiki: "the log hook must not be called from
interrupt code"]`

Practical rule for virte1000: RX bottom half. Don't complete
CMD_READ from the interrupt handler; move all RX completion to a
device Task woken by a Signal from the ISR. Reasons:
- Lets us drop the "hook must be interrupt-safe" assumption entirely.
- Roadshow's `CopyToBuff` will absolutely take a semaphore internally
  even if the spec claims it needn't.
- Interrupt latency budget on OS4 sam460ex is not generous.

### 2.8 What if the hook is NULL?

Not legal for `S2_CopyToBuff` / `S2_CopyFromBuff` — the open fails
with `S2WERR_FUNCTIONS_MISSING` `[sana2.h:460]`. All other hook tags
being NULL or absent is legal; driver just doesn't invoke them.

The spec does not permit "fall back to `CopyMem` if the opener didn't
supply a hook". The abstract data type of `ios2_Data` is deliberately
opaque — it might be an mbuf chain, a userspace pointer, a virtual
address of a locked-down page, whatever. Only the opener knows.

---

## 3. Command reference

Numeric values from `[sana2.h:373-405]`. Base is
`S2_START = CMD_NONSTD = 9`.

### 3.1 `S2_DEVICEQUERY` (9)

Roadshow calls this early to discover MTU, HW type, MAC size.

- Caller sets: `ios2_StatData` → pointer to a
  `struct Sana2DeviceQuery` `[sana2.h:130]` with `SizeAvailable`
  initialised.
- Driver: fills `SizeSupplied`, `DevQueryFormat` (0), `DeviceLevel` (0),
  `AddrFieldSize` (48 for Ethernet), `MTU` (1500), `BPS` (1e9 — see
  §5), `HardwareType` (`S2WireType_Ethernet` = 1 `[sana2.h:158]`),
  `RawMTU` (1514).
- Reply: synchronous. Set `io_Error = 0`, `ios2_WireError = 0`.
- Errors: `S2ERR_BAD_ARGUMENT` + `S2WERR_BAD_STATDATA` `[sana2.h:449]`
  if `ios2_StatData` is NULL or `SizeAvailable` too small.
- Roadshow: **required**. Called at bind time. Must work while offline.

Errata: the classic A2065 driver misbehaves if `SizeAvailable > 30`
`[wiki]`. Not relevant to virte1000 clients — Roadshow supplies enough.

### 3.2 `S2_GETSTATIONADDRESS` (10)

- Caller sets: nothing meaningful; `ios2_SrcAddr` and `ios2_DstAddr`
  will be written.
- Driver: writes the **factory / default** MAC into `ios2_SrcAddr` and
  the **currently-configured** MAC into `ios2_DstAddr`. For virte1000
  in QEMU, the factory MAC comes from the e1000 EEPROM (words 0-2).
- Reply: synchronous. Must work while offline / unconfigured.
- Errors: essentially none in normal operation.
- Roadshow: **required**. Roadshow uses this to auto-generate its
  interface name.

### 3.3 `S2_CONFIGINTERFACE` (11)

- Caller sets: `ios2_SrcAddr` = the MAC address to program into the
  hardware. May be all-zeroes meaning "use factory".
- Driver: writes the MAC into the e1000 RAR0, brings the PHY up if
  needed (usually deferred to `S2_ONLINE`), transitions unit to
  "configured but offline" state. **May be called only once**
  `[wiki]`.
- Reply: synchronous. Second call → `io_Error = S2ERR_BAD_STATE`,
  `ios2_WireError = S2WERR_IS_CONFIGURED` `[sana2.h:451]`.
- Errors: `S2WERR_NULL_POINTER` if `ios2_SrcAddr` obviously bad;
  `S2ERR_BAD_ADDRESS` for multicast/broadcast MAC.
- Roadshow: **required**. Called after `S2_GETSTATIONADDRESS`.

### 3.4 `S2_ONLINE` (25)

- Caller sets: nothing.
- Driver: enables e1000 RX/TX, brings PHY link up, resets stats,
  updates `Sana2DeviceStats.LastStart`, increments `Reconfigurations`,
  signals `S2EVENT_ONLINE` to all queued `S2_ONEVENT` requests whose
  mask includes it.
- Reply: synchronous once hardware is up.
- Errors: `S2ERR_BAD_STATE` + `S2WERR_NOT_CONFIGURED` if
  `S2_CONFIGINTERFACE` not yet run.
- Roadshow: **required**. Called after config.

### 3.5 `S2_OFFLINE` (26)

- Caller sets: nothing.
- Driver: disables e1000 RX/TX. **All pending IO requests** on all
  openers are returned with `io_Error = S2ERR_OUTOFSERVICE`
  `[sana2.h:418]`. `[wiki: "All pending and new requests to the driver
  shall be returned with S2ERR_OUTOFSERVICE when a device is
  off-line."]` Signals `S2EVENT_OFFLINE` to `S2_ONEVENT` queues.
- Reply: synchronous.
- Errors: none normally.
- Roadshow: called at shutdown.

### 3.6 `S2_READORPHAN` (24)

- Caller sets: `io_Command`, `ios2_Data` (opener's buffer), maybe
  `SANA2IOB_RAW` in `io_Flags`.
- Driver: this request sits in the opener's orphan queue. When a
  packet arrives whose ether-type has **no** pending `CMD_READ` on
  **any** opener **and** the destination is our MAC / broadcast /
  subscribed multicast (or PROM), the driver satisfies the oldest
  orphan reader from **some** opener with it. `[wiki: "It is a
  request to read any packet of a type for which there is no
  outstanding CMD_READ."]`
- Reply: async when a matching orphan arrives. Populate `ios2_SrcAddr`,
  `ios2_DstAddr`, `ios2_DataLength`, `ios2_PacketType`, invoke
  `CopyToBuff`.
- Errors: `S2ERR_OUTOFSERVICE` if unit offline; `S2WERR_BUFF_ERROR`
  if the copy hook fails.
- Roadshow: **not used**. Roadshow tracks all types it cares about.

### 3.7 `CMD_READ` (2)

- Caller sets: `io_Command`, `ios2_PacketType` (16-bit ether-type in
  low bits), `ios2_Data`, possibly `SANA2IOB_RAW`.
- Driver: enqueues on this opener's per-type read queue. When an RX
  arrives matching `(opener_is_subscribed_via_TRACKTYPE, ether-type)`
  the oldest read on that opener's queue for that type is filled.
- Reply: async. Populate `ios2_SrcAddr`, `ios2_DstAddr`,
  `ios2_DataLength`, `SANA2IOF_BCAST`/`SANA2IOF_MCAST` in `io_Flags`
  as appropriate `[sana2.h:95-97]`, then `CopyToBuff` the payload.
- Errors: `S2ERR_OUTOFSERVICE`, `S2WERR_BUFF_ERROR`.
- Roadshow: **required**. Many concurrent CMD_READs pending is normal
  and expected. `[wiki: "you should have multiple CMD_READ requests
  pending at any given time."]`

### 3.8 `CMD_WRITE` (3)

- Caller sets: `io_Command`, `ios2_PacketType`, `ios2_DstAddr`,
  `ios2_DataLength`, `ios2_Data`, optionally `SANA2IOB_RAW`.
- Driver: if not RAW, prepend a 14-byte Ethernet header
  (`dst | src(=our MAC) | ethertype`) and use `CopyFromBuff` to pull
  the payload into a driver TX descriptor buffer, then submit to the
  e1000 TX ring. If RAW, treat `ios2_Data` as an already-framed
  L2 frame (`ios2_DataLength` includes the 14-byte header).
- Reply: async, on TX descriptor writeback. In practice the ISR can
  complete it as soon as the descriptor's DD bit is set.
- Errors:
  - `S2ERR_OUTOFSERVICE` if offline
  - `S2ERR_MTU_EXCEEDED` if `ios2_DataLength > MTU` (cooked) or
    `> RawMTU` (raw) `[sana2.h:415]`
  - `S2ERR_BAD_ADDRESS` + `S2WERR_DST_ADDRESS` for zero MAC etc.
  - `S2WERR_BUFF_ERROR` if `CopyFromBuff` fails
- Roadshow: **required**.

### 3.9 `S2_BROADCAST` (17)

- Caller sets: same as CMD_WRITE **except** `ios2_DstAddr` is ignored /
  overwritten.
- Driver: forces destination MAC to `ff:ff:ff:ff:ff:ff`, otherwise
  identical to CMD_WRITE.
- Reply / errors: as CMD_WRITE, plus `S2ERR_NOT_SUPPORTED` if the
  hardware doesn't do broadcast (e1000 does).
  `[wiki: "S2_BROADCAST — send broadcast packet"]`
- Roadshow: used for ARP.

### 3.10 `S2_MULTICAST` (16)

- Caller sets: as CMD_WRITE, `ios2_DstAddr` = a multicast MAC.
- Driver: validate that `ios2_DstAddr[0] & 0x01` (LSB of first octet
  set = multicast per IEEE 802.3). If not, `S2ERR_BAD_ADDRESS` +
  `S2WERR_BAD_MULTICAST` `[sana2.h:446]`. Otherwise submit like a
  cooked CMD_WRITE.
- Reply / errors: as CMD_WRITE.
- Roadshow: used for IPv6 ND, mDNS, IGMP.

### 3.11 `S2_ADDMULTICASTADDRESS` (14)

- Caller sets: `ios2_SrcAddr` = a multicast MAC to enable RX for.
- Driver: increment refcount on this address in the unit's multicast
  table. If the count went 0→1, program the e1000's MTA (multicast
  table array — a 128×32 bit hash filter) with the corresponding hash
  bit set.
- Reply: synchronous.
- Errors: `S2WERR_MULTICAST_FULL` if we're capping the table
  (arbitrary — say 64 entries); `S2ERR_BAD_ADDRESS` + `S2WERR_
  BAD_MULTICAST` if not actually multicast.
- Roadshow: called at bind for IPv4/IPv6 protocol multicasts.

### 3.12 `S2_DELMULTICASTADDRESS` (15)

- Caller sets: `ios2_SrcAddr` = the address to un-enable.
- Driver: decrement refcount; if it hit zero, re-compute the MTA (or
  clear that hash bit if no other subscribed address hashes to it).
- Reply: synchronous.
- Errors: `S2WERR_BAD_MULTICAST` if the address wasn't subscribed.

### 3.13 `S2_TRACKTYPE` (18)

- Caller sets: `ios2_PacketType`.
- Driver: mark this opener as tracking this ether-type; allocate a
  `Sana2PacketTypeStats` slot for it in this opener's state.
- Reply: synchronous.
- Errors: `S2WERR_ALREADY_TRACKED` `[sana2.h:440]` if we're already
  tracking it for this opener; `S2ERR_NO_RESOURCES` if out of memory
  for the tracker slot.
- Roadshow: called for each protocol it cares about (0x0800 IP,
  0x0806 ARP, 0x86dd IPv6). CMD_READ / CMD_WRITE on a type where
  no opener is tracking is **still legal** — TRACKTYPE only controls
  stats visibility. Untracked-type packets are counted only in
  the global `UnknownTypesReceived` counter.

### 3.14 `S2_UNTRACKTYPE` (19)

- Symmetric to TRACKTYPE. Removes the tracker; stats are lost.
- Errors: `S2WERR_NOT_TRACKED` `[sana2.h:441]`.

### 3.15 `S2_GETTYPESTATS` (20)

- Caller sets: `ios2_PacketType`, `ios2_StatData` →
  `Sana2PacketTypeStats` `[sana2.h:177]`.
- Driver: fill in `PacketsSent`, `PacketsReceived`, `BytesSent`,
  `BytesReceived`, `PacketsDropped`.
- Errors: `S2WERR_NOT_TRACKED` if this opener isn't tracking that
  type; `S2WERR_BAD_STATDATA` if the pointer is NULL.

### 3.16 `S2_GETGLOBALSTATS` (22)

- Caller sets: `ios2_StatData` → `Sana2DeviceStats` `[sana2.h:206]`.
- Driver: fill `PacketsReceived`, `PacketsSent`, `BadData` (CRC
  errors from RXERRC), `Overruns` (from MPC, missed packets),
  `UnknownTypesReceived`, `Reconfigurations`, `LastStart`.
- Reply: synchronous.
- Errors: `S2WERR_BAD_STATDATA`.

### 3.17 `S2_GETSPECIALSTATS` (21)

- Caller sets: `ios2_StatData` → `Sana2SpecialStatHeader`
  `[sana2.h:196]` with `RecordCountMax` set. Immediately followed
  in memory by `Sana2SpecialStatRecord[RecordCountMax]`.
- Driver: fill up to `RecordCountMax` records, set
  `RecordCountSupplied`. Each record has a driver-defined `Type` id,
  a `Count`, and a static-string `String` label.
- For virte1000 we'll expose e1000-specific counters: `CRCERRS`,
  `SYMERRS`, `MPC`, `SCC`, `ECOL`, `LATECOL`, `TU`, `RJC`, `RUC`.
- Errors: `S2WERR_BAD_STATDATA`.

### 3.18 `S2_ONEVENT` (23)

- Caller sets: `ios2_WireError` = bitmask of `S2EVENT_*` values to
  wait for `[sana2.h:469-479]`.
- Driver: if the event has already happened for a non-error event
  (e.g. opener asks for `S2EVENT_ONLINE` and we're already online),
  reply immediately. Otherwise enqueue on this opener's event queue.
  When an event fires, reply every queued req whose mask has an
  overlapping bit, setting `ios2_WireError` to the exact subset of
  the mask that fired.
- Reply: async in the general case.
- Errors: `S2WERR_BAD_EVENT` `[sana2.h:448]` for events we don't
  support (e.g. `S2EVENT_CONNECT` / `_DISCONNECT` are PPP-only).
- Roadshow: uses this for link-state tracking. **Important** for
  Roadshow to notice link-down/up transitions.

### 3.19 `CMD_FLUSH` (7, from `<exec/errors.h>`)

- Caller sets: nothing.
- Driver: abort every queued IO across every opener with
  `IOERR_ABORTED` `[wiki: "Aborts all queued I/O requests"]`. Does
  **not** affect a request currently being serviced by hardware.
- Reply: synchronous.

### 3.20 `CMD_CLEAR`

- The spec says most Exec `CMD_*` (CMD_RESET, CMD_START, CMD_STOP,
  CMD_UPDATE, CMD_CLEAR) don't apply to SANA-II; return
  `IOERR_NOCMD`. `[wiki]`

### 3.21 `NSCMD_DEVICEQUERY` (NewStyleDevices 0x4000)

Not a SANA-II command but OS4 expects it. Return the standard NSD
capability response listing all commands we implement. `[wiki: "A
driver must implement the NewStyleDevices NSCMD_DEVICEQUERY command,
in conformance with the NSD specification 1.6 or newer."]`

### 3.22 Extended (0xC000-range) commands

Defined in `[sana2.h:397-405]`:

| Command                     | Value  | Support? |
|-----------------------------|--------|----------|
| `S2_ADDMULTICASTADDRESSES`  | 0xC000 | future — batch add for perf                   |
| `S2_DELMULTICASTADDRESSES`  | 0xC001 | future                                        |
| `S2_GETPEERADDRESS`         | 0xC002 | no — PPP-only, `IOERR_NOCMD`                  |
| `S2_GETDNSADDRESS`          | 0xC003 | no — PPP-only, `IOERR_NOCMD`                  |
| `S2_GETEXTENDEDGLOBALSTATS` | 0xC004 | yes eventually — 64-bit counters              |
| `S2_CONNECT`                | 0xC005 | no                                            |
| `S2_DISCONNECT`             | 0xC006 | no                                            |
| `S2_SAMPLE_THROUGHPUT`      | 0xC007 | maybe                                         |
| `S2_SANA2HOOK`              | 0xC008 | maybe — see §10                               |

For v1 all of these except maybe `S2_GETEXTENDEDGLOBALSTATS` return
`IOERR_NOCMD`. That's correct behaviour.

---

## 4. Event notification (S2_ONEVENT)

Full event set from `[sana2.h:469-479]`:

| Event                    | Bit  | Type      | virte1000 posts when                                    |
|--------------------------|------|-----------|---------------------------------------------------------|
| `S2EVENT_ERROR`          | 0x01 | error     | on any error (aggregate)                                |
| `S2EVENT_TX`             | 0x02 | error     | TX descriptor completes with error bits                 |
| `S2EVENT_RX`             | 0x04 | error     | RX descriptor with CRC / length / other error           |
| `S2EVENT_ONLINE`         | 0x08 | non-error | `S2_ONLINE` handled; PHY link up                        |
| `S2EVENT_OFFLINE`        | 0x10 | non-error | `S2_OFFLINE` handled; PHY link down                     |
| `S2EVENT_BUFF`           | 0x20 | error     | opener's `CopyTo`/`CopyFromBuff` returned failure       |
| `S2EVENT_HARDWARE`       | 0x40 | error     | e1000 RXO (RX overrun), MDAC (management data)          |
| `S2EVENT_SOFTWARE`       | 0x80 | error     | invariant violation, allocation failure                 |
| `S2EVENT_CONFIGCHANGED`  | 0x100| non-error | (nothing user-visible changes at runtime for us)        |
| `S2EVENT_CONNECT`        | 0x200| non-error | not applicable (PPP)                                    |
| `S2EVENT_DISCONNECT`     | 0x400| non-error | not applicable                                          |

Rules from `[wiki]`:
- Non-error events return immediately if already in that state (so a
  freshly-posted `S2_ONEVENT` for `S2EVENT_ONLINE` on an already-online
  unit completes at once).
- One physical event may fire multiple event bits. A hardware error
  during RX should fire `S2EVENT_ERROR | S2EVENT_RX | S2EVENT_HARDWARE`
  simultaneously — a caller asking for any subset gets replied.
- Reply sets `ios2_WireError` to the subset of the requested mask that
  actually fired, so the caller can distinguish which event woke it.
- Unsupported event bits in the caller's mask → return
  `S2WERR_BAD_EVENT` `[sana2.h:448]` without waiting.

Link-state coupling with e1000 PHY: virte1000's LSC (Link Status
Change) interrupt handler must translate to `S2EVENT_ONLINE` /
`S2EVENT_OFFLINE` events, and re-run enough of the config to update
`Sana2DeviceStats.Reconfigurations`.

---

## 5. DeviceQuery response — required fields for Roadshow

Struct from `[sana2.h:130-150]`:

```c
struct Sana2DeviceQuery {
    ULONG SizeAvailable;   /* IN:  bytes available at this ptr */
    ULONG SizeSupplied;    /* OUT: bytes we actually wrote     */
    ULONG DevQueryFormat;  /* OUT: 0 (this version)            */
    ULONG DeviceLevel;     /* OUT: 0 (this version)            */
    UWORD AddrFieldSize;   /* OUT: address width in bits       */
    ULONG MTU;             /* OUT: max cooked packet size      */
    ULONG BPS;             /* OUT: line rate                   */
    ULONG HardwareType;    /* OUT: S2WireType_Ethernet         */
    ULONG RawMTU;          /* OUT: max raw packet size         */
};
```

Note `SizeAvailable` is IN; every other field is OUT.

Ethernet values (82540EM in QEMU):

| Field           | Value              | Rationale                                      |
|-----------------|--------------------|------------------------------------------------|
| `SizeSupplied`  | `sizeof(struct)`   | min(SizeAvailable, our sizeof)                 |
| `DevQueryFormat`| 0                  | mandated                                       |
| `DeviceLevel`   | 0                  | mandated                                       |
| `AddrFieldSize` | 48                 | 6-byte MAC × 8                                 |
| `MTU`           | 1500               | Ethernet II standard                           |
| `BPS`           | 1000000000         | Report 1 Gbps — 82540EM caps out at 1 Gbps and QEMU has no throttle. See §10 for whether Roadshow does anything meaningful with this. |
| `HardwareType`  | 1 (`S2WireType_Ethernet`) | `[sana2.h:158]`                       |
| `RawMTU`        | 1514               | MTU + 14 header bytes; excludes preamble/CRC   |

`SizeAvailable` handling: if the caller's `SizeAvailable` is smaller
than our sizeof, write only the fields that fit and set `SizeSupplied`
to what we wrote. Do **not** fail. The spec is silent on rejecting
undersized queries and the A2065 errata `[wiki]` shows that being
strict here breaks clients.

RawMTU on Ethernet is 1514 = 6+6+2+1500 (dst MAC + src MAC + type +
payload). `[wiki: "that value would be 1514"]`

For a driver that doesn't support raw (we do), set `RawMTU = 0`.

---

## 6. WireError vs io_Error

Every SANA-II reply carries **two** error fields:
- `ios2_Req.io_Error` = the standard Exec error, from the small set
  in `<exec/errors.h>` + `S2ERR_*` `[sana2.h:410-419]`
- `ios2_WireError`    = a much more specific SANA-II code
  `[sana2.h:436-460]`, meaningful only if `io_Error != 0`

Full `S2ERR_*` set:

| Constant             | Val | Meaning                                              |
|----------------------|-----|------------------------------------------------------|
| `S2ERR_NO_ERROR`     | 0   | success                                              |
| `S2ERR_NO_RESOURCES` | 1   | alloc failed, table full, etc.                       |
| `S2ERR_BAD_ARGUMENT` | 3   | garbage in a field                                   |
| `S2ERR_BAD_STATE`    | 4   | command wrong for current state                      |
| `S2ERR_BAD_ADDRESS`  | 5   | src/dst MAC unusable                                 |
| `S2ERR_MTU_EXCEEDED` | 6   | payload > MTU (or RawMTU for raw)                    |
| `S2ERR_NOT_SUPPORTED`| 8   | HW doesn't do this (broadcast, multicast on old HW)  |
| `S2ERR_SOFTWARE`     | 9   | invariant violation in driver / hook returned err    |
| `S2ERR_OUTOFSERVICE` | 10  | unit offline                                         |
| `S2ERR_TX_FAILURE`   | 11  | e1000 gave up (excessive collisions, no link)        |

Plus reused Exec errors: `IOERR_OPENFAIL` (-1), `IOERR_ABORTED` (-2),
`IOERR_NOCMD` (-3), `IOERR_BADLENGTH` (-4), `IOERR_BADADDRESS` (-5),
`IOERR_UNITBUSY` (-6), `IOERR_SELFTEST` (-7). `[sana2.h:422-431]`

Full `S2WERR_*` set:

| Constant                       | Val | Meaning                                        |
|--------------------------------|-----|------------------------------------------------|
| `S2WERR_GENERIC_ERROR`         | 0   | no specific info                               |
| `S2WERR_NOT_CONFIGURED`        | 1   | need `S2_CONFIGINTERFACE` first                |
| `S2WERR_UNIT_ONLINE`           | 2   | command needs offline unit                     |
| `S2WERR_UNIT_OFFLINE`          | 3   | command needs online unit                      |
| `S2WERR_ALREADY_TRACKED`       | 4   | TRACKTYPE on already-tracked type              |
| `S2WERR_NOT_TRACKED`           | 5   | UNTRACKTYPE/GETTYPESTATS on untracked          |
| `S2WERR_BUFF_ERROR`            | 6   | opener's copy hook returned failure            |
| `S2WERR_SRC_ADDRESS`           | 7   | src MAC malformed                              |
| `S2WERR_DST_ADDRESS`           | 8   | dst MAC malformed                              |
| `S2WERR_BAD_BROADCAST`         | 9   | broadcast attempted where inappropriate        |
| `S2WERR_BAD_MULTICAST`         | 10  | multicast MAC malformed                        |
| `S2WERR_MULTICAST_FULL`        | 11  | multicast table full                           |
| `S2WERR_BAD_EVENT`             | 12  | S2_ONEVENT mask has bits we don't support      |
| `S2WERR_BAD_STATDATA`          | 13  | ios2_StatData bad                              |
| — (skipped 14) —               | 14  | reserved / hole in numbering `[sana2.h:450]`   |
| `S2WERR_IS_CONFIGURED`         | 15  | S2_CONFIGINTERFACE called twice                |
| `S2WERR_NULL_POINTER`          | 16  | NULL where non-NULL expected                   |
| `S2WERR_TOO_MANY_RETRIES`      | 17  | TX gave up                                     |
| `S2WERR_RCVREL_HDW_ERR`        | 18  | driver-recoverable HW error                    |
| `S2WERR_UNIT_DISCONNECTED`     | 19  | PPP-style not-connected                        |
| `S2WERR_UNIT_CONNECTED`        | 20  | PPP-style already-connected                    |
| `S2WERR_INVALID_OPTION`        | 21  | PPP option rejected                            |
| `S2WERR_MISSING_OPTION`        | 22  | PPP mandatory option absent                    |
| `S2WERR_AUTHENTICATION_FAILED` | 23  | PPP auth failed                                |
| `S2WERR_FUNCTIONS_MISSING`     | 24  | mandatory copy hook missing from OpenDevice    |

`S2WERR_TOO_MANY_RETIRES` is a documented misspelling alias for
`_RETRIES` `[sana2.h:464]`.

When to set which:

- Successful command: `io_Error = 0`. Do not touch `ios2_WireError`;
  spec doesn't say to clear it, and callers only read it when
  `io_Error != 0`.
- OpenDevice fail with wrong tag list: `io_Error = IOERR_OPENFAIL`
  (-1), `ios2_WireError = S2WERR_FUNCTIONS_MISSING`.
- CMD on offline unit: `io_Error = S2ERR_OUTOFSERVICE`,
  `ios2_WireError = S2WERR_UNIT_OFFLINE`.
- CMD_WRITE too big: `io_Error = S2ERR_MTU_EXCEEDED`,
  `ios2_WireError = 0` (no more specific wire info needed).
- Second S2_CONFIGINTERFACE: `io_Error = S2ERR_BAD_STATE`,
  `ios2_WireError = S2WERR_IS_CONFIGURED`.
- Unknown command: `io_Error = IOERR_NOCMD`, `ios2_WireError = 0`.

---

## 7. Buffer-management tag list — how to actually parse it in Open

Concrete sketch of what the driver's Open method does with the caller's
tag list:

```c
LONG virte1000_open(struct IOSana2Req *req, LONG unitno, ULONG flags)
{
    struct V1000Unit   *unit    = find_unit(unitno);
    struct V1000Opener *opener;
    struct TagItem     *tagptr  = (struct TagItem *)req->ios2_BufferManagement;
    struct TagItem     *ti;

    if (!unit) goto fail_openfail;

    /* Enforce exclusivity */
    if ((flags & SANA2OPF_MINE) && !IsListEmpty(&unit->openers))
        goto fail_unitbusy;
    if (unit->has_exclusive_opener)
        goto fail_unitbusy;
    if ((flags & SANA2OPF_PROM) && !(flags & SANA2OPF_MINE))
        goto fail_openfail_null_ptr;   /* PROM requires MINE */

    opener = AllocVecTags(sizeof(*opener),
                          AVT_Type,      MEMF_SHARED,
                          AVT_ClearWithValue, 0,
                          TAG_END);
    if (!opener) goto fail_no_resources;

    /* Resolve the required hooks — both must be present and non-NULL. */
    opener->copy_to_buff   = (struct Hook *)IUtility->GetTagData(
        S2_CopyToBuff,   0, tagptr);
    opener->copy_from_buff = (struct Hook *)IUtility->GetTagData(
        S2_CopyFromBuff, 0, tagptr);

    if (!opener->copy_to_buff || !opener->copy_from_buff) {
        req->ios2_WireError = S2WERR_FUNCTIONS_MISSING;
        FreeVec(opener);
        goto fail_openfail;
    }

    /* Optional hooks */
    opener->copy_to_buff_16   = (struct Hook *)IUtility->GetTagData(
        S2_CopyToBuff16,      0, tagptr);
    opener->copy_from_buff_16 = (struct Hook *)IUtility->GetTagData(
        S2_CopyFromBuff16,    0, tagptr);
    opener->copy_to_buff_32   = (struct Hook *)IUtility->GetTagData(
        S2_CopyToBuff32,      0, tagptr);
    opener->copy_from_buff_32 = (struct Hook *)IUtility->GetTagData(
        S2_CopyFromBuff32,    0, tagptr);
    opener->dma_to_32   = (struct Hook *)IUtility->GetTagData(
        S2_DMACopyToBuff32,   0, tagptr);
    opener->dma_from_32 = (struct Hook *)IUtility->GetTagData(
        S2_DMACopyFromBuff32, 0, tagptr);
    opener->dma_to_64   = (struct Hook *)IUtility->GetTagData(
        S2_DMACopyToBuff64,   0, tagptr);
    opener->dma_from_64 = (struct Hook *)IUtility->GetTagData(
        S2_DMACopyFromBuff64, 0, tagptr);
    opener->packet_filter = (struct Hook *)IUtility->GetTagData(
        S2_PacketFilter,      0, tagptr);
    opener->log_hook      = (struct Hook *)IUtility->GetTagData(
        S2_Log,               0, tagptr);

    opener->unit       = unit;
    opener->open_flags = flags;
    NEWLIST(&opener->rx_queue);       /* CMD_READ, keyed by type */
    NEWLIST(&opener->rx_orphan_queue);/* S2_READORPHAN */
    NEWLIST(&opener->event_queue);    /* S2_ONEVENT */
    NEWLIST(&opener->tracked_types);

    /* Link into unit */
    ObtainSemaphore(&unit->lock);
    AddTail(&unit->openers, (struct Node *)&opener->node);
    if (flags & SANA2OPF_MINE) unit->has_exclusive_opener = TRUE;
    ReleaseSemaphore(&unit->lock);

    /* Overwrite the tag list ptr with the magic cookie. */
    req->ios2_BufferManagement = opener;
    req->ios2_Req.io_Device    = &v1000_base->device;
    req->ios2_Req.io_Unit      = (struct Unit *)unit;
    req->ios2_Req.io_Error     = 0;
    return 0;
}
```

The invocation side, when we need to move packet data:

```c
/* On RX, once we've dequeued a matching CMD_READ (`req`) belonging to
 * `opener`, invoke CopyToBuff to move from our RX descriptor buffer
 * into the opener's ios2_Data. */
struct SANA2CopyHookMsg msg = {
    .schm_Method  = S2_CopyToBuff,
    .schm_MsgSize = sizeof msg,
    .schm_To      = req->ios2_Data,   /* opener's abstract buffer   */
    .schm_From    = rx_payload,       /* our descriptor payload     */
    .schm_Size    = rx_len,
};
BOOL ok = (BOOL)IUtility->CallHookPkt(opener->copy_to_buff, req, &msg);
if (!ok) {
    req->ios2_Req.io_Error = S2ERR_SOFTWARE;
    req->ios2_WireError    = S2WERR_BUFF_ERROR;
    fire_event(opener, S2EVENT_BUFF | S2EVENT_ERROR);
}
```

---

## 8. Per-opener list — data-structure sketch

```c
struct V1000Opener {
    struct MinNode  node;              /* on unit->openers            */
    struct V1000Unit *unit;
    ULONG           open_flags;        /* SANA2OPF_MINE / _PROM       */

    /* Copy hooks — all pointers to struct Hook, invoked via
       IUtility->CallHookPkt(). NULL means "not supplied". Copy hooks
       marked mandatory are guaranteed non-NULL after Open. */
    struct Hook    *copy_to_buff;       /* mandatory */
    struct Hook    *copy_from_buff;     /* mandatory */
    struct Hook    *copy_to_buff_16;
    struct Hook    *copy_from_buff_16;
    struct Hook    *copy_to_buff_32;
    struct Hook    *copy_from_buff_32;
    struct Hook    *dma_to_32;
    struct Hook    *dma_from_32;
    struct Hook    *dma_to_64;
    struct Hook    *dma_from_64;
    struct Hook    *packet_filter;
    struct Hook    *log_hook;

    /* Per-opener queues. All manipulated under unit->lock. */
    struct List     rx_queue;           /* CMD_READ, mixed types      */
    struct List     rx_orphan_queue;    /* S2_READORPHAN              */
    struct List     event_queue;        /* S2_ONEVENT                 */
    struct List     tracked_types;      /* list of V1000TypeStat      */
};

struct V1000TypeStat {
    struct MinNode  node;
    ULONG           packet_type;        /* ethertype                  */
    struct Sana2PacketTypeStats stats;
};

struct V1000Unit {
    struct Unit     pub;                /* Exec Unit                   */
    struct SignalSemaphore lock;
    struct List     openers;            /* V1000Opener nodes           */

    BOOL            has_exclusive_opener;
    BOOL            configured;
    BOOL            online;
    UBYTE           mac[6];             /* current                     */
    UBYTE           factory_mac[6];     /* from EEPROM                 */

    /* Global stats — merged across openers */
    struct Sana2DeviceStats stats;
    struct TimeVal  last_start;

    /* Multicast address table with refcount */
    struct MulticastEntry {
        struct MinNode node;
        UBYTE          addr[6];
        ULONG          refcount;
    }              *mc_table;
    ULONG           mc_count;

    /* Hardware state */
    struct e1000_regs *regs;            /* BAR0 remap                  */
    struct RxDesc  *rx_ring;
    struct TxDesc  *tx_ring;
    /* ...irq stuff, TX/RX bottom-half signals, etc. */
};
```

Why the lists live where they do:

- `rx_queue` is per-opener because CMD_READ is dispatched to the opener
  that owns it. Multiple openers can each hold reads for the same
  ether-type — the driver's RX path must broadcast a match to all of
  them.
- `event_queue` is per-opener because events are replies to that
  opener's `S2_ONEVENT`.
- Tracked-type stats are per-opener because `S2_TRACKTYPE` is a
  per-opener concept.
- Multicast table is shared (unit-level) with refcount, because the
  hardware filter is single. Refcount ensures we don't disable a
  filter that another opener still needs.
- Global stats are per-unit (the wire is shared).

---

## 9. Roadshow / bsdsocket binding — minimum to make it bind

The spec does not lay this out step-by-step (Roadshow is a separate
product, not standard SANA-II). Based on the general SANA-II flow
described in `[wiki]` and inferring from `bsdsocket.library`
conventions:

1. `OpenDevice("virte1000.device", 0, req, 0)` with a well-formed
   `ios2_BufferManagement` tag list.
2. `NSCMD_DEVICEQUERY` to check we're an NSD-capable device and see
   which commands we implement.
3. `S2_DEVICEQUERY` — reads MTU, HardwareType, RawMTU.
4. `S2_GETSTATIONADDRESS` — grabs factory MAC.
5. `S2_CONFIGINTERFACE` — sets the operating MAC (usually to the
   factory MAC unless the user overrode it in Prefs/Roadshow).
6. `S2_ONLINE` — brings the interface up.
7. `S2_ADDMULTICASTADDRESS` × N for the protocol multicasts (IPv4
   224.0.0.1 / 224.0.0.22, IPv6 solicited-node, IPv6 all-nodes
   ff02::1).
8. `S2_TRACKTYPE` × N for the ether-types Roadshow cares about
   (0x0800 IP, 0x0806 ARP, 0x86dd IPv6).
9. Post many concurrent `CMD_READ` requests per tracked type
   `[wiki: "you should have multiple CMD_READ requests pending"]`.
10. Post an `S2_ONEVENT` mask covering `S2EVENT_ONLINE |
    S2EVENT_OFFLINE | S2EVENT_ERROR` for link-state tracking.

Bind aborts if:
- OpenDevice returns nonzero
- `S2_DEVICEQUERY` fails or reports non-Ethernet
- `S2_CONFIGINTERFACE` fails
- `S2_ONLINE` fails

Bind warns but continues if:
- `S2_ADDMULTICASTADDRESS` fails (some protocols won't work)
- `S2_TRACKTYPE` fails (stats won't work, but IO does)

**Minimum surface for a "hello world" bind:** DEVICEQUERY,
GETSTATIONADDRESS, CONFIGINTERFACE, ONLINE, CMD_READ, CMD_WRITE,
CMD_FLUSH, S2_OFFLINE, S2_ONEVENT (even if stubbed to reply-immediate
for ONLINE). Everything else can start as `IOERR_NOCMD` and get filled
in later without Roadshow refusing to attach.

---

## 10. Open questions

Genuine unknowns. **Do not silently answer these** later — read a
real driver's source (`a1000BootLan`, `rtl8139.device`, an open
`etherlink3.device`) to confirm.

1. **`S2_SANA2HOOK` (0xC008)** `[sana2.h:405]` — the wiki calls this
   "proposed" and doesn't fully define it. The intent seems to be that
   an opener can install a *single* hook that handles all callback
   methods (multiplexed via `shm_Method` in `SANA2HookMsg`), replacing
   the per-tag hooks. Not clear whether Roadshow uses it. **Action:**
   check a live driver source.

2. **`BPS` field value** — spec says "line rate". For QEMU-emulated
   e1000 there is no meaningful line rate. Report 1e9 for now, but
   Roadshow may use BPS to compute retry timers; if we get inflated
   RTOs, revisit.

3. **Copy-hook interrupt safety** — spec is silent on whether
   `CopyTo`/`CopyFromBuff` may be called from interrupt context. I'm
   choosing "no — always run through a device Task" for safety. If
   this hurts throughput badly (extra Signal + task-switch per
   packet), revisit.

4. **Alignment / DMA hook usage by Roadshow** — do any current OS4
   protocol stacks actually pass `S2_DMACopyToBuff32/64` tags? If so
   we can skip a memcpy per RX packet by DMA-ing straight into their
   mbufs (or whatever). If not, don't bother implementing.

5. **`ios2_PacketType` field size** — sana2.h declares it `ULONG`
   `[sana2.h:78]`, but Ethernet types are 16-bit. For packet types
   < 1500 the spec says "802.3 framing" — do we need to support 802.3
   LLC/SNAP? Roadshow probably doesn't care but we should not crash.

6. **`S2_MULTICASTINFO`** — mentioned in the caller's outline but I
   see no such command in `[sana2.h]` or the wiki. Skipping.

7. **`SANA2OPF_MINE` behaviour when count = 1 and non-MINE openers
   exist** — spec says MINE requires exclusive access. Does an
   already-open (non-MINE) unit reject a subsequent MINE open? I
   think yes. Verify with driver source.

8. **`Sana2SpecialStatRecord.String`** — is this string OS4-locale-
   aware, or plain English only? For now, English only, but flag if
   we care later.

9. **`CMD_FLUSH` interaction with in-flight hardware IO** — spec says
   FLUSH aborts queued requests but not the "currently servicing"
   one. For a `CMD_WRITE` we've already handed to the e1000 TX ring,
   is that "queued" or "servicing"? I'll treat "on the TX ring" as
   "servicing" and only abort requests that haven't hit the ring yet.

10. **What Roadshow does on `S2EVENT_OFFLINE`** — does it re-drive
    us with `S2_OFFLINE` (redundant) and then wait for `_ONLINE`?
    Or does it require us to keep the interface configured but stop
    RX/TX? Test on live sam460ex.

11. **The `Sana2DeviceQuery.SizeAvailable` is a UBYTE-count trap** —
    early implementations (per A2065 errata) reject queries where
    the struct is "too big". We shouldn't hit this because we're the
    driver, not the caller — but if we ever query another SANA-II
    driver ourselves, pass at most 30 bytes.

12. **`ios2_Data` when `SANA2IOB_RAW` is set** — for RAW writes, does
    `ios2_Data` include the FCS or not? Spec says "entire physical
    frame ... unmodified", but Ethernet FCS is a hardware artifact.
    I'll assume "no FCS in `ios2_Data`; hardware computes it".
    Confirm empirically.

---

## Appendix A. `IOSana2Req` field map (quick reference)

From `[sana2.h:73-85]`:

```
ios2_Req                struct IORequest — standard Exec IO
  io_Message              struct Message — reply port etc.
  io_Device               set by OpenDevice, do not touch
  io_Unit                 set by OpenDevice, do not touch
  io_Command              caller: SANA-II command constant
  io_Flags                caller: SANA2IOF_RAW | _BCAST | _MCAST | _QUICK
  io_Error                driver reply: S2ERR_* or IOERR_*
ios2_WireError          driver reply: S2WERR_* (only if io_Error != 0)
ios2_PacketType         caller/driver: ether-type (RX/TX)
ios2_SrcAddr[16]        caller/driver: source MAC (or factory MAC)
ios2_DstAddr[16]        caller: dest MAC (TX); driver: dest MAC (RX)
ios2_DataLength         caller: TX size / driver: RX size
ios2_Data               caller: opener abstract buffer (opaque to us)
ios2_StatData           caller: pointer to stat struct for stat cmds
ios2_BufferManagement   IN OpenDevice: tag list; OUT: magic cookie
```

Address arrays are `SANA2_MAX_ADDR_BYTES = 16` bytes wide
`[sana2.h:70-71]` for future-proofing, but for Ethernet only the
first 6 are used.

---

## Appendix B. Header vs wiki numeric cross-check

All commands checked against `[sana2.h:373-405]`:

| Command                     | Header | Wiki | OK? |
|-----------------------------|--------|------|-----|
| `S2_DEVICEQUERY`            | 9      | 9    | ✓   |
| `S2_GETSTATIONADDRESS`      | 10     | 10   | ✓   |
| `S2_CONFIGINTERFACE`        | 11     | 11   | ✓   |
| (no #12)                    | —      | —    | ✓ hole intentional |
| (no #13)                    | —      | —    | ✓                  |
| `S2_ADDMULTICASTADDRESS`    | 14     | 14   | ✓   |
| `S2_DELMULTICASTADDRESS`    | 15     | 15   | ✓   |
| `S2_MULTICAST`              | 16     | 16   | ✓   |
| `S2_BROADCAST`              | 17     | 17   | ✓   |
| `S2_TRACKTYPE`              | 18     | 18   | ✓   |
| `S2_UNTRACKTYPE`            | 19     | 19   | ✓   |
| `S2_GETTYPESTATS`           | 20     | 20   | ✓   |
| `S2_GETSPECIALSTATS`        | 21     | 21   | ✓   |
| `S2_GETGLOBALSTATS`         | 22     | 22   | ✓   |
| `S2_ONEVENT`                | 23     | 23   | ✓   |
| `S2_READORPHAN`             | 24     | 24   | ✓   |
| `S2_ONLINE`                 | 25     | 25   | ✓   |
| `S2_OFFLINE`                | 26     | 26   | ✓   |

Note the numbering holes at 12 and 13 (between CONFIGINTERFACE and
ADDMULTICASTADDRESS) — the header simply skips them and the wiki
doesn't define anything there. Not an error, just history.

All error / event constants matched header exactly.

**One resolved ambiguity:** `[sana2.h:450]` explicitly documents
"THERE IS NO WIRE ERROR CODE 14" — the wiki also skips 14, so both
sources agree.
