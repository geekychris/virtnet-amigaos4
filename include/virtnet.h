#ifndef VIRTNET_H
#define VIRTNET_H

#include <exec/devices.h>
#include <exec/interfaces.h>
#include <exec/interrupts.h>  /* struct Interrupt */
#include <exec/io.h>
#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <exec/types.h>
#include <dos/dos.h>          /* BPTR */
#include <devices/sana2.h>
#include <expansion/pci.h>    /* PCIDevice, PCIResourceRange */
#include <interfaces/expansion.h>  /* PCIIFace method definitions */
#include <utility/tagitem.h>
#include <utility/hooks.h>
#include <interfaces/utility.h>

#include <proto/exec.h>

/*
 * Library base for virtnet.device. First field MUST be struct Device so
 * that (struct Device *)devBase is well-defined — the kernel wires up the
 * negative-offset jump table relative to &dev_Base.dd_Library.
 *
 * Phase-3a: expansion.library + IPCI + pciDevice added, plus BAR0 slot.
 * All fields may be NULL at any time (Init degrades gracefully — see
 * device.c). Expunge unconditionally frees whatever is non-NULL, so a
 * partially-completed Init still tears down cleanly.
 */
struct VirtnetBase
{
    struct Device      dev_Base;
    struct ExecIFace  *IExec;
    BPTR               dev_SegList;
    struct SignalSemaphore io_lock;

    /* PCI plumbing — populated by Init if virtio-net present, else NULL. */
    struct Library         *ExpansionBase;
    struct PCIIFace        *IPCI;
    struct PCIDevice       *pciDevice;
    struct PCIResourceRange *bar0;   /* legacy: I/O port range; modern: MMIO */

    /* Phase 10: virtio legacy PCI transport state. io_base is the
     * PCI I/O port base extracted from BAR0 (masked to clear the
     * I/O-space bit). Register accesses = IPCI->OutByte(dev, io_base
     * + REG_OFFSET, val) etc. */
    ULONG              io_base;
    ULONG              device_features;   /* what device offered */
    ULONG              driver_features;   /* what we accepted */

    /* Virtqueue backing store — one contiguous alloc per queue.
     * Layout per virtio 0.9.5 §2.4.2: desc[num] + avail + PAD +
     * used. VRING_TOTAL_BYTES(num) sized. Physical address (page
     * frame number) written to VIRTIO_PCI_QUEUE_PFN. */
    APTR               rx_vring;
    ULONG              rx_vring_phys;
    UWORD              rx_vring_num;      /* queue size (from QUEUE_NUM) */
    UWORD              rx_next_avail;     /* next slot in avail ring driver-owned */
    UWORD              rx_last_used;      /* our high-water on used ring */
    APTR               rx_bufs;           /* pool of RX packet buffers */
    ULONG              rx_bufs_phys;

    APTR               tx_vring;
    ULONG              tx_vring_phys;
    UWORD              tx_vring_num;
    UWORD              tx_next_avail;
    UWORD              tx_last_used;
    APTR               tx_scratch2;       /* single TX bounce buffer (phase 10 stub) */
    ULONG              tx_scratch2_phys;

    /* Cached hardware state — read once at Init from RAL/RAH. Serves
     * S2_GETSTATIONADDRESS + S2_DEVICEQUERY without re-hitting MMIO on
     * every request. */
    UBYTE   mac[6];
    BOOL    hw_present;   /* TRUE iff pciDevice + bar0 valid */

    /* Phase 4: DMA-visible RX/TX descriptor rings and RX buffer pool.
     * On sam460ex CPU addresses == PCI-bus addresses for main RAM
     * (identity-mapped), so we hand these pointers directly to the NIC
     * as RDBAL/TDBAL values. NULL if ring alloc failed — subsequent
     * phases check and refuse to enable the device. */
    APTR    rx_ring;      /* 16 × 16-byte descriptors (CPU virt) */
    APTR    tx_ring;      /* 16 × 16-byte descriptors (CPU virt) */
    APTR    rx_buffers;   /* 16 × 2KB pool; per-descriptor slices (CPU virt) */

