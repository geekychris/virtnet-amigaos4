/*
 * virtio.c — virtio 0.9.5 legacy PCI transport layer for virtnet.device.
 *
 * Responsibilities:
 *   - Read/write BAR0 I/O port registers (device features, status, queue
 *     configuration, notify).
 *   - Reset the device, walk the init sequence, negotiate features.
 *   - Set up virtqueues in guest RAM and tell the device their PFN.
 *   - Kick a queue (notify device that avail-ring advanced).
 *
 * All accessors take the ExecIFace + a Virtnet base pointer to reach
 * the PCIDevice (via base->pciDevice) and the io_base (BAR0 masked).
 *
 * Guest-native endianness: legacy virtio uses guest endian for in-RAM
 * descriptor / avail / used fields. On PPC BE, our stw/lwz work
 * natively — no swap. The BAR0 PCI I/O accesses ARE little-endian by
 * PCI convention; IPCI->InByte/OutByte etc. handle that transparently.
 */

#include "virtnet.h"
#include "virtio.h"

#include <exec/exec.h>
#include <exec/types.h>

/* ---------- Low-level register accessors ---------- */

static UBYTE vio_read8(struct VirtnetBase *base, ULONG offset)
{
    return base->pciDevice->InByte(base->io_base + offset);
}
static UWORD vio_read16(struct VirtnetBase *base, ULONG offset)
{
    return base->pciDevice->InWord(base->io_base + offset);
}
static ULONG vio_read32(struct VirtnetBase *base, ULONG offset)
{
    return base->pciDevice->InLong(base->io_base + offset);
}
static void vio_write8(struct VirtnetBase *base, ULONG offset, UBYTE val)
{
    base->pciDevice->OutByte(base->io_base + offset, val);
}
static void vio_write16(struct VirtnetBase *base, ULONG offset, UWORD val)
{
    base->pciDevice->OutWord(base->io_base + offset, val);
}
static void vio_write32(struct VirtnetBase *base, ULONG offset, ULONG val)
{
    base->pciDevice->OutLong(base->io_base + offset, val);
}

/* Public wrappers used from device.c (declared in virtnet.h). */
UBYTE virtio_read_status(struct VirtnetBase *base)
{
    return vio_read8(base, VIRTIO_PCI_STATUS);
}
void virtio_write_status(struct VirtnetBase *base, UBYTE val)
{
    vio_write8(base, VIRTIO_PCI_STATUS, val);
}
UBYTE virtio_read_isr(struct VirtnetBase *base)
{
    return vio_read8(base, VIRTIO_PCI_ISR);
}
UBYTE virtio_read_dev_cfg8(struct VirtnetBase *base, ULONG offset)
{
    return vio_read8(base, VIRTIO_PCI_DEV_CFG_OFFSET + offset);
}
UWORD virtio_read_dev_cfg16(struct VirtnetBase *base, ULONG offset)
{
    return vio_read16(base, VIRTIO_PCI_DEV_CFG_OFFSET + offset);
}
void virtio_notify_queue(struct VirtnetBase *base, UWORD queue_num)
{
    vio_write16(base, VIRTIO_PCI_QUEUE_NOTIFY, queue_num);
}

/* ---------- Init sequence (spec §3.1.1) ---------- */

/* Reset the device and walk the ACKNOWLEDGE → DRIVER handshake.
 * Returns TRUE on success, FALSE if the device signals FAILED at any
 * point. Caller has already set base->io_base from BAR0. */
BOOL virtio_reset_and_ack(struct VirtnetBase *base)
{
    /* Write 0 to STATUS to trigger reset. Then poll until it reads 0
     * (device confirms reset complete). Spec says the device MUST
     * complete reset promptly but doesn't bound it; a small spin
     * is safe here since we're pre-online. */
    vio_write8(base, VIRTIO_PCI_STATUS, 0);
    int spin = 100;
    while (spin-- > 0 && vio_read8(base, VIRTIO_PCI_STATUS) != 0) {
        /* memory barrier is implicit in the I/O port cycle */
    }
    if (vio_read8(base, VIRTIO_PCI_STATUS) != 0) return FALSE;

    /* ACKNOWLEDGE + DRIVER: driver has seen the device and knows how
     * to drive it. FEATURES_OK comes after feature negotiation. */
    vio_write8(base, VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    vio_write8(base, VIRTIO_PCI_STATUS,
               VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    UBYTE st = vio_read8(base, VIRTIO_PCI_STATUS);
    if (st & VIRTIO_STATUS_FAILED) return FALSE;
    return TRUE;
}

/* Read device-offered features, intersect with what we support, write
 * that back as driver features. Store both on the base for logging. */
void virtio_negotiate_features(struct VirtnetBase *base, ULONG driver_wanted)
{
    ULONG dev_feat = vio_read32(base, VIRTIO_PCI_HOST_FEATURES);
    ULONG accepted = dev_feat & driver_wanted;
    vio_write32(base, VIRTIO_PCI_GUEST_FEATURES, accepted);
    base->device_features = dev_feat;
    base->driver_features = accepted;
}

/* Read the size of a specific queue by selecting it. Zero return means
 * the queue is not implemented at this index (per spec — "if the device
 * doesn't support the queue, it returns 0"). */
UWORD virtio_queue_num(struct VirtnetBase *base, UWORD queue_sel)
{
    vio_write16(base, VIRTIO_PCI_QUEUE_SEL, queue_sel);
    return vio_read16(base, VIRTIO_PCI_QUEUE_NUM);
}

/* Publish a queue's page-frame number to the device. The queue's
 * physical base MUST be 4096-byte aligned (spec requires PFN
 * granularity = page). Caller has QUEUE_SEL set from a prior
 * virtio_queue_num call. */
void virtio_set_queue_pfn(struct VirtnetBase *base, UWORD queue_sel, ULONG phys_addr)
{
    vio_write16(base, VIRTIO_PCI_QUEUE_SEL, queue_sel);
    vio_write32(base, VIRTIO_PCI_QUEUE_PFN, phys_addr / VRING_ALIGN);
}

/* Ring the doorbell for a fully-configured driver. Device may start
 * processing our queues after this returns. */
void virtio_driver_ok(struct VirtnetBase *base)
{
    UBYTE st = vio_read8(base, VIRTIO_PCI_STATUS);
    vio_write8(base, VIRTIO_PCI_STATUS, st | VIRTIO_STATUS_FEATURES_OK);
    st = vio_read8(base, VIRTIO_PCI_STATUS);
    vio_write8(base, VIRTIO_PCI_STATUS, st | VIRTIO_STATUS_DRIVER_OK);
}
