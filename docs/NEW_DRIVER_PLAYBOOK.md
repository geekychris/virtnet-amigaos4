# Playbook: fork this repo for a new virtio-* driver

This project is set up so you can copy it as a starting point for
other virtio device drivers on AmigaOS 4 — virtio-blk, virtio-console,
virtio-rng, virtio-scsi, etc. About 90% of the code carries over
unchanged; the delta is the device-specific top half (SANA-II for
network, exec.device / trackdisk-style for block, etc.) and the
device-config field decoders.

**Value proposition**: someone writing an AmigaOS 4 driver for a
device that's never had one takes a few weeks. Someone forking this
project can usually get to a booting, feature-negotiated, DRIVER_OK
device in an evening.

## What's reusable

### From `src/virtio.c` (100% reusable)

- `vio_read8/16/32` + `vio_write8/16/32` — I/O port wrappers over
  IPCI->InByte/OutByte/etc.
- `virtio_reset_and_ack()` — the RESET → ACK → DRIVER handshake
- `virtio_negotiate_features()` — feature intersection
- `virtio_queue_num()` — per-queue depth discovery
- `virtio_set_queue_pfn()` — PFN publish
- `virtio_driver_ok()` — DRIVER_OK flip

### From `include/virtio.h` (100% reusable)

- Register offsets (VIRTIO_PCI_*)
- Status bits, ISR bits
- Virtqueue struct definitions (`vring_desc`, `vring_avail_header`,
  `vring_used_header`, `vring_used_elem`)
- `VRING_TOTAL_BYTES(num)` layout macro
- Generic feature bits (VIRTIO_F_*)

### From `src/device.c` (partially reusable)

- The Amiga library-base + resident-tag boilerplate — `_manager_Init`,
  `_manager_Open`, `_manager_Close`, `_manager_Expunge` — is standard
  OS4 device pattern; keep as-is except for device name and per-opener
  struct.
- The unit-task pattern (`vn_task_start` / `vn_task_body` etc.) —
  most drivers want a background task to dispatch async I/O. Keep.
- `TRACEF` / `LOGF` file-based logging via `dos.library`. Keep.
- `virte1000-init.log` / `RAM:` diagnostic paths — rename per device.

### From `include/virtnet.h` (partially reusable)

- The `VirtnetBase` outer struct fields (Device base, ExecIFace,
  IPCI, pciDevice, unit task fields, DBG counter fields) — keep.
- Anything with `sana2_` or `copy_to_buff` or `read_queue` /
  `event_queue` — network-specific; DELETE if you're writing a
  block driver or a serial driver.

## What you need to replace

### 1. PCI vendor/device IDs

In `include/virtnet.h` (or wherever you moved it):
```c
#define VIRTIO_PCI_VENDOR            0x1AF4    /* stays */
#define VIRTIO_NET_PCI_DEVICE        0x1000    /* rename + change */
```

Legacy virtio PCI device IDs (from spec):
- 0x1000 = network
- 0x1001 = block
- 0x1002 = balloon
- 0x1003 = console
- 0x1004 = scsi
- 0x1005 = entropy source (rng)
- 0x1009 = 9P filesystem (virtfs)
- 0x1042 = block (modern; add 0x1040 offset from device-type-id-1)
- 0x1043 = console (modern)
- 0x1044 = rng (modern)
- ... and so on. See virtio spec chapter 4.

### 2. Device-specific feature bits

`include/virtio.h` currently lists `VIRTIO_NET_F_*`. Replace with the
device's own feature namespace (`VIRTIO_BLK_F_*`, `VIRTIO_RNG_F_*`,
etc.). The spec chapter for each device type lists them.

### 3. Device-specific config field layout

For virtio-net we read MAC (6 bytes at cfg offset 0) and link status
(2 bytes at offset 6). Block/console/rng each have different
layouts. Rewrite the "read device config" section of Init.

**Remember the endianness gotcha**: device-config on legacy virtio is
guest-native. On PPC BE, use `vio_read8` + explicit BE-decode for
multi-byte fields. `vio_read16` / `vio_read32` on the standard
registers is fine (those ARE swapped by IPCI).