    /* PCI-bus physical addresses of the above, obtained via
     * IExec->StartDMA + GetDMAList. Written into RDBAL/TDBAL and
     * per-descriptor buffer_addr fields. On sam460ex these usually
     * equal the CPU virt addresses (identity map) but NOT reliably —
     * pa6t_eth's approach (StartDMA per allocation) is the OS4-blessed
     * path. */
    ULONG   rx_ring_phys;
    ULONG   tx_ring_phys;
    ULONG   rx_buffers_phys;

    /* Phase 6j-3: dedicated scratch buffer for CMD_WRITE. Sharing the
     * RX buffer pool with TX caused DMA read/write races on the same
     * address when an inbound frame arrived during outbound send. */
    APTR    tx_scratch;
    ULONG   tx_scratch_phys;

    /* Phase 5: PCI INTx interrupt handler. irq_installed guards Expunge
     * so we RemIntServer only if AddIntServer succeeded. irq_counter and
     * last_icr are updated by the ISR on every fire — read from user
     * context (test programs) via a private query command. Volatile so
     * a spinning reader sees new values. */
    struct Interrupt irq_node;
    ULONG            irq_vector;
    BOOL             irq_installed;
    volatile ULONG   irq_counter;
    volatile ULONG   last_icr;

    /* Phase 6a: SANA-II state machine. Init leaves us OFFLINE with rings
     * programmed but RX/TX disabled and IRQs masked. S2_CONFIGINTERFACE
     * promotes to CONFIGURED (one-shot). S2_ONLINE promotes to ONLINE
     * (writes RCTL.EN, TCTL.EN, unmasks IMS). S2_OFFLINE demotes back
     * to CONFIGURED (masks IMS, clears RCTL.EN, TCTL.EN, retains MAC). */
    ULONG            state;   /* VN_STATE_* below */

    /* Phase 6b: TX ring producer index. Next TX descriptor to fill on
     * CMD_WRITE. Wraps at VN_RING_ENTRIES. Since we currently reply
     * synchronously (poll for TDH advance) there is only ever ONE
     * outstanding descriptor at a time, so tx_next_slot doesn't really
     * need to be a full ring — but keeping it as a slot index gets the
     * bookkeeping right for the future async version. */
    ULONG            tx_next_slot;

    /* Phase 6d: real struct Unit so OS4 exec can enqueue IORequests
     * for async-path commands (CMD_WRITE, S2_BROADCAST, S2_MULTICAST).
     * Embedded (not a pointer) so lifetime tracks VirtnetBase and no
     * separate alloc/free is needed. Init sets up unit_MsgPort with
     * PA_IGNORE so exec bypasses the signal path and calls BeginIO
     * synchronously — same net effect as the old fake io_Unit but with
     * a valid MsgPort structure. Single unit only (unit 0). */
    struct Unit      vn_unit;

    /* Phase 6j: per-opener state. utility.library IUtility gives us
     * the tag-list parsing we need at Open (S2_CopyFromBuff /
     * S2_CopyToBuff lookup) and CallHookPkt for later cooked-mode
     * TX/RX. opener_list is a MinList of V1000Opener nodes; opener_lock
     * serialises Add/Remove against future ISR-driven RX delivery. */
    struct Library     *UtilityBase;
    struct UtilityIFace *IUtility;
    struct MinList      opener_list;
    struct SignalSemaphore opener_lock;

    /* Phase 6k: driver task that processes queued CMD_READ requests
     * when the ISR signals a new RX frame. task_shutdown = TRUE tells
     * the task to exit; ISR signals unit_task with unit_signal_mask
     * on RXT0. Startup handshake uses parent_task/parent_mask so Init
     * can wait until the task is ready before returning to caller. */
    struct Task        *unit_task;
    ULONG               unit_signal_mask;   /* filled by task itself */
    volatile BOOL       task_shutdown;
    volatile BOOL       task_ready_ok;      /* task sets on successful start */

