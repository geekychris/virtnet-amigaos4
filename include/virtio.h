/*
 * virtio.h — virtio 0.9.5 legacy PCI transport definitions.
 *
 * Reference: OASIS Virtio 0.9.5 spec (which QEMU's transitional
 * virtio-net-pci implements when we access it via the I/O port BAR).
 * Modern (1.0+) uses PCI capabilities + MMIO — we're skipping that
 * layer for now because legacy is universally supported and simpler.
 *
 * Endianness note (important on PPC BE): legacy virtio uses
 * GUEST-NATIVE endianness for in-memory descriptor / ring fields.
 * So on our big-endian PPC 460EX target, all uint16/uint32 fields
 * in the virtqueue structs stay BE and QEMU reads them BE.
 * NO byte-swap dance like we had with e1000 descriptor fields.
 *
 * The BAR0 register accesses ARE PCI I/O ports (little-endian by
 * PCI convention), so the OS4 IPCI->OutByte/InByte/OutWord/InWord/
 * OutLong/InLong methods handle the byte-swap for us.
 */

#ifndef VIRTIO_H
#define VIRTIO_H

#include <exec/types.h>

/* ---------- Legacy PCI transport register offsets (from BAR0) ---------- */

#define VIRTIO_PCI_HOST_FEATURES     0x00   /* 32-bit RO: features the device offers */
#define VIRTIO_PCI_GUEST_FEATURES    0x04   /* 32-bit RW: features driver accepts */
#define VIRTIO_PCI_QUEUE_PFN         0x08   /* 32-bit RW: queue page frame # (phys/4096) */
#define VIRTIO_PCI_QUEUE_NUM         0x0C   /* 16-bit RO: size of currently-selected queue */
#define VIRTIO_PCI_QUEUE_SEL         0x0E   /* 16-bit RW: select which queue to configure */
#define VIRTIO_PCI_QUEUE_NOTIFY      0x10   /* 16-bit RW: write queue# to kick device */
#define VIRTIO_PCI_STATUS            0x12   /* 8-bit RW: device status flags (below) */
#define VIRTIO_PCI_ISR               0x13   /* 8-bit RC: interrupt status (read-to-clear) */

/* Device-specific config starts at 0x14 IF MSI-X is not enabled.
 * We're not using MSI-X, so device config starts here for us. */
#define VIRTIO_PCI_DEV_CFG_OFFSET    0x14

/* Device status bits (VIRTIO_PCI_STATUS register). Written as a set,
 * NOT as individual toggles — driver ORs bits in as it progresses
 * through the init sequence. Zeroing the register triggers device
 * reset (spec calls this VIRTIO_CONFIG_S_RESET but the bit is 0). */
#define VIRTIO_STATUS_ACKNOWLEDGE    0x01   /* driver recognizes device */
#define VIRTIO_STATUS_DRIVER         0x02   /* driver knows how to drive it */
#define VIRTIO_STATUS_DRIVER_OK      0x04   /* driver setup complete, device may be used */
#define VIRTIO_STATUS_FEATURES_OK    0x08   /* driver-side feature negotiation complete */
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 0x40
#define VIRTIO_STATUS_FAILED         0x80

/* ISR bits (VIRTIO_PCI_ISR register). Read clears. */
#define VIRTIO_ISR_QUEUE             0x01   /* one or more queues used-idx advanced */
#define VIRTIO_ISR_CONFIG            0x02   /* device config changed */

/* ---------- Generic feature bits (all virtio devices) ---------- */

#define VIRTIO_F_NOTIFY_ON_EMPTY     (1U << 24)
#define VIRTIO_F_ANY_LAYOUT          (1U << 27)
#define VIRTIO_F_RING_INDIRECT_DESC  (1U << 28)
#define VIRTIO_F_RING_EVENT_IDX      (1U << 29)

/* ---------- virtio-net device-specific feature bits ---------- */

