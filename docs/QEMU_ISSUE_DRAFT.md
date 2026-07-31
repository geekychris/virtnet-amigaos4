# DRAFT: QEMU sam460ex PCI DMA drops guest→device writes on descriptor-based NICs

**QEMU version**: reproduced on 11.0.1, 11.0.3, and v11.1.0-rc2 (`QEMU emulator version 11.0.92 (v11.1.0-rc2)`, built from source)
**Host**: macOS 14 arm64 (Homebrew build)
**Guest**: AmigaOS 4.1 FE on sam460ex
**Category**: PCI DMA / machine model

## Symptom

On the `sam460ex` machine, guest-driven TX packets never reach the
netdev's `filter-dump` pcap, on both `virtio-net-pci` (legacy) and
`e1000-82540em`. Only `rtl8139` — which puts the DMA address
directly in a NIC register (TSAD0-3), no descriptor indirection —
works.

`filter-dump` on rtl8139 shows real packet content. On virtio-net-pci
and e1000 it shows the correct number of zero-filled packets — the
NIC "receives" a TX request but sends only zero bytes.

## Root cause investigation

Using HMP `xp` (physical peek) to bypass every guest-side layer
(CPU MMU, cache, our proprietary bridge inspection tool):

1. **CPU writes DO reach guest-physical RAM at the exact address the
   driver publishes as descriptor `addr_lo`.**

   For virtio-net-pci:
   ```
   (qemu) xp /24bx 0x0283A820
   0283a820: 00 00 00 00 00 00 00 00       <- virtio_net_hdr[0..7]
   0283a828: 00 00 ff ff ff ff ff ff       <- hdr[8..9] + eth dst = broadcast
   0283a830: 00 00 00 00 00 00 08 06       <- eth src + Ethertype ARP
   ```
   For e1000:
   ```
   (qemu) xp /24bx 0x007CD340
   007cd340: ff ff ff ff ff ff 52 54       <- eth dst=broadcast, src=52:54:..
   007cd348: 00 12 34 57 08 06 00 00       <- src cont, type ARP, ARP body
   ```
   Both are well-formed frames the driver built.

2. **QEMU DMA fetches zeros from the same address.** The netdev pcap
   for the same test shows the correct-length frame but bytes are
   all zero.

3. **QEMU trace confirms the TX request was noticed.** For
   virtio-net-pci, `virtqueue_pop` fires with `out_num=1` immediately
   after the guest bumps the doorbell. So QEMU DID read the
   descriptor (from RAM at `tx_vring[0]`) and DID parse `addr_lo`
   correctly (else "bogus descriptor" instead of `pop`). The issue
   is only with the FOLLOW-UP DMA that reads the payload buffer.

## Hypothesis

On sam460ex, PCI DMA works for reads that go through the QEMU device
model's initial register/descriptor-fetch path, but subsequent
scatter-gather DMA fetches (payload → NIC egress buffer) return
zeros. Possibly a stale mapping in `hw/ppc/ppc4xx_pci.c`'s inbound
window, or a virtio-net-specific bug in how the legacy transport
resolves `iov` addresses on a BE guest.

Since rtl8139 has no scatter-gather (DMA addr is written into a NIC
register directly, not into a descriptor that the NIC re-reads), it's
unaffected.

## Reproduction

Prerequisites: AmigaOS 4.1 FE guest, an SDK-cross-compiled driver
that publishes descriptor addr = a MEMF_KICK-allocated buffer's
GetDMAList-returned physical address.

Minimal QEMU invocation:
```
qemu-system-ppc -machine sam460ex -m 1024 \
    -drive file=amigaos4-system.hdf,format=raw,if=ide,index=0 \
    -netdev user,id=n1 -device e1000-82540em,netdev=n1 \
    -object filter-dump,id=dump,netdev=n1,file=/tmp/e1000.pcap \
    -monitor tcp:127.0.0.1:2348,server,nowait
```

Steps:
1. In guest, load driver + open device + send a broadcast ARP frame
   via SANA-II `CMD_WRITE`.
2. Note the driver's TX-buffer physical address from its init log.
3. On host: `(echo "xp /48bx 0x<phys>"; echo q) | nc localhost 2348`
   → shows the real ARP bytes.
4. `tcpdump -r /tmp/e1000.pcap -X` → shows zero-filled packet.

## Impact

Blocks driver development for any modern PCI NIC on sam460ex. Only
rtl8139 is usable.

## Files/lines to investigate

- `hw/ppc/sam460ex.c` — PCI host bridge / DMA window setup
- `hw/pci-host/ppc4xx_pci.c` — inbound DMA window mapping
- `hw/net/virtio-net.c` + `hw/virtio/virtio.c` — descriptor-payload DMA
- `hw/net/e1000.c` — TX descriptor buffer fetch

## Environment

- QEMU 11.0.1, 11.0.3 (Homebrew bottles), v11.1.0-rc2 (source build) —
  all reproduce the same symptom
- Host: macOS 14 arm64, Homebrew
- Guest: AmigaOS 4.1 FE, PPC 460EX BE
- Notably, `hw/net/virtio-net.c` commit df12999c ("Protect from DMA
  re-entrancy bugs", 2026-07-23) IS in v11.1.0-rc2 but does not
  resolve this bug — so this is a different DMA path than the
  re-entrancy fix addressed.