    /* PHASE 7q PER-UNIT-PROCESS ARCH (per rolsen74/amy_skeletons):
     * BeginIO now does only PutMsg(begin_port, ioreq). The unit task
     * dequeues + dispatches the full command handler in ITS own task
     * context, not the caller's. Avoids the DSI guru at 0x01855a0c
     * that fired when we enabled IRQs from Roadshow's task-frozen
     * context. */
    struct MsgPort     *begin_port;         /* BeginIO enqueues here */
    ULONG               begin_port_mask;    /* 1U << begin_port->mp_SigBit */
    /* PHASE 7L: Sana2Hook installed via S2_SANA2HOOK. When set, RX
     * frames are delivered via this hook instead of being dequeued
     * from per-opener read queues + delivered via the CMD_READ ioreq's
     * BufferManagement CopyToBuff. Roadshow prefers this mechanism
     * over CMD_READ when the driver supports it — and may skip the
     * CMD_READ path entirely, sidestepping the guru at 0x01855a0c. */
    APTR                sana2hook;   /* struct Sana2Hook* */

    /* PHASE 7d: request from BeginIO (Roadshow's context) that the unit
     * task perform vn_online_hw() and transition to ONLINE. Calling
     * online_hw() directly from BeginIO caused a DSI guru — the IMS
     * unmask + IRQ firing + unit-task signal race with Roadshow's
     * still-executing AddInterface task. Deferring to the unit task
     * moves the transition to a task context that's not blocking
     * Roadshow. */
    volatile BOOL       pending_online;
    struct Task        *parent_task;        /* startup handshake target */
    ULONG               parent_ready_mask;

    /* Phase 6l diagnostic counters — exposed via DBG_STATUS so tests
     * can pinpoint where the RX-to-CopyToBuff pipeline breaks:
     *   task_wake_count       — # of times task woke on ISR signal
     *   process_rx_dd_seen    — # of DD-set slots the last process_rx call saw
     *   process_rx_delivered  — # of frames CopyToBuff'd to date */
    volatile ULONG      task_wake_count;
    volatile ULONG      process_rx_dd_seen;
    volatile ULONG      process_rx_delivered;

    /* Debug counters for copy-hook isolation (Phase 8). Incremented on
     * every vn_invoke_copy_to/from entry BEFORE we (would) call the
     * caller's function. last_* captures the pointer/tag/size of the
     * most recent invocation for post-hoc inspection via DBG_STATUS. */
    volatile ULONG      copy_to_calls;
    volatile ULONG      copy_from_calls;
    APTR                last_copy_to_ptr;
    APTR                last_copy_from_ptr;
    ULONG               last_copy_to_tag;
    ULONG               last_copy_from_tag;
    ULONG               last_copy_to_size;
    ULONG               last_copy_from_size;
    volatile ULONG      replymsg_skips;
    volatile UWORD      last_skipped_cmd;

    /* Phase 8f: cmd number written on dispatch ENTRY (before switch).
     * Survives if the switch body crashes — testdiag/DBG_STATUS post-
     * crash can read it. Note: DBG cmds are excluded (they don't
     * touch this) so we still see the actual last non-debug cmd. */
    volatile UWORD      last_dispatched_cmd;

    /* Phase 8c: persistent trace log — dos.library file we append every
     * dispatch entry + reply. Opened lazily on first BeginIO (dos.library
     * isn't available at _manager_Init on OS4 boot). Survives task
     * crashes since it lives on disk, so post-crash we can read the log
     * and see which io_Command was the last one dispatched. */
    struct Library     *DOSBase;
    APTR                IDOS;         /* struct DOSIFace* — void APTR to avoid header pull */
    BPTR                trace_fh;
    volatile ULONG      trace_seq;