#define VIRTIO_NET_F_CSUM            (1U << 0)  /* device can checksum on TX */
#define VIRTIO_NET_F_GUEST_CSUM      (1U << 1)  /* driver can accept partial csum */
#define VIRTIO_NET_F_CTRL_GUEST_OFFLOADS (1U << 2)
#define VIRTIO_NET_F_MTU             (1U << 3)  /* device advertises max MTU */
#define VIRTIO_NET_F_MAC             (1U << 5)  /* device provides its MAC in config */
#define VIRTIO_NET_F_GUEST_TSO4      (1U << 7)
#define VIRTIO_NET_F_GUEST_TSO6      (1U << 8)
#define VIRTIO_NET_F_GUEST_ECN       (1U << 9)
#define VIRTIO_NET_F_GUEST_UFO       (1U << 10)
#define VIRTIO_NET_F_HOST_TSO4       (1U << 11)
#define VIRTIO_NET_F_HOST_TSO6       (1U << 12)
#define VIRTIO_NET_F_HOST_ECN        (1U << 13)
#define VIRTIO_NET_F_HOST_UFO        (1U << 14)
#define VIRTIO_NET_F_MRG_RXBUF       (1U << 15) /* mergeable RX buffers */
#define VIRTIO_NET_F_STATUS          (1U << 16) /* device provides link status */
#define VIRTIO_NET_F_CTRL_VQ         (1U << 17) /* control virtqueue present */
#define VIRTIO_NET_F_CTRL_RX         (1U << 18)
#define VIRTIO_NET_F_CTRL_VLAN       (1U << 19)
#define VIRTIO_NET_F_GUEST_ANNOUNCE  (1U << 21)
#define VIRTIO_NET_F_MQ              (1U << 22)
#define VIRTIO_NET_F_CTRL_MAC_ADDR   (1U << 23)

/* Queue indices for virtio-net */
#define VIRTIO_NET_Q_RX              0
#define VIRTIO_NET_Q_TX              1
#define VIRTIO_NET_Q_CTRL            2   /* only if VIRTIO_NET_F_CTRL_VQ */

/* ---------- Virtqueue structures (guest-native endian on PPC BE) ---------- */

/* Descriptor flags */
#define VRING_DESC_F_NEXT            1
#define VRING_DESC_F_WRITE           2   /* buffer is device-write (from device to driver) */
#define VRING_DESC_F_INDIRECT        4

/* Available ring flags */
#define VRING_AVAIL_F_NO_INTERRUPT   1

/* Used ring flags */
#define VRING_USED_F_NO_NOTIFY       1

struct vring_desc {
    uint32 addr_lo;      /* Buffer physical address low 32 bits */
    uint32 addr_hi;      /* Buffer physical address high 32 bits (== 0 on 32-bit) */
    uint32 len;          /* Buffer length in bytes */
    uint16 flags;        /* NEXT | WRITE | INDIRECT */
    uint16 next;         /* Next descriptor if flags & NEXT */
};

/* Available ring — driver → device: "these descriptors are ready to
 * be processed". Variable-length; last field's array size is queue-num. */
struct vring_avail_header {
    uint16 flags;        /* AVAIL_F_NO_INTERRUPT etc. */
    uint16 idx;          /* next slot to write (driver-owned) */
    /* uint16 ring[num];  ← variable-length; accessed via pointer arithmetic */
    /* uint16 used_event; ← optional if VIRTIO_F_EVENT_IDX */
};

/* Used ring — device → driver: "these descriptors have been processed". */
struct vring_used_elem {
    uint32 id;           /* Descriptor chain head index */
    uint32 len;          /* Total bytes written into descriptor buffers */
};

struct vring_used_header {
    uint16 flags;        /* USED_F_NO_NOTIFY etc. */
    uint16 idx;          /* next slot to write (device-owned) */
    /* struct vring_used_elem ring[num]; */
    /* uint16 avail_event; ← optional if VIRTIO_F_EVENT_IDX */
};

/* Legacy virtqueue layout in RAM (spec §2.4.2):
 *
 *     [ desc[num]                      ] (16 * num bytes)
 *     [ avail (header + ring[num] + [avail_event]) ]
 *     ==== pad to 4096-byte alignment ====
 *     [ used  (header + ring[num] + [used_event])  ]
 *
 * The 4096 alignment between avail and used is REQUIRED by the
 * legacy spec (modern relaxes this). Total size for a queue of
 * num descriptors: see VRING_SIZE_LEGACY macro. */

#define VRING_ALIGN                  4096

/* Round up x to a multiple of a. Both must be powers-of-two friendly
 * or at least valid ulong values; only used with align=4096. */
#define VRING_ALIGN_UP(x, a)         (((x) + (a) - 1) & ~((ULONG)(a) - 1))