### 4. Queue set

virtio-net has queues {RX=0, TX=1, [CTRL=2]}. Other devices have
their own layout:

- virtio-blk: single request queue
- virtio-rng: single entropy-in queue
- virtio-console: {RX=0, TX=1, control-rx=2, control-tx=3, ...}
- virtio-scsi: {control=0, event=1, request[N]=2..2+N-1}

Adjust `VirtnetBase` fields (rx_vring / tx_vring) to the right set
for your device.

### 5. TX/RX bottom halves

Everything network-specific in the dispatch layer (S2_DEVICEQUERY,
S2_CONFIGINTERFACE, S2_ONEVENT, CMD_READ/WRITE, S2_BROADCAST, the
SANA2HOOK path, opener list, copy hooks) is SANA-II — irrelevant for
non-network drivers. Delete and replace with the device's own
command set:

- **virtio-blk**: implement Amiga `trackdisk.device` command set —
  CMD_READ / CMD_WRITE with byte offsets, TD_MOTOR, TD_SEEK,
  TD_FORMAT (opt), TD_GETGEOMETRY.
- **virtio-console**: implement the AmigaDOS char-device pattern
  (basically stream in / stream out through Read / Write).
- **virtio-rng**: expose as an entropy source; simplest — just a
  read-only stream on an on-demand queue.

The **queue mechanics** (put descriptor on avail, kick, wait for
used-idx bump, reclaim) are the same regardless of device. Extract
those into virtio.c helpers you can share across drivers.

### 6. Roadshow / mount config

Only relevant if this is a network driver. For a block driver:
create a `DEVS:DOSDrivers/` entry that points at your `.device`.
For virtio-rng: expose it via a wrapper library (`entropy.library`?)
that userland programs can OpenLibrary to.

## Renaming checklist

If forking as `virtio_blk` (say), run something like:

```sh
cp -R virtio_net virtio_blk
cd virtio_blk
rm -rf build .git

# Bulk rename identifiers. NOTE: on macOS use gsed or adjust for BSD sed.
find include src tests -type f \( -name '*.c' -o -name '*.h' \) -print0 | \
    while IFS= read -r -d '' f; do
        sed -i.bak \
            -e 's/virtnet/virtblk/g' \
            -e 's/Virtnet/Virtblk/g' \
            -e 's/VIRTNET/VIRTBLK/g' \
            -e 's/vn_/vb_/g' \
            -e 's/VN_/VB_/g' \
            -e 's/VIRTIO_NET_PCI_DEVICE/VIRTIO_BLK_PCI_DEVICE/g' \
            -e 's/VIRTIO_NET_F_/VIRTIO_BLK_F_/g' \
            "$f"
        rm -f "$f.bak"
    done

# Update the top of include/version.h and Makefile targets.
```

Then re-init the git repo and start writing block-specific dispatch
in `src/device.c`.

## Getting from scaffold to first-boot

Rough sequence, ~1-3 days each phase:

1. **Fork + rename** (hours). Verify build passes.
2. **Change PCI IDs**, verify device shows up in your Init log's
   `FindDeviceTags` output.
3. **Read device-specific config fields**, verify sane values (MAC,
   link status, block-device capacity, whatever).
4. **Allocate + PFN + populate virtqueues**. Confirm via QEMU
   monitor (`info pci`) that PFN registers hold your buffer address.
5. **DRIVER_OK + first descriptor round-trip**. For block: send an
   IDENTIFY-like request, get used-ring bump, decode the response.
6. **Wire the device-specific dispatch commands** (SANA-II for
   network, trackdisk for block, etc.).

Steps 1-3 are usually a single evening; 4-6 is where the real work
happens.

## Contributing back

If you write another virtio-* driver on top of this scaffold,
please add a link in the sibling-projects section of the top-level
README. The more OS4 virtio drivers exist, the stronger the case
for others to switch away from emulated-hardware drivers.