    /* Phase 7a: snapshot of Roadshow's Sana2DeviceQuery buffer, taken
     * INSIDE our S2_DEVICEQUERY handler. `deviceq_before` = raw bytes
     * of *q from position 0..47 BEFORE we write anything; `deviceq_after`
     * = same after our writes. Also record what q->SizeAvailable was on
     * entry. Lets us verify:
     *   (a) whether Roadshow allocates >= sizeof struct or smaller
     *   (b) whether our writes actually land where we think
     *   (c) whether Roadshow's INPUT MTU (uninitialised bytes) is
     *       what we're seeing back in NetLogViewer's "0 bytes" */
    volatile ULONG      deviceq_call_count;
    volatile ULONG      deviceq_sizeavailable;
    volatile UBYTE      deviceq_before[48];
    volatile UBYTE      deviceq_after[48];

    /* Same-shape snapshot for S2_CONFIGINTERFACE. Snapshots the raw
     * IOSana2Req bytes from offset 0 (io_Message.mn_Node) through
     * offset 79 (well past ios2_DstAddr[]). Roadshow's alloc(0)
     * failure might be triggered by something in this ioreq we
     * haven't looked at. */
    volatile ULONG      config_call_count;
    volatile UBYTE      config_before[80];
    volatile UBYTE      config_after[80];

    /* Phase 7a diagnostic ring: last 32 BeginIO commands so a test can
     * see what Roadshow actually issued when a bind fails. Compact
     * per-entry format; VN_DBG_CMDLOG dumps 32 x 40 bytes. */
    volatile ULONG      cmdlog_head;    /* next slot to write */
    struct {
        UWORD  cmd;                 /* io_Command */
        UBYTE  flags_in;            /* io_Flags on entry */
        WORD   ioerr;               /* io_Error we returned */
        ULONG  wire;                /* WireError we returned */
        ULONG  data_in;             /* ios2_Data pointer on entry */
        ULONG  datalen_in;          /* ios2_DataLength on entry */
        ULONG  bm_in;               /* ios2_BufferManagement on entry */
        UWORD  ptype_in;            /* ios2_PacketType on entry */
        UBYTE  src_out[6];          /* ios2_SrcAddr we wrote back */
        UBYTE  dst_out[6];          /* ios2_DstAddr we wrote back */
        ULONG  datalen_out;         /* ios2_DataLength we returned */
    }                   cmdlog[32];
};

/* Per-opener state — one entry per OpenDevice() call. Stashes the
 * caller's buffer-management cookie plus (once we implement it) the
 * SANA-II copy hook function pointers (see SANA-II-NOTES §7). List
 * lives in VirtnetBase.opener_list; Open adds, Close removes. */
struct V1000Opener
{
    struct MinNode  node;
    APTR            bm_cookie;      /* the ios2_BufferManagement value */
    /* SANA-II Rev 4 tag-list-resolved hooks. Function pointers pulled
     * from the caller's BufferManagement tag list at Open time via
     * IUtility->GetTagData. NULL if the caller didn't supply them
     * (e.g. RAW-only clients like testtx). */
    APTR            copy_from_buff;
    APTR            copy_to_buff;
    /* Which tag we resolved these from. 0 = none, else one of
     * S2_CopyToBuff / S2_CopyToBuff16 / S2_CopyToBuff32. Used to
     * decide the invocation ABI: the plain S2_CopyToBuff is a
     * Rev 4 68k function called via EmulateTags; the -16/-32
     * variants (per OS4 sana2.h SANA2CopyHookMsg) are Hook* called
     * via CallHookPkt. Tag value fits in a ULONG. */
    ULONG           copy_to_tag;
    ULONG           copy_from_tag;

    /* Phase 8d: preferred delivery mechanism — Roadshow-installed
     * Sana2Hook (via S2_SANA2HOOK command). When set, it takes
     * precedence over the tag-list copy_to_buff / copy_from_buff.
     * The hook is invoked with a SANA2CopyHookMsg (schm_Method =
     * S2_CopyToBuff or S2_CopyFromBuff) via IUtility->CallHookPkt.
     * `sana2_hook_methods` mirrors s2h_Methods so we know which
     * operations the hook advertises support for. */
    struct Hook    *sana2_hook;
    ULONG          *sana2_hook_methods;