/* Byte offsets within a contiguous virtqueue buffer given num descriptors. */
#define VRING_DESC_OFFSET(num)       (0)
#define VRING_DESC_BYTES(num)        (16UL * (num))
#define VRING_AVAIL_OFFSET(num)      VRING_DESC_BYTES(num)
#define VRING_AVAIL_BYTES(num)       (4UL + 2UL * (num) + 2UL)  /* header + ring + used_event */
#define VRING_USED_OFFSET(num)       VRING_ALIGN_UP(VRING_AVAIL_OFFSET(num) + VRING_AVAIL_BYTES(num), VRING_ALIGN)
#define VRING_USED_BYTES(num)        (4UL + 8UL * (num) + 2UL)  /* header + ring + avail_event */
#define VRING_TOTAL_BYTES(num)       (VRING_USED_OFFSET(num) + VRING_USED_BYTES(num))

/* ---------- virtio-net packet header (prepended to every packet) ---------- */

/* Without VIRTIO_NET_F_MRG_RXBUF this is 10 bytes; with it, 12 bytes.
 * We aim to negotiate MRG_RXBUF OFF for simplicity. */
struct virtio_net_hdr {
    uint8  flags;         /* csum flags */
    uint8  gso_type;      /* GSO segmentation type */
    uint16 hdr_len;
    uint16 gso_size;
    uint16 csum_start;
    uint16 csum_offset;
    /* uint16 num_buffers; only if VIRTIO_NET_F_MRG_RXBUF */
};

#define VIRTIO_NET_HDR_LEN           10

/* virtio-net status field (only if VIRTIO_NET_F_STATUS negotiated) */
#define VIRTIO_NET_S_LINK_UP         1
#define VIRTIO_NET_S_ANNOUNCE        2

/* ---------- Endianness helpers ----------
 *
 * Virtio 0.9.5 spec §2.4 says legacy virtio uses guest-native endian
 * in virtqueue memory. In practice, QEMU's transitional virtio-net-pci
 * treats it as LITTLE-ENDIAN regardless of guest endianness (an
 * incompatibility with the spec that Linux/BSD drivers work around
 * the same way we do here). Byte-swap every 16- and 32-bit field we
 * read from or write to the virtqueue rings.
 *
 * Using inline byte-by-byte accessors instead of __builtin_bswap*
 * because PPC 460EX has direct byte-reverse load/store insns but
 * struct field access via -> can't easily invoke them. The compiler
 * usually collapses these to lwbrx / sthbrx anyway. */

static inline uint16 vio_le16_get(uint16 *p)
{
    volatile UBYTE *b = (volatile UBYTE *)p;
    return ((uint16)b[0]) | ((uint16)b[1] << 8);
}
static inline void vio_le16_put(uint16 *p, uint16 val)
{
    volatile UBYTE *b = (volatile UBYTE *)p;
    b[0] = (UBYTE)(val);
    b[1] = (UBYTE)(val >> 8);
}
static inline uint32 vio_le32_get(uint32 *p)
{
    volatile UBYTE *b = (volatile UBYTE *)p;
    return ((uint32)b[0])
         | ((uint32)b[1] <<  8)
         | ((uint32)b[2] << 16)
         | ((uint32)b[3] << 24);
}
static inline void vio_le32_put(uint32 *p, uint32 val)
{
    volatile UBYTE *b = (volatile UBYTE *)p;
    b[0] = (UBYTE)(val);
    b[1] = (UBYTE)(val >>  8);
    b[2] = (UBYTE)(val >> 16);
    b[3] = (UBYTE)(val >> 24);
}

/* ---------- Prototypes (implemented in src/virtio.c) ---------- */

struct VirtnetBase;   /* forward decl — real def in virtnet.h */

UBYTE virtio_read_status(struct VirtnetBase *base);
void  virtio_write_status(struct VirtnetBase *base, UBYTE val);
UBYTE virtio_read_isr(struct VirtnetBase *base);
UBYTE virtio_read_dev_cfg8(struct VirtnetBase *base, ULONG offset);
UWORD virtio_read_dev_cfg16(struct VirtnetBase *base, ULONG offset);
void  virtio_notify_queue(struct VirtnetBase *base, UWORD queue_num);

BOOL  virtio_reset_and_ack(struct VirtnetBase *base);
void  virtio_negotiate_features(struct VirtnetBase *base, ULONG driver_wanted);
UWORD virtio_queue_num(struct VirtnetBase *base, UWORD queue_sel);
void  virtio_set_queue_pfn(struct VirtnetBase *base, UWORD queue_sel, ULONG phys_addr);
void  virtio_driver_ok(struct VirtnetBase *base);

#endif /* VIRTIO_H */