    /* Phase 6k: queue of CMD_READ requests waiting for a frame.
     * BeginIO enqueues, unit task dequeues + delivers via CopyToBuff.
     * IORequest's io_Message.mn_Node fits as-is on this list. */
    struct MinList  read_queue;

    /* Phase 7a: queue of S2_ONEVENT requests waiting for a specific
     * event mask. Each entry's ios2_WireError holds the caller's
     * requested event mask (S2EVENT_ONLINE etc.). BeginIO adds;
     * vn_signal_event() walks + replies matching entries when the
     * corresponding state transition fires (S2_ONLINE / S2_OFFLINE).
     * Roadshow subscribes to S2EVENT_ONLINE at bind time and expects
     * that request to stay pending until the interface actually goes
     * online — earlier immediate-success reply broke its bind flow. */
    struct MinList  event_queue;
};

/* Unit-state values held in VirtnetBase.state. Defined as constants
 * rather than an enum so testonline can #include this header without
 * dragging in the whole libBase struct. */
#define VN_STATE_OFFLINE     0   /* fresh from Init or reset */
#define VN_STATE_CONFIGURED  1   /* MAC programmed, hardware idle */
#define VN_STATE_ONLINE      2   /* RX/TX enabled, IRQs unmasked */

/* Private debug command IDs — kept in header so tests can #include and
 * share the values. Well outside SANA-II (< 0x38) and new-style (0xC000+)
 * ranges, so no chance of colliding with a spec command. */
#define VN_DBG_STATUS   0xF001   /* returns counter/icr/state */
#define VN_DBG_FIRE_IRQ 0xF002   /* ICS.LSC → forces one ISR call */
#define VN_DBG_SEND     0xF003   /* send raw L2 frame from ios2_Data
                                     * of ios2_DataLength bytes; workaround
                                     * for CMD_WRITE being blocked by
                                     * exec's dispatch layer */
#define VN_DBG_RECV     0xF004   /* Reap one RX frame from the ring.
                                     * If opener has CopyToBuff hook,
                                     * invoke it with schm_To = ios2_Data
                                     * (caller's target cookie). Returns
                                     * frame_len in ios2_DataLength on
                                     * success; io_Error = IOERR_UNITBUSY
                                     * if no frame ready. */
#define VN_DBG_DUMPTX   0xF005   /* Copy first 16 bytes of tx_scratch
                                     * (last-built TX frame — Ethernet
                                     * header + 2 payload bytes) into
                                     * ios2_SrcAddr[0..15]. Diagnostic
                                     * for cooked-mode framing bugs. */
#define VN_DBG_CMDLOG   0xF006   /* Dump the last N BeginIO command
                                     * IDs + io_Error + wire into
                                     * ios2_Data (caller supplies a
                                     * UBYTE buffer of at least
                                     * 32*8 = 256 bytes). Returns count
                                     * of entries in ios2_DataLength.
                                     * Used to trace Roadshow's command
                                     * sequence when a bind fails. */
#define VN_DBG_DQBUF    0xF007   /* Dump the last S2_DEVICEQUERY
                                     * buffer snapshot. Copies:
                                     *   ios2_SrcAddr[0..15] = header
                                     *     (call_count / SizeAvailable
                                     *     as 4-byte BE + 8 pad)
                                     *   ios2_Data[0..47]   = before
                                     *   ios2_Data[48..95]  = after
                                     * Caller supplies >=96-byte buf. */
#define VN_DBG_CFGBUF   0xF008   /* Dump the last S2_CONFIGINTERFACE
                                     * ioreq snapshot. Header in
                                     * ios2_SrcAddr[0..7] = call_count.
                                     * ios2_Data[0..79]  = before
                                     * ios2_Data[80..159] = after
                                     * Caller supplies >=160-byte buf. */

#endif
