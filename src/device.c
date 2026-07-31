/*
 * virtnet.device — SANA-II network device driver for the virtio-net paravirtual NIC
 * (82540EM) as emulated by QEMU on the sam460ex AmigaOS 4.1 PPC target.
 *
 * PHASE 1 — SKELETON ONLY.
 *
 * This build produces a valid loadable OS4 device with a resident tag,
 * Init/Open/Close/Expunge, and a BeginIO that always replies IOERR_NOCMD.
 * No PCI enumeration, no descriptor rings, no SANA-II command dispatch,
 * no unit tasks. Purpose: prove the docker → push → OpenDevice loop.
 *
 * Shape borrowed from derfsss/VirtualSCSIDevice — most of the hard-won
 * OS4 device-driver conventions (resident-in-.data, 68k jump table,
 * DeviceManagerInterface vector layout, RTF_AUTOINIT semantics) come
 * from studying that project. See docs/DESIGN.md for the register-level
 * design; this file is deliberately minimal.
 */

#include "virtnet.h"
#include "virtio.h"
#include "version.h"

#include <exec/exectags.h>
#include <exec/interfaces.h>
#include <exec/resident.h>
#include <exec/errors.h>
#include <exec/execbase.h>
#include <exec/emulation.h>       /* ET_RegisterA0/A1/D0 for EmulateTags */

#include <devices/newstyle.h>   /* NSCMD_DEVICEQUERY + NSDeviceQueryResult */

#include <dos/dos.h>
#include <interfaces/dos.h>

#include <exec/ports.h>    /* PA_IGNORE, MsgPort init */

#include <stdarg.h>
#include <stddef.h>   /* offsetof - S2_DEVICEQUERY field-fits gates */

/* Status-file path. Init appends one line per attempt; each line begins
 * with a monotonic tag so tests can grep for a specific run. Writing to
 * a file rather than DebugPrintF because DebugPrintF on the QEMU sam460ex
 * target isn't wired to the kernel serial channel and disappears; a file
 * we can read back via devbench's /api/file endpoint. */
#define VIRTNET_STATUS_FILE  "RAM:virtnet-init.log"
#define VIRTNET_TRACE_FILE   "RAM:virtnet-trace.log"

/* Phase 8c: lazy trace-log opener. dos.library isn't reliably available
 * at _manager_Init boot-time (Init runs early); attempt to open on
 * first BeginIO instead. Safe to no-op if open fails — caller checks
 * base->trace_fh before write. */
static void vn_trace_open(struct VirtnetBase *base)
{
    if (base->trace_fh) return;   /* already open */
    if (!base->DOSBase) {
        base->DOSBase = base->IExec->OpenLibrary("dos.library", 51);
        if (!base->DOSBase) return;
        base->IDOS = base->IExec->GetInterface(base->DOSBase, "main", 1, NULL);
        if (!base->IDOS) {
            base->IExec->CloseLibrary(base->DOSBase);
            base->DOSBase = NULL;
            return;
        }
    }
    struct DOSIFace *IDOS = (struct DOSIFace *)base->IDOS;
    base->trace_fh = IDOS->Open((CONST_STRPTR)VIRTNET_TRACE_FILE, MODE_NEWFILE);
}

/* Phase 8c revert: TRACEF was hanging when called from BeginIO fast-
 * path post-crash — dos.library Open() may block if there's an issue
 * with the DOS handle lifecycle across task deaths. Compiled out
 * entirely. cmdlog ring in VirtnetBase already records every
 * dispatch and is readable via DBG_CMDLOG through the BeginIO
 * fast-path (added at the same time as this). */
#define TRACEF(base, ...) do { (void)(base); } while (0)

/*
 * Manager Obtain/Release — reference counting on the interface itself.
 * Standard OS4 idiom; every device driver's manager interface has this.
 */
uint32 _manager_Obtain(struct DeviceManagerInterface *Self)
{
    Self->Data.RefCount++;
    return Self->Data.RefCount;
}

uint32 _manager_Release(struct DeviceManagerInterface *Self)
{
    Self->Data.RefCount--;
    return Self->Data.RefCount;
}

/* Forward decls for the vector table. */
extern struct Library *_manager_Init(struct Library *library, BPTR seglist, struct Interface *exec);
extern struct VirtnetBase *_manager_Open(struct DeviceManagerInterface *Self,
                                           struct IOSana2Req *ioreq,
                                           ULONG unitNum, ULONG flags);
extern BPTR _manager_Close(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq);
extern BPTR _manager_Expunge(struct DeviceManagerInterface *Self);
extern void _manager_BeginIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq);
extern LONG _manager_AbortIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq);

/* OS4 vector table for DeviceManagerInterface. Order and terminator are
 * mandated by exec/interfaces.h — do not reorder. */
static const APTR _manager_Vectors[] = {
    (APTR)_manager_Obtain,
    (APTR)_manager_Release,
    (APTR)NULL,             /* Expunge-slot on Interface — unused */
    (APTR)NULL,             /* Clone — unused */
    (APTR)_manager_Open,
    (APTR)_manager_Close,
    (APTR)_manager_Expunge,
    (APTR)NULL,             /* Reserved */
    (APTR)_manager_BeginIO,
    (APTR)_manager_AbortIO,
    (APTR)-1,
};

static const struct TagItem _manager_Tags[] = {
    {MIT_Name,        (ULONG)"__device"},
    {MIT_VectorTable, (ULONG)_manager_Vectors},
    {MIT_Version,     1},
    {TAG_END,         0},
};

const APTR devInterfaces[] = { (APTR)_manager_Tags, (APTR)NULL };

/*
 * 68k-compat jump table. Not strictly required for a SANA-II device
 * (stack callers are all OS4-native), but including it costs nothing
 * and matches the pattern of every shipping OS4 .device — safer
 * default. See VSD device.c for the incident that established the
 * need for this on trackdisk-style drivers.
 */
static const APTR _manager_Vectors68K[] = {
    (APTR)_manager_Open,     /* -6  */
    (APTR)_manager_Close,    /* -12 */
    (APTR)_manager_Expunge,  /* -18 */
    (APTR)NULL,              /* -24 Reserved */
    (APTR)_manager_BeginIO,  /* -30 */
    (APTR)_manager_AbortIO,  /* -36 */
    (APTR)-1,
};

/* Version cookie for the AmigaDOS `Version` command. */
static const char verstag[] __attribute__((used)) = "\0$VER: " DEVVERSIONSTRING;

/* Init tag list — CLT_DataSize tells the kernel how big our libBase is. */
static struct TagItem dev_init_tags[] = {
    {CLT_DataSize,     sizeof(struct VirtnetBase)},
    {CLT_Interfaces,   (ULONG)devInterfaces},
    {CLT_InitFunc,     (ULONG)_manager_Init},
    {CLT_Vector68K,    (ULONG)_manager_Vectors68K},
    {CLT_NoLegacyIFace, FALSE},
    {TAG_END,          0},
};

/*
 * Resident struct — NOT const. Every working OS4 kickstart device places
 * this in writable .data because the kernel/DOS may patch fields during
 * boot binding. Putting it in .rodata is a footgun documented on the
 * VirtualSCSIDevice side. Keep it here, keep it writable, done.
 */
static struct Resident dev_res __attribute__((used)) = {
    RTC_MATCHWORD,
    (struct Resident *)&dev_res,
    (struct Resident *)(&dev_res + 1),
    RTF_NATIVE | RTF_COLDSTART | RTF_AUTOINIT,
    DEVVER,
    NT_DEVICE,
    0,
    DEVNAME,
    DEVVERSIONSTRING,
    (APTR)dev_init_tags,
};

/*
 * Shell entry point. This file is not a runnable executable, but the
 * linker still needs an _start. Print an explanatory line via
 * DebugPrintF (no dos.library needed) and return failure.
 */
int _start(char *argstring, int arglen, struct ExecBase *sysbase)
{
    (void)argstring; (void)arglen;
    struct ExecIFace *IExec = (struct ExecIFace *)sysbase->MainInterface;
    IExec->DebugPrintF("%s is a device — install to SYS:Kickstart/ or "
                       "OpenDevice() from a test program. Cannot run from shell.\n",
                       DEVNAME);
    return 20; /* RETURN_FAIL */
}

/* ------------------------------------------------------------------ */
/* Init / Open / Close / Expunge / BeginIO / AbortIO — skeleton bodies */
/* ------------------------------------------------------------------ */

/* virtio-net-pci device IDs. QEMU's -device virtio-net-pci is
 * transitional — supports both legacy (0.9.5) and modern (1.0+)
 * modes. Legacy device ID is 0x1000, modern is 0x1041. Target the
 * legacy path first — simpler, no PCI capability walking, universally
 * supported. Vendor 0x1AF4 = Red Hat (assigned to virtio). */
#define VIRTIO_PCI_VENDOR            0x1AF4
#define VIRTIO_NET_PCI_DEVICE        0x1000    /* legacy / transitional */
#define VIRTIO_NET_PCI_DEVICE_MODERN 0x1041    /* modern-only (unused for now) */

/* NSCMD_DEVICEQUERY supported-commands list. Standard NSD idiom
 * (see daynaport-amiga/device.c for the pattern). OS4 exec MAY be
 * pre-checking this list before dispatching CMD_WRITE / S2_BROADCAST
 * / other "standard-range" io_Commands to us — that's the current
 * theory for why those cases returned IOERR_UNITBUSY (-6) before
 * BeginIO ran. Terminated by 0.
 *
 * If any client asks for a command not in this list, we still
 * dispatch it (BeginIO's switch handles it or falls to
 * IOERR_NOCMD) — the list is just what we ADVERTISE. */
static const UWORD vn_supported_cmds[] = {
    NSCMD_DEVICEQUERY,
    CMD_READ,
    CMD_WRITE,
    S2_DEVICEQUERY,
    S2_GETSTATIONADDRESS,
    S2_CONFIGINTERFACE,
    S2_ONLINE,
    S2_OFFLINE,
    S2_BROADCAST,
    S2_READORPHAN,
    S2_ONEVENT,
    S2_GETGLOBALSTATS,
    /* Phase 7a: advertise the commands Roadshow issues at bind time
     * even though we treat them as no-ops. Roadshow's DHCP failure with
     * "broadcast access not supported" is likely because it calls
     * S2_ADDMULTICASTADDRESSES(broadcast MAC) and gets IOERR_NOCMD. */
    S2_TRACKTYPE,
    S2_UNTRACKTYPE,
    S2_ADDMULTICASTADDRESS,
    S2_DELMULTICASTADDRESS,
    S2_ADDMULTICASTADDRESSES,
    S2_DELMULTICASTADDRESSES,
    0
};

/* e1000 register offsets we currently touch. Full map lives in the QEMU
 * source at hw/net/e1000_regs.h + the 82540EM datasheet; only the offsets
 * we actually read/write get named here. Cross-referenced with the design
 * agent's docs/DESIGN.md §2. */
#define E1000_REG_CTRL     0x00000
#define E1000_REG_STATUS   0x00008
#define E1000_REG_ICR      0x000C0   /* Interrupt Cause Read (RO/read-to-clear) */
#define E1000_REG_ICS      0x000C8   /* Interrupt Cause Set (WO, forces IRQ) */
#define E1000_REG_IMS      0x000D0   /* Interrupt Mask Set */
#define E1000_REG_IMC      0x000D8   /* Interrupt Mask Clear (WO, write 1 to mask) */
#define E1000_REG_RCTL     0x00100   /* Receive Control */
#define E1000_REG_TCTL     0x00400   /* Transmit Control */
#define E1000_REG_RDBAL    0x02800   /* RX Descriptor Base Address Low */
#define E1000_REG_RDBAH    0x02804   /* RX Descriptor Base Address High */
#define E1000_REG_RDLEN    0x02808   /* RX Descriptor Length (in bytes) */
#define E1000_REG_RDH      0x02810   /* RX Descriptor Head */
#define E1000_REG_RDT      0x02818   /* RX Descriptor Tail */
#define E1000_REG_TDBAL    0x03800   /* TX Descriptor Base Address Low */
#define E1000_REG_TDBAH    0x03804
#define E1000_REG_TDLEN    0x03808
#define E1000_REG_TDH      0x03810
#define E1000_REG_TDT      0x03818
#define E1000_REG_TPT      0x040D4   /* Total Packets Transmitted */

/* Legacy TX descriptor CMD bits (byte 11 of the 16-byte descriptor). */
#define E1000_TXD_CMD_EOP  (1U << 0)   /* End Of Packet */
#define E1000_TXD_CMD_IFCS (1U << 1)   /* Insert FCS (CRC) */
#define E1000_TXD_CMD_RS   (1U << 3)   /* Report Status — set STATUS.DD */

/* Legacy TX descriptor STATUS bits (byte 12). */
#define E1000_TXD_STAT_DD  (1U << 0)   /* Descriptor Done */
#define E1000_REG_RAL      0x05400   /* Receive Address Low  */
#define E1000_REG_RAH      0x05404   /* Receive Address High */

/* Little-endian store to main memory. Descriptor entries live in host RAM
 * but the e1000 reads them as LE, so we byte-reverse on the store side. */
static inline void poke_le32(volatile void *p, uint32 v)
{
    __asm__ volatile ("stwbrx %0, 0, %1" : : "r"(v), "r"(p) : "memory");
}

/* Full memory barrier — ensures all prior stores complete before any
 * subsequent memory ops. Required after building a descriptor and
 * before writing the doorbell (TDT) so HW sees a consistent descriptor.
 * pa6t_eth calls this pasemi_wmb; same instruction. */
static inline void vn_wmb(void)
{
    __asm__ volatile ("sync" : : : "memory");
}

/* -------- Phase 6k: unit task ---------- */

/* Forward decl for use by ISR. */
static void vn_process_rx(struct VirtnetBase *base);
static void vn_online_hw(struct VirtnetBase *base);
static void vn_signal_event(struct VirtnetBase *base, ULONG event_mask);
/* PHASE 7q: full command dispatcher, called from unit task after
 * BeginIO PutMsg's ioreq to begin_port. Does everything BeginIO used
 * to do — the switch/case on io_Command. */
static void vn_dispatch_ioreq(struct VirtnetBase *base,
                                  struct IOSana2Req *ioreq);

/* Task entry function. Called by CreateTaskTags. Args-less; we recover
 * our own Task struct via classic-Amiga SysBase-at-4 → FindTask(NULL),
 * then read tc_UserData for the VirtnetBase pointer. */
static void vn_task_body(void)
{
    struct ExecIFace *IExec = ((struct ExecIFace *)
        ((*(struct ExecBase **)4)->MainInterface));
    struct Task *self = IExec->FindTask(NULL);
    struct VirtnetBase *base = (struct VirtnetBase *)self->tc_UserData;

    int8 sig = IExec->AllocSignal(-1);
    if (sig < 0) {
        base->task_ready_ok = FALSE;
        IExec->Signal(base->parent_task, base->parent_ready_mask);
        return;
    }
    ULONG sig_mask = 1UL << sig;
    base->unit_signal_mask = sig_mask;

    /* PHASE 7q: create begin_port here so its signal is registered
     * to THIS task (signals are per-task). Sig bit auto-allocated by
     * ASOT_PORT via ASOPORT_AllocSig. */
    base->begin_port = (struct MsgPort *)IExec->AllocSysObjectTags(
        ASOT_PORT, ASOPORT_AllocSig, TRUE, TAG_END);
    if (!base->begin_port) {
        IExec->FreeSignal(sig);
        base->unit_signal_mask = 0;
        base->task_ready_ok = FALSE;
        IExec->Signal(base->parent_task, base->parent_ready_mask);
        return;
    }
    base->begin_port_mask = 1UL << base->begin_port->mp_SigBit;

    base->task_ready_ok = TRUE;
    /* Handshake — Init is Wait'ing on parent_ready_mask. */
    IExec->Signal(base->parent_task, base->parent_ready_mask);

    ULONG wait_mask = sig_mask | base->begin_port_mask | SIGBREAKF_CTRL_C;
    while (!base->task_shutdown) {
        ULONG got = IExec->Wait(wait_mask);
        if (IExec->SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) break;

        /* PHASE 7q: drain begin_port FIRST — RX depends on state
         * that CONFIGINTERFACE / ONLINE handlers set up. */
        if (got & base->begin_port_mask) {
            struct IOSana2Req *req;
            while ((req = (struct IOSana2Req *)IExec->GetMsg(base->begin_port)) != NULL) {
                vn_dispatch_ioreq(base, req);
            }
        }

        if (got & sig_mask) {
            IExec->SetSignal(0, sig_mask);   /* consume all pending IRQ signals */
            base->task_wake_count++;
            vn_process_rx(base);
        }
    }

    /* Cleanup — task exits after this returns. */
    if (base->begin_port) {
        IExec->FreeSysObject(ASOT_PORT, base->begin_port);
        base->begin_port = NULL;
        base->begin_port_mask = 0;
    }
    IExec->FreeSignal(sig);
    base->unit_signal_mask = 0;
    base->unit_task = NULL;
}

/* Called from Init. Spawns the task; waits until task signals ready. */
static BOOL vn_task_start(struct VirtnetBase *base, struct ExecIFace *iexec)
{
    int8 ready_bit = iexec->AllocSignal(-1);
    if (ready_bit < 0) return FALSE;

    base->task_shutdown     = FALSE;
    base->task_ready_ok     = FALSE;
    base->parent_task       = iexec->FindTask(NULL);
    base->parent_ready_mask = 1UL << ready_bit;

    iexec->Forbid();
    struct Task *t = iexec->CreateTaskTags("virtnet-unit", 20,
                                           vn_task_body, 8192,
                                           TAG_END);
    if (t) t->tc_UserData = (APTR)base;
    base->unit_task = t;
    iexec->Permit();

    if (!t) { iexec->FreeSignal(ready_bit); return FALSE; }

    (void)iexec->Wait(base->parent_ready_mask);
    iexec->FreeSignal(ready_bit);
    return base->task_ready_ok;
}

/* Called from Expunge. */
static void vn_task_stop(struct VirtnetBase *base, struct ExecIFace *IExec)
{
    if (!base->unit_task) return;
    base->task_shutdown = TRUE;
    IExec->Signal(base->unit_task, SIGBREAKF_CTRL_C);
    /* Yield until the task clears itself. */
    ULONG patience = 100000;
    while (base->unit_task && patience-- > 0) {
        IExec->Forbid();
        IExec->Permit();
    }
}

/* Find the opener matching this request's bm_cookie. NULL if none —
 * unusual (Open would have added one), but Close-race-safe. Caller
 * should hold opener_lock (unless within Init/Expunge). */
static struct V1000Opener *vn_find_opener(struct VirtnetBase *base,
                                              APTR bm_cookie)
{
    struct V1000Opener *op;
    for (op = (struct V1000Opener *)base->opener_list.mlh_Head;
         op->node.mln_Succ;
         op = (struct V1000Opener *)op->node.mln_Succ) {
        if (op->bm_cookie == bm_cookie) return op;
    }
    return NULL;
}

/* Walk every opener's event_queue; for each request whose
 * ios2_WireError (caller-set mask) intersects event_mask, remove
 * from queue, set WireError to the matched bits, and ReplyMsg.
 * Called from S2_ONLINE / S2_OFFLINE handlers on state transitions.
 * Mirrors pa6t_eth's signal_event() but per-opener queue rather
 * than per-driver event_list. */
static void vn_signal_event(struct VirtnetBase *base, ULONG event_mask)
{
    struct ExecIFace *IExec = base->IExec;
    IExec->ObtainSemaphore(&base->opener_lock);
    struct V1000Opener *op;
    for (op = (struct V1000Opener *)base->opener_list.mlh_Head;
         op->node.mln_Succ;
         op = (struct V1000Opener *)op->node.mln_Succ) {
        struct MinNode *n = op->event_queue.mlh_Head, *next;
        while ((next = n->mln_Succ) != NULL) {
            /* Node is at offset 0 of IOSana2Req - direct cast. */
            struct IOSana2Req *r = (struct IOSana2Req *)n;
            ULONG mask = r->ios2_WireError;
            ULONG match = mask & event_mask;
            if (match) {
                IExec->Remove((struct Node *)n);
                r->ios2_WireError    = match;
                r->ios2_Req.io_Error = 0;
                IExec->ReplyMsg((struct Message *)r);
            }
            n = next;
        }
    }
    IExec->ReleaseSemaphore(&base->opener_lock);
}

/* Resolve a CPU-virtual allocation to its PCI-bus physical address
 * via the OS4-blessed StartDMA + GetDMAList path (pa6t_eth pattern).
 * Returns 0 on failure. Caller should have kept the allocation in the
 * DMA state via StartDMA — this function releases via EndDMA before
 * returning, which is fine for one-shot alloc-and-forget or when
 * we're doing StartDMA per operation. For the driver's long-lived
 * ring memory we keep the mapping alive until Expunge (see Init). */
static uint32 vn_dma_phys(struct ExecIFace *IExec, void *virt,
                             uint32 size, uint32 flags)
{
    uint32 nentries = IExec->StartDMA(virt, size, flags);
    if (nentries == 0) return 0;
    struct DMAEntry *dlist = (struct DMAEntry *)IExec->AllocSysObjectTags(
        ASOT_DMAENTRY, ASODMAE_NumEntries, nentries, TAG_DONE);
    if (!dlist) {
        IExec->EndDMA(virt, size, flags | DMAF_NoModify);
        return 0;
    }
    IExec->GetDMAList(virt, size, flags, dlist);
    uint32 phys = (uint32)dlist[0].PhysicalAddress;
    if (nentries > 1) {
        IExec->DebugPrintF("[virtnet] dma_phys: WARNING non-contiguous virt=%p size=%lu nentries=%lu\n",
                           virt, size, nentries);
    }
    IExec->FreeSysObject(ASOT_DMAENTRY, dlist);
    return phys;
}

/* Forward decls — real definitions live further down with the other
 * MMIO helpers. The ISR below needs to call e1000_read32(ICR); the
 * Phase 6k vn_process_rx also uses e1000_write32(RDT). */
static inline uint32 e1000_read32(volatile void *base, uint32 off);
static inline void   e1000_write32(volatile void *base, uint32 off, uint32 val);

/* Ring sizing — used by vn_process_rx (which is above Init where
 * these are also referenced during ring alloc). Moved here so both
 * users see them; keeping the values single-source-of-truth. */
#define VN_RING_ENTRIES   16
#define VN_DESC_SIZE      16
#define VN_RX_BUFSIZE     2048

/* SANA-II Rev 4 has two CopyTo/CopyFromBuff ABIs. The classic tag
 * S2_CopyToBuff / S2_CopyFromBuff points at a m68k asm function that
 * takes (a0=to, a1=from, d0=size) and returns d0=bool — on OS4 we call
 * it via IExec->EmulateTags. The newer S2_CopyToBuff16 / 32 (and their
 * CopyFromBuff twins) point at a struct Hook* that accepts a
 * SANA2CopyHookMsg — invoked via IUtility->CallHookPkt. Roadshow on
 * OS4 supplies BOTH; picking the wrong ABI causes a DSI on first RX
 * delivery (calling a code pointer as a Hook* jumps to bytes 8 into
 * the function). op->copy_*_tag records which one we resolved at
 * Open, and these helpers dispatch accordingly. */
typedef BOOL (*V1000CopyFn)(APTR to, APTR from, ULONG size);

/* Phase 8d: kill switch OFF. With S2_SANA2HOOK now handled properly
 * (Roadshow's preferred delivery path), invoke_copy_to/from should
 * hit the sana2_hook branch first — a CallHookPkt on a legit Hook*
 * with a proper SANA2CopyHookMsg. That eliminates the wrong-ABI
 * hypothesis that motivated this bypass. */
#define VN_BYPASS_COPYHOOKS 0

static BOOL vn_invoke_copy_to(struct VirtnetBase *base,
                                 struct V1000Opener *op,
                                 struct IOSana2Req *ioreq,
                                 APTR from, ULONG size)
{
    if (!op) return FALSE;
    base->copy_to_calls++;
    base->last_copy_to_size = size;

    /* Preferred path: Roadshow installed a Sana2Hook via S2_SANA2HOOK.
     * That hook services multiple methods; invoke with schm_Method =
     * S2_CopyToBuff and a SANA2CopyHookMsg. Falls through to tag-list
     * fallback if the hook wasn't installed. */
    if (op->sana2_hook) {
        base->last_copy_to_ptr = op->sana2_hook;
        base->last_copy_to_tag = S2_CopyToBuff;
        struct SANA2CopyHookMsg msg;
        msg.schm_Method  = S2_CopyToBuff;
        msg.schm_MsgSize = sizeof(msg);
        msg.schm_To      = ioreq->ios2_Data;
        msg.schm_From    = from;
        msg.schm_Size    = size;
        return (BOOL)(ULONG)base->IUtility->CallHookPkt(
            op->sana2_hook, ioreq, &msg);
    }
    if (!op->copy_to_buff) return FALSE;
    base->last_copy_to_ptr = op->copy_to_buff;
    base->last_copy_to_tag = op->copy_to_tag;
    if (op->copy_to_tag == S2_CopyToBuff) {
        V1000CopyFn fn = (V1000CopyFn)op->copy_to_buff;
        return fn(ioreq->ios2_Data, from, size);
    }
    struct SANA2CopyHookMsg msg;
    msg.schm_Method  = op->copy_to_tag;
    msg.schm_MsgSize = sizeof(msg);
    msg.schm_To      = ioreq->ios2_Data;
    msg.schm_From    = from;
    msg.schm_Size    = size;
    return (BOOL)(ULONG)base->IUtility->CallHookPkt(
        (struct Hook *)op->copy_to_buff, ioreq, &msg);
}

static BOOL vn_invoke_copy_from(struct VirtnetBase *base,
                                   struct V1000Opener *op,
                                   struct IOSana2Req *ioreq,
                                   APTR to, ULONG size)
{
    if (!op) return FALSE;
    base->copy_from_calls++;
    base->last_copy_from_size = size;

    if (op->sana2_hook) {
        base->last_copy_from_ptr = op->sana2_hook;
        base->last_copy_from_tag = S2_CopyFromBuff;
        struct SANA2CopyHookMsg msg;
        msg.schm_Method  = S2_CopyFromBuff;
        msg.schm_MsgSize = sizeof(msg);
        msg.schm_To      = to;
        msg.schm_From    = ioreq->ios2_Data;
        msg.schm_Size    = size;
        return (BOOL)(ULONG)base->IUtility->CallHookPkt(
            op->sana2_hook, ioreq, &msg);
    }
    if (!op->copy_from_buff) return FALSE;
    base->last_copy_from_ptr = op->copy_from_buff;
    base->last_copy_from_tag = op->copy_from_tag;
    if (op->copy_from_tag == S2_CopyFromBuff) {
        V1000CopyFn fn = (V1000CopyFn)op->copy_from_buff;
        return fn(to, ioreq->ios2_Data, size);
    }
    struct SANA2CopyHookMsg msg;
    msg.schm_Method  = op->copy_from_tag;
    msg.schm_MsgSize = sizeof(msg);
    msg.schm_To      = to;
    msg.schm_From    = ioreq->ios2_Data;
    msg.schm_Size    = size;
    return (BOOL)(ULONG)base->IUtility->CallHookPkt(
        (struct Hook *)op->copy_from_buff, ioreq, &msg);
}

/* Walk the RX ring, deliver frames to queued CMD_READ requests via the
 * opener's CopyToBuff hook, reset descriptors + advance RDT so HW can
 * refill the slot. Called from the unit task on IRQ signal. */
static void vn_process_rx(struct VirtnetBase *base)
{
    if (!base->rx_vring || !base->IUtility) return;
    struct ExecIFace *IExec = base->IExec;

    UWORD num = base->rx_vring_num;
    /* All ring fields are LITTLE-ENDIAN on QEMU virtio-net-pci —
     * use vio_le*_get / vio_le*_put to access them from BE PPC. */
    UBYTE *avail_bytes = ((UBYTE *)base->rx_vring) + VRING_AVAIL_OFFSET(num);
    struct vring_avail_header *avail = (struct vring_avail_header *)avail_bytes;
    uint16 *avail_ring = (uint16 *)(avail_bytes + 4);
    UBYTE *used_bytes  = ((UBYTE *)base->rx_vring) + VRING_USED_OFFSET(num);
    struct vring_used_header *used = (struct vring_used_header *)used_bytes;
    struct vring_used_elem *used_ring = (struct vring_used_elem *)(used_bytes + 4);

    /* Memory barrier before reading device-owned idx. */
    __asm__ volatile ("eieio; sync" : : : "memory");
    UWORD cur_used_idx = vio_le16_get(&used->idx);
    UWORD last = base->rx_last_used;
    ULONG delivered = 0;
    ULONG iterated = 0;

    while (last != cur_used_idx) {
        iterated++;
        UWORD slot = last % num;
        UWORD desc_idx = (UWORD)vio_le32_get(&used_ring[slot].id);
        ULONG bytes_used = vio_le32_get(&used_ring[slot].len);

        if (desc_idx >= num) {
            base->process_rx_dd_seen++;
            last++;
            continue;
        }
        if (bytes_used < VIRTIO_NET_HDR_LEN + 14) {
            last++;
            vio_le16_put(&avail_ring[vio_le16_get(&avail->idx) % num], desc_idx);
            __asm__ volatile ("eieio; sync" : : : "memory");
            vio_le16_put(&avail->idx, vio_le16_get(&avail->idx) + 1);
            continue;
        }

        UBYTE *rxbuf = (UBYTE *)base->rx_bufs + (desc_idx * VN_RX_BUFSIZE);
        UBYTE *eth   = rxbuf + VIRTIO_NET_HDR_LEN;
        ULONG eth_len = bytes_used - VIRTIO_NET_HDR_LEN;
        ULONG payload_len = eth_len - 14;

        struct IOSana2Req *ioreq = NULL;
        struct V1000Opener *op   = NULL;
        IExec->ObtainSemaphore(&base->opener_lock);
        for (struct V1000Opener *o = (struct V1000Opener *)base->opener_list.mlh_Head;
             o->node.mln_Succ;
             o = (struct V1000Opener *)o->node.mln_Succ) {
            if (o->read_queue.mlh_Head && o->read_queue.mlh_Head->mln_Succ) {
                op = o;
                ioreq = (struct IOSana2Req *)IExec->RemHead((struct List *)&o->read_queue);
                break;
            }
        }
        IExec->ReleaseSemaphore(&base->opener_lock);

        if (ioreq && op && ioreq->ios2_Data) {
            for (int i = 0; i < 6; i++) ioreq->ios2_SrcAddr[i] = eth[6 + i];
            for (int i = 0; i < 6; i++) ioreq->ios2_DstAddr[i] = eth[i];
            ioreq->ios2_PacketType = ((ULONG)eth[12] << 8) | (ULONG)eth[13];
            ioreq->ios2_DataLength = payload_len;

            BOOL ok = vn_invoke_copy_to(base, op, ioreq, eth + 14, payload_len);
            ioreq->ios2_Req.io_Error = ok ? 0 : S2ERR_NO_RESOURCES;
            IExec->ReplyMsg((struct Message *)ioreq);
            if (ok) delivered++;
        }

        /* Refill descriptor. */
        vio_le16_put(&avail_ring[vio_le16_get(&avail->idx) % num], desc_idx);
        __asm__ volatile ("eieio; sync" : : : "memory");
        vio_le16_put(&avail->idx, vio_le16_get(&avail->idx) + 1);
        last++;
    }

    base->rx_last_used = last;
    base->process_rx_delivered += delivered;

    if (iterated > 0) {
        __asm__ volatile ("eieio; sync" : : : "memory");
        virtio_notify_queue(base, VIRTIO_NET_Q_RX);
    }
}

/* -------- Phase 5b: ISR ----------
 * Runs at exec interrupt level on the shared PCI INTx chain. Contract per
 * exec IntServer autodoc + VirtualSCSIDevice's virtio_irq.c:
 *   - No memory allocation, no blocking, no library calls beyond IExec
 *     essentials (Signal/Cause/similar).
 *   - No DebugPrintF (deadlock risk).
 *   - Return non-zero if we recognised and claimed this interrupt;
 *     return zero to let the next server on the chain look at it.
 *
 * Reading ICR both TELLS us what fired AND acknowledges the causes
 * (read-to-clear per DESIGN.md §6). We stash the last non-zero value
 * in devBase->last_icr for post-hoc inspection by test programs;
 * they read it via the driver-private query command added in Phase 5e.
 *
 * When IMS is 0 (Phase 5b) no cause is unmasked so ICR reads here
 * should be 0 and we always return 0 (not-ours). That's the safest
 * starting point: we're on the chain but never claim, so an install
 * bug can't storm. */
static uint32 vn_isr(struct ExceptionContext *ctx,
                        struct ExecBase *sysbase, APTR is_Data)
{
    struct VirtnetBase *base = (struct VirtnetBase *)is_Data;
    (void)ctx; (void)sysbase;

    /* virtio legacy: read VIRTIO_PCI_ISR. It's read-to-clear.
     * bit 0 = queue-used advanced, bit 1 = config changed. If both
     * zero, this IRQ isn't for us (shared INTx). */
    if (!base->io_base) return 0;
    UBYTE isr = base->pciDevice->InByte(base->io_base + VIRTIO_PCI_ISR);
    if (isr == 0) return 0;

    base->irq_counter++;
    base->last_icr = (uint32)isr;   /* reuse existing DBG field for compat */

    /* On queue interrupt, wake unit task to drain used ring.
     * Ring walking is not ISR-safe (Alloc/semaphore/etc.). */
    if ((isr & VIRTIO_ISR_QUEUE) && base->unit_task && base->unit_signal_mask) {
        struct ExecIFace *IExec = base->IExec;
        IExec->Signal(base->unit_task, base->unit_signal_mask);
    }
    /* VIRTIO_ISR_CONFIG (bit 1) would fire on link-status change
     * etc. — nothing consumes it yet, so just log via last_icr. */
    return 1;
}

/* STATUS register bits (§2 of DESIGN.md, e1000x_regs.h:STATUS_*) */
#define E1000_STATUS_FD    (1U << 0)   /* Full Duplex */
#define E1000_STATUS_LU    (1U << 1)   /* Link Up */
#define E1000_STATUS_SPEED_MASK   (3U << 6)
#define E1000_STATUS_SPEED_SHIFT  6

/* PPC MMIO helpers. e1000 registers are little-endian; PPC 460EX is
 * big-endian; we use the byte-reversed load/store insns (lwbrx/stwbrx)
 * so we can dereference LE registers without an explicit bswap step.
 * `volatile` on the pointer stops GCC from caching or reordering. */
static inline uint32 e1000_read32(volatile void *base, uint32 off)
{
    volatile uint8 *p = (volatile uint8 *)base + off;
    uint32 v;
    __asm__ volatile ("lwbrx %0, 0, %1; eieio" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static inline void e1000_write32(volatile void *base, uint32 off, uint32 val)
{
    volatile uint8 *p = (volatile uint8 *)base + off;
    __asm__ volatile ("stwbrx %0, 0, %1; eieio" : : "r"(val), "r"(p) : "memory");
}

/* Diagnostic scope — held open across Init so we can FPrintf status lines
 * inline. We use FPrintf (varargs, works with C promotion rules) rather
 * than VFPrintf (takes a ULONG* argarray in the AmigaDOS convention, which
 * fights with PPC va_list ABI and garbled our earlier attempts).
 *
 * DebugPrintF on this QEMU target isn't wired to the kernel serial port,
 * so a status file is the only observability channel that works before
 * PCI/MMIO is up. Test scripts read it back via /api/file. */
struct V1000LogCtx {
    struct Library *DOSBase;
    struct DOSIFace *IDOS;
    BPTR fh;
};

static void vn_log_open(struct ExecIFace *IExec, struct V1000LogCtx *ctx)
{
    ctx->DOSBase = NULL; ctx->IDOS = NULL; ctx->fh = (BPTR)NULL;
    ctx->DOSBase = IExec->OpenLibrary("dos.library", 51);
    if (!ctx->DOSBase) return;
    ctx->IDOS = (struct DOSIFace *)IExec->GetInterface(ctx->DOSBase, "main", 1, NULL);
    if (!ctx->IDOS) { IExec->CloseLibrary(ctx->DOSBase); ctx->DOSBase = NULL; return; }
    /* MODE_NEWFILE truncates on each Init so we only ever see the latest
     * run's status. If you want the history, tail /api/logs instead. */
    ctx->fh = ctx->IDOS->Open((CONST_STRPTR)VIRTNET_STATUS_FILE, MODE_NEWFILE);
}

static void vn_log_close(struct ExecIFace *IExec, struct V1000LogCtx *ctx)
{
    if (ctx->fh) { ctx->IDOS->Close(ctx->fh); ctx->fh = (BPTR)NULL; }
    if (ctx->IDOS) { IExec->DropInterface((struct Interface *)ctx->IDOS); ctx->IDOS = NULL; }
    if (ctx->DOSBase) { IExec->CloseLibrary(ctx->DOSBase); ctx->DOSBase = NULL; }
}

/* Emit one line — no-op if any part of the log context failed to open. */
#define LOGF(ctx, ...) \
    do { if ((ctx).fh) (ctx).IDOS->FPrintf((ctx).fh, __VA_ARGS__); } while (0)

struct Library *_manager_Init(struct Library *library, BPTR seglist, struct Interface *exec)
{
    struct VirtnetBase *devBase = (struct VirtnetBase *)library;
    struct ExecIFace *iexec = (struct ExecIFace *)exec;

    iexec->DebugPrintF("[virtnet] Init: %s\n", DEVVERSIONSTRING_FULL);

    struct V1000LogCtx log;
    vn_log_open(iexec, &log);
    LOGF(log, (CONST_STRPTR)"=== Init %s ===\n", (STRPTR)DEVVERSIONSTRING_FULL);

    devBase->IExec = iexec;
    devBase->dev_SegList = seglist;
    iexec->InitSemaphore(&devBase->io_lock);

    /* All PCI-side fields start NULL. Any partial-init failure below leaves
     * them NULL, and Expunge unconditionally frees only what is non-NULL.
     * hw_present tracks "PCI enum + BAR mapping both succeeded" as one
     * flag so BeginIO can gate on a single boolean instead of null-checking
     * every pointer. */
    devBase->ExpansionBase = NULL;
    devBase->IPCI          = NULL;
    devBase->pciDevice     = NULL;
    devBase->bar0          = NULL;
    devBase->hw_present    = FALSE;
    devBase->rx_ring       = NULL;
    devBase->tx_ring       = NULL;
    devBase->rx_buffers    = NULL;
    devBase->rx_ring_phys  = 0;
    devBase->tx_ring_phys  = 0;
    devBase->rx_buffers_phys = 0;
    devBase->tx_scratch    = NULL;
    devBase->tx_scratch_phys = 0;
    devBase->irq_vector    = 0;
    devBase->irq_installed = FALSE;
    devBase->irq_counter   = 0;
    devBase->last_icr      = 0;
    devBase->state         = VN_STATE_OFFLINE;
    devBase->tx_next_slot  = 0;
    devBase->UtilityBase   = NULL;
    devBase->IUtility      = NULL;
    iexec->NewList((struct List *)&devBase->opener_list);
    iexec->InitSemaphore(&devBase->opener_lock);

    /* Phase 6d: init the embedded unit's MsgPort. PA_IGNORE tells exec
     * "don't queue+signal, just call BeginIO synchronously". That's the
     * behavior we already implement — BeginIO handles everything inline
     * and calls ReplyMsg when IOF_QUICK is clear. mp_MsgList must still
     * be a valid empty List so nothing dereferences a NULL head/tail. */
    {
        struct Unit *u = &devBase->vn_unit;
        u->unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
        u->unit_MsgPort.mp_Node.ln_Name = (STRPTR)DEVNAME;
        u->unit_MsgPort.mp_Node.ln_Pri  = 0;
        /* PA_IGNORE: our BeginIO handles requests synchronously; no
         * consumer task is needed. Empirically neither PA_IGNORE nor
         * PA_SIGNAL with a real SigTask unblocks CMD_WRITE via DoIO —
         * exec has a deeper check we haven't identified yet (returns
         * IOERR_UNITBUSY before BeginIO runs). The other IO paths
         * (DEVICEQUERY, GETSTATIONADDRESS, ONLINE/OFFLINE, private
         * DBG commands) work fine with either mode. */
        u->unit_MsgPort.mp_Flags        = PA_IGNORE;
        u->unit_MsgPort.mp_SigBit       = 0;
        u->unit_MsgPort.mp_SigTask      = NULL;
        iexec->NewList(&u->unit_MsgPort.mp_MsgList);
        u->unit_flags   = UNITF_ACTIVE;
        u->unit_pad     = 0;
        u->unit_OpenCnt = 0;
    }
    for (int i = 0; i < 6; i++) devBase->mac[i] = 0;

    devBase->dev_Base.dd_Library.lib_Node.ln_Type = NT_DEVICE;
    devBase->dev_Base.dd_Library.lib_Node.ln_Pri  = 0;
    devBase->dev_Base.dd_Library.lib_Node.ln_Name = (STRPTR)DEVNAME;
    devBase->dev_Base.dd_Library.lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    devBase->dev_Base.dd_Library.lib_Version      = DEVVER;
    devBase->dev_Base.dd_Library.lib_Revision     = DEVREV;
    devBase->dev_Base.dd_Library.lib_IdString     = (STRPTR)DEVVERSIONSTRING;

    /* -------- Phase 3a: safe PCI enumeration ----------
     * Try to open expansion.library, get IPCI, and locate the e1000. Any
     * failure at any step is logged and left as NULL — Init STILL returns
     * OK so the loop test (testopen) keeps passing. Hardware ops added in
     * later phases will refuse cleanly if pciDevice is NULL. */

    /* utility.library — needed for GetTagData at Open time and
     * CallHookPkt for cooked-mode TX/RX copies. Open before PCI stuff
     * so we can bail cleanly if it's not available. */
    devBase->UtilityBase = iexec->OpenLibrary("utility.library", 50);
    if (devBase->UtilityBase) {
        devBase->IUtility = (struct UtilityIFace *)
            iexec->GetInterface(devBase->UtilityBase, "main", 1, NULL);
    }
    LOGF(log, (CONST_STRPTR)"utility.library: %s IUtility=%p\n",
         devBase->UtilityBase ? (CONST_STRPTR)"OK" : (CONST_STRPTR)"FAILED",
         devBase->IUtility);

    devBase->ExpansionBase = iexec->OpenLibrary("expansion.library", 53);
    if (!devBase->ExpansionBase) {
        LOGF(log, (CONST_STRPTR)"expansion.library v53 open FAILED - no-hardware mode\n");
        vn_log_close(iexec, &log);
        return (struct Library *)devBase;
    }
    LOGF(log, (CONST_STRPTR)"expansion.library v53 OK\n");

    devBase->IPCI = (struct PCIIFace *)iexec->GetInterface(devBase->ExpansionBase,
                                                            "pci", 1, NULL);
    if (!devBase->IPCI) {
        LOGF(log, (CONST_STRPTR)"GetInterface(pci,1) FAILED - no-hardware mode\n");
        vn_log_close(iexec, &log);
        return (struct Library *)devBase;
    }
    LOGF(log, (CONST_STRPTR)"IPCI v1 acquired\n");

    devBase->pciDevice = devBase->IPCI->FindDeviceTags(
        FDT_VendorID, VIRTIO_PCI_VENDOR,
        FDT_DeviceID, VIRTIO_NET_PCI_DEVICE,
        TAG_END);

    if (!devBase->pciDevice) {
        LOGF(log, (CONST_STRPTR)"FindDeviceTags(%04lx:%04lx): NOT FOUND - "
             "add '-device e1000-82540em' to QEMU\n",
             (ULONG)VIRTIO_PCI_VENDOR, (ULONG)VIRTIO_NET_PCI_DEVICE);
        vn_log_close(iexec, &log);
        return (struct Library *)devBase;
    }

    {
        uint8 bus = 0, dev = 0, fn = 0;
        devBase->pciDevice->GetAddress(&bus, &dev, &fn);
        LOGF(log, (CONST_STRPTR)"FindDeviceTags(%04lx:%04lx): FOUND at PCI %02lx:%02lx.%lu\n",
             (ULONG)VIRTIO_PCI_VENDOR, (ULONG)VIRTIO_NET_PCI_DEVICE,
             (ULONG)bus, (ULONG)dev, (ULONG)fn);
    }

    /* -------- Phase 3c: dump PCI config space ----------
     * All READS. Safe. Values verified against the QEMU e1000 emulation:
     *   vendor=8086, device=100E, class=0200 (Ethernet), rev=03.
     * Command should have MEM_SPACE=1 and BUS_MASTER=1 for the driver to
     * function. If BUS_MASTER isn't set yet we'll turn it on in Phase 3e
     * before touching descriptor rings. */
    {
        struct PCIDevice *pd = devBase->pciDevice;
        UWORD vendor  = pd->ReadConfigWord(PCI_VENDOR_ID);
        UWORD devid   = pd->ReadConfigWord(PCI_DEVICE_ID);
        UWORD cmd     = pd->ReadConfigWord(PCI_COMMAND);
        UWORD stat    = pd->ReadConfigWord(PCI_STATUS);
        UBYTE rev     = pd->ReadConfigByte(PCI_REVISION_ID);
        UBYTE clsprog = pd->ReadConfigByte(PCI_CLASS_PROG);
        UBYTE clsdev  = pd->ReadConfigByte(PCI_CLASS_DEVICE);
        UBYTE intline = pd->ReadConfigByte(PCI_INTERRUPT_LINE);
        UBYTE intpin  = pd->ReadConfigByte(PCI_INTERRUPT_PIN);
        ULONG bar0raw = pd->ReadConfigLong(PCI_BASE_ADDRESS_0);

        LOGF(log, (CONST_STRPTR)"config: vendor=%04lx device=%04lx rev=%02lx class=%02lx.%02lx\n",
             (ULONG)vendor, (ULONG)devid, (ULONG)rev, (ULONG)clsdev, (ULONG)clsprog);
        LOGF(log, (CONST_STRPTR)"config: command=%04lx status=%04lx\n",
             (ULONG)cmd, (ULONG)stat);
        LOGF(log, (CONST_STRPTR)"config: interrupt line=%lu pin=%lu\n",
             (ULONG)intline, (ULONG)intpin);
        LOGF(log, (CONST_STRPTR)"config: BAR0 raw=%08lx  (type=%s, prefetch=%s)\n",
             (ULONG)bar0raw,
             (bar0raw & 1) ? (CONST_STRPTR)"IO" : (CONST_STRPTR)"MEM",
             (bar0raw & 8) ? (CONST_STRPTR)"yes" : (CONST_STRPTR)"no");
    }

    /* -------- Phase 10a: virtio BAR0 (I/O port) ----------
     * Legacy virtio devices expose their control registers via an
     * I/O port BAR (BAR0, bit0 set). We extract the port base by
     * reading the raw BAR value and masking off the low 2 bits (I/O
     * space indicator + reserved). Register accesses go through
     * IPCI->InByte/OutByte etc. which handle the PPC → PCI I/O
     * translation and the little-endian byte-swap for us. */
    {
        ULONG bar0raw = devBase->pciDevice->ReadConfigLong(PCI_BASE_ADDRESS_0);
        if (!(bar0raw & 1)) {
            LOGF(log, (CONST_STRPTR)"BAR0 is MEM not IO — virtio-modern-only device? aborting Init\n");
            vn_log_close(iexec, &log);
            return (struct Library *)devBase;
        }
        devBase->io_base = bar0raw & ~0x03UL;
        LOGF(log, (CONST_STRPTR)"virtio: io_base=%08lx\n", (ULONG)devBase->io_base);
    }

    /* Ensure BUS_MASTER + IO_SPACE are enabled in PCI command. QEMU
     * usually pre-sets these but be defensive — a cold reset may
     * clear BUS_MASTER. */
    {
        UWORD cmd = devBase->pciDevice->ReadConfigWord(PCI_COMMAND);
        UWORD want = cmd | 0x0005;   /* IO_SPACE=1 | BUS_MASTER=4 */
        if (want != cmd) {
            devBase->pciDevice->WriteConfigWord(PCI_COMMAND, want);
            LOGF(log, (CONST_STRPTR)"pci cmd: %04lx -> %04lx (enabling IO+bus-master)\n",
                 (ULONG)cmd, (ULONG)want);
        }
    }

    /* -------- Phase 10b: virtio init handshake ----------
     * Per virtio 0.9.5 §3.1.1: RESET → ACK → DRIVER → feature
     * negotiate → FEATURES_OK → queue setup → DRIVER_OK. Here we
     * do RESET+ACK+DRIVER + feature negotiate. Queue setup and
     * DRIVER_OK are Phase 10c/d, coming next. */
    if (!virtio_reset_and_ack(devBase)) {
        LOGF(log, (CONST_STRPTR)"virtio: reset/ack FAILED — device signaled FAILED\n");
        vn_log_close(iexec, &log);
        return (struct Library *)devBase;
    }
    LOGF(log, (CONST_STRPTR)"virtio: reset OK, status=%02lx\n",
         (ULONG)virtio_read_status(devBase));

    /* Negotiate ONLY the features we actually implement. Keep the
     * subset minimal for the first cut:
     *   - VIRTIO_NET_F_MAC   : device provides MAC via config space
     *   - VIRTIO_NET_F_STATUS: device advertises link status
     * Explicitly REJECT VIRTIO_NET_F_MRG_RXBUF (keeps the packet
     * header at 10 bytes not 12) and all GSO / TSO / checksum-
     * offload variants (we do plain copies, no HW offload support). */
    /* Also request VIRTIO_F_ANY_LAYOUT — this tells the device we
     * can pack the virtio_net_hdr and the Ethernet frame into a
     * SINGLE descriptor (as we do). Without it, TX must be a 2-
     * descriptor chain (header + frame linked via NEXT flag) and
     * QEMU rejects a single-descriptor TX with "bogus descriptor
     * or out of resources". */
    /* Drop VIRTIO_NET_F_MRG_RXBUF — it grows virtio_net_hdr from 10
     * to 12 bytes and we haven't updated VIRTIO_NET_HDR_LEN or the
     * TX packet layout accordingly. Any-layout stays so we can use
     * single-descriptor TX. */
    ULONG want_feat = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS
                    | VIRTIO_F_ANY_LAYOUT;
    virtio_negotiate_features(devBase, want_feat);
    LOGF(log, (CONST_STRPTR)"virtio: device_features=%08lx driver_wants=%08lx accepted=%08lx\n",
         (ULONG)devBase->device_features, (ULONG)want_feat,
         (ULONG)devBase->driver_features);

    /* Discover queue sizes. Legacy virtio-net has two mandatory
     * queues: 0=RX, 1=TX. Queue 2 is CTRL_VQ, only present if we
     * negotiated VIRTIO_NET_F_CTRL_VQ (we didn't, so skip). */
    devBase->rx_vring_num = virtio_queue_num(devBase, VIRTIO_NET_Q_RX);
    devBase->tx_vring_num = virtio_queue_num(devBase, VIRTIO_NET_Q_TX);
    LOGF(log, (CONST_STRPTR)"virtio queues: RX num=%lu, TX num=%lu\n",
         (ULONG)devBase->rx_vring_num, (ULONG)devBase->tx_vring_num);

    /* -------- Phase 10c: read MAC from device config ----------
     * With VIRTIO_NET_F_MAC negotiated, bytes 0..5 of the device-
     * specific config region (offset 0x14 from BAR0 when no MSI-X)
     * hold the primary MAC. QEMU pre-fills from the -device mac=
     * option; default 52:54:00:12:34:5x. */
    if (devBase->driver_features & VIRTIO_NET_F_MAC) {
        for (int i = 0; i < 6; i++) {
            devBase->mac[i] = virtio_read_dev_cfg8(devBase, i);
        }
        LOGF(log, (CONST_STRPTR)"MAC         = %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
             (ULONG)devBase->mac[0], (ULONG)devBase->mac[1], (ULONG)devBase->mac[2],
             (ULONG)devBase->mac[3], (ULONG)devBase->mac[4], (ULONG)devBase->mac[5]);
    } else {
        LOGF(log, (CONST_STRPTR)"virtio: no MAC feature — device didn't provide MAC\n");
        /* Fake a MAC so the driver doesn't crash on GETSTATIONADDRESS.
         * Real fix: fail Init instead. */
        for (int i = 0; i < 6; i++) devBase->mac[i] = 0;
    }
    /* Optional: read link status if VIRTIO_NET_F_STATUS negotiated.
     * Status is 2 bytes at cfg offset 6. */
    if (devBase->driver_features & VIRTIO_NET_F_STATUS) {
        /* virtio-net status is 2 bytes at cfg offset 6. Empirically
         * with QEMU's virtio-net-pci on PPC BE: memory contains
         * bytes [0x00, 0x01] for LINK_UP — i.e. guest-native BIG-
         * endian encoding, not the little-endian we'd expect from
         * the standard registers. Legacy virtio spec explicitly says
         * device-specific config uses guest-native endianness. */
        UBYTE b6 = virtio_read_dev_cfg8(devBase, 6);
        UBYTE b7 = virtio_read_dev_cfg8(devBase, 7);
        UWORD link = ((UWORD)b6 << 8) | b7;   /* BE decode */
        LOGF(log, (CONST_STRPTR)"virtio link status: bytes=%02lx,%02lx link=%04lx (%s)\n",
             (ULONG)b6, (ULONG)b7, (ULONG)link,
             (link & VIRTIO_NET_S_LINK_UP) ? (CONST_STRPTR)"UP" : (CONST_STRPTR)"down");
    }

    /* All the acquire steps succeeded — flip the flag so BeginIO
     * dispatch knows S2_DEVICEQUERY / S2_GETSTATIONADDRESS can
     * answer with real values instead of S2ERR_OUTOFSERVICE. */
    devBase->hw_present = TRUE;

    /* -------- Phase 10d: allocate virtqueues + publish PFN ----------
     * One contiguous 4KB-aligned allocation per queue, VRING_TOTAL_BYTES
     * sized. Zero-init (avail/used idx = 0, all descriptors dead).
     * Resolve physical address, hand PFN to device. */
    if (devBase->rx_vring_num == 0 || devBase->tx_vring_num == 0) {
        LOGF(log, (CONST_STRPTR)"virtio: queue-num was 0 — device rejected setup, aborting\n");
        vn_log_close(iexec, &log);
        return (struct Library *)devBase;
    }

    {
        ULONG rx_bytes = VRING_TOTAL_BYTES(devBase->rx_vring_num);
        ULONG tx_bytes = VRING_TOTAL_BYTES(devBase->tx_vring_num);
        devBase->rx_vring = iexec->AllocVecTags(rx_bytes,
            AVT_Type,              MEMF_SHARED,
            AVT_Contiguous,        TRUE,
            AVT_PhysicalAlignment, TRUE,
            AVT_Alignment,         4096,
            AVT_ClearWithValue,    0,
            TAG_END);
        devBase->tx_vring = iexec->AllocVecTags(tx_bytes,
            AVT_Type,              MEMF_SHARED,
            AVT_Contiguous,        TRUE,
            AVT_PhysicalAlignment, TRUE,
            AVT_Alignment,         4096,
            AVT_ClearWithValue,    0,
            TAG_END);
        if (!devBase->rx_vring || !devBase->tx_vring) {
            LOGF(log, (CONST_STRPTR)"virtio: vring alloc FAILED (rx=%p tx=%p size rx=%lu tx=%lu)\n",
                 devBase->rx_vring, devBase->tx_vring,
                 (ULONG)rx_bytes, (ULONG)tx_bytes);
            vn_log_close(iexec, &log);
            return (struct Library *)devBase;
        }
        devBase->rx_vring_phys = vn_dma_phys(iexec, devBase->rx_vring, rx_bytes, 0);
        devBase->tx_vring_phys = vn_dma_phys(iexec, devBase->tx_vring, tx_bytes, DMA_ReadFromRAM);
        LOGF(log, (CONST_STRPTR)"virtio vrings: rx=%p phys=%08lx (%lu bytes)  tx=%p phys=%08lx (%lu bytes)\n",
             devBase->rx_vring, (ULONG)devBase->rx_vring_phys, (ULONG)rx_bytes,
             devBase->tx_vring, (ULONG)devBase->tx_vring_phys, (ULONG)tx_bytes);
        if ((devBase->rx_vring_phys & 0xFFF) != 0 || (devBase->tx_vring_phys & 0xFFF) != 0) {
            LOGF(log, (CONST_STRPTR)"virtio: vring NOT 4KB-aligned — device will reject PFN\n");
            vn_log_close(iexec, &log);
            return (struct Library *)devBase;
        }

        virtio_set_queue_pfn(devBase, VIRTIO_NET_Q_RX, devBase->rx_vring_phys);
        /* Readback: what did QEMU actually store? PFN is stored per
         * currently-selected queue. After the set, QUEUE_SEL is still
         * VIRTIO_NET_Q_RX so QUEUE_PFN reads the RX PFN back. */
        {
            ULONG rx_pfn_read = devBase->pciDevice->InLong(
                devBase->io_base + VIRTIO_PCI_QUEUE_PFN);
            ULONG expected = devBase->rx_vring_phys / VRING_ALIGN;
            LOGF(log, (CONST_STRPTR)"virtio RX PFN: wrote=%08lx readback=%08lx %s\n",
                 (ULONG)expected, (ULONG)rx_pfn_read,
                 (expected == rx_pfn_read) ? (CONST_STRPTR)"OK" : (CONST_STRPTR)"MISMATCH");
        }
        virtio_set_queue_pfn(devBase, VIRTIO_NET_Q_TX, devBase->tx_vring_phys);
        {
            ULONG tx_pfn_read = devBase->pciDevice->InLong(
                devBase->io_base + VIRTIO_PCI_QUEUE_PFN);
            ULONG expected = devBase->tx_vring_phys / VRING_ALIGN;
            LOGF(log, (CONST_STRPTR)"virtio TX PFN: wrote=%08lx readback=%08lx %s\n",
                 (ULONG)expected, (ULONG)tx_pfn_read,
                 (expected == tx_pfn_read) ? (CONST_STRPTR)"OK" : (CONST_STRPTR)"MISMATCH");
        }
    }

    /* -------- Phase 10d: allocate RX buffer pool + populate avail ring ----
     * One 2 KB buffer per RX descriptor. Fill each descriptor:
     *   addr_lo = buffer's PCI-bus phys addr
     *   addr_hi = 0 (we're 32-bit)
     *   len     = 2048
     *   flags   = VRING_DESC_F_WRITE  (device-writable)
     *   next    = 0
     * Then push every descriptor index onto avail->ring so the device
     * has all 256 slots ready to receive. */
    {
        ULONG num = devBase->rx_vring_num;
        ULONG pool_bytes = num * VN_RX_BUFSIZE;
        devBase->rx_bufs = iexec->AllocVecTags(pool_bytes,
            AVT_Type,              MEMF_SHARED,
            AVT_Contiguous,        TRUE,
            AVT_PhysicalAlignment, TRUE,
            AVT_Alignment,         16,
            AVT_ClearWithValue,    0,
            TAG_END);
        if (!devBase->rx_bufs) {
            LOGF(log, (CONST_STRPTR)"virtio: rx_bufs alloc FAILED (%lu bytes)\n",
                 (ULONG)pool_bytes);
            vn_log_close(iexec, &log);
            return (struct Library *)devBase;
        }
        devBase->rx_bufs_phys = vn_dma_phys(iexec, devBase->rx_bufs, pool_bytes, 0);
        LOGF(log, (CONST_STRPTR)"virtio rx_bufs: cpu=%p phys=%08lx pool=%lu\n",
             devBase->rx_bufs, (ULONG)devBase->rx_bufs_phys, (ULONG)pool_bytes);

        /* Populate descriptor table. All ring fields are stored in
         * LITTLE-ENDIAN — QEMU's transitional virtio-net-pci reads
         * ring memory LE regardless of guest endianness. Use the
         * vio_le*_put helpers so PPC BE writes byte-swap correctly. */
        struct vring_desc *desc = (struct vring_desc *)devBase->rx_vring;
        for (ULONG i = 0; i < num; i++) {
            vio_le32_put(&desc[i].addr_lo, devBase->rx_bufs_phys + (i * VN_RX_BUFSIZE));
            vio_le32_put(&desc[i].addr_hi, 0);
            vio_le32_put(&desc[i].len, VN_RX_BUFSIZE);
            vio_le16_put(&desc[i].flags, VRING_DESC_F_WRITE);
            vio_le16_put(&desc[i].next, 0);
        }
        /* RESTORED: populate avail ring with all 256 slots. Phase 10j
         * isolation test confirmed error is TX-side, not RX. */
        UBYTE *avail_bytes = ((UBYTE *)devBase->rx_vring) + VRING_AVAIL_OFFSET(num);
        struct vring_avail_header *avail = (struct vring_avail_header *)avail_bytes;
        uint16 *avail_ring = (uint16 *)(avail_bytes + 4);
        vio_le16_put(&avail->flags, 0);
        for (ULONG i = 0; i < num; i++) {
            vio_le16_put(&avail_ring[i], (uint16)i);
        }
        __asm__ volatile ("eieio; sync" : : : "memory");
        vio_le16_put(&avail->idx, (uint16)num);
        __asm__ volatile ("eieio; sync" : : : "memory");

        devBase->rx_next_avail = (UWORD)num;
        devBase->rx_last_used  = 0;
    }

    /* -------- Phase 10f: TX scratch buffer ----------
     * Single buffer big enough for one virtio_net_hdr + max Ethernet
     * frame (1514 bytes payload including header + 10-byte hdr = 1524).
     * Round to 2 KB for cache-line safety. All TX packets use this
     * single buffer + descriptor 0, serialized by the SANA-II dispatch
     * layer. */
    {
        /* PHASE 10j-5: Try MEMF_24BITDMA (< 16 MB physical) to test
         * whether high-memory addresses are unreachable by QEMU's
         * PCI DMA on sam460ex. If TX now sends bytes to pcap, the
         * DMA window is the issue. */
        /* Phase 10j-11: allocate with AVT_Lock=TRUE to pin the physical
         * mapping so it doesn't move under our feet between the
         * GetDMAList call and later CPU writes. */
        devBase->tx_scratch2 = iexec->AllocVecTags(VN_RX_BUFSIZE,
            AVT_Type,              MEMF_SHARED,
            AVT_Contiguous,        TRUE,
            AVT_PhysicalAlignment, TRUE,
            AVT_Alignment,         4096,   /* full page align */
            AVT_Lock,              TRUE,   /* pin phys mapping */
            AVT_ClearWithValue,    0,
            TAG_END);
        if (!devBase->tx_scratch2) {
            LOGF(log, (CONST_STRPTR)"virtio: tx_scratch2 alloc FAILED\n");
            vn_log_close(iexec, &log);
            return (struct Library *)devBase;
        }
        devBase->tx_scratch2_phys = vn_dma_phys(iexec, devBase->tx_scratch2,
                                                VN_RX_BUFSIZE, DMA_ReadFromRAM);
        LOGF(log, (CONST_STRPTR)"virtio tx_scratch2: cpu=%p phys=%08lx (%lu bytes)\n",
             devBase->tx_scratch2, (ULONG)devBase->tx_scratch2_phys,
             (ULONG)VN_RX_BUFSIZE);
    }

    /* -------- Phase 10e: install IRQ + flip DRIVER_OK ----------
     * The device may start writing to our RX ring the moment we set
     * DRIVER_OK, so install the IRQ handler FIRST. Existing vn_isr
     * from the virte1000 fork reads e1000 ICR — we'd need to replace
     * that read with a virtio ISR read for actual RX to work, but
     * even the stub is enough to keep the vector claimed. */
    if (devBase->pciDevice) {
        devBase->irq_vector = devBase->pciDevice->MapInterrupt();
        LOGF(log, (CONST_STRPTR)"MapInterrupt: vector=%lu\n", devBase->irq_vector);
        if (devBase->irq_vector != 0) {
            devBase->irq_node.is_Node.ln_Type = NT_INTERRUPT;
            devBase->irq_node.is_Node.ln_Pri  = 0;
            devBase->irq_node.is_Node.ln_Name = (STRPTR)DEVNAME;
            devBase->irq_node.is_Data         = (APTR)devBase;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
            devBase->irq_node.is_Code = (VOID (*)())vn_isr;
#pragma GCC diagnostic pop
            BOOL ok = iexec->AddIntServer(devBase->irq_vector, &devBase->irq_node);
            devBase->irq_installed = ok;
            LOGF(log, (CONST_STRPTR)"AddIntServer: vec=%lu result=%s\n",
                 devBase->irq_vector, ok ? (CONST_STRPTR)"OK" : (CONST_STRPTR)"FAILED");
        }
    }

    /* PHASE 10j FINDING: with DRIVER_OK skipped, QEMU never emits
     * "bogus descriptor" — confirming the error is triggered by ring
     * processing post-DRIVER_OK (specifically, our TX notify or
     * QEMU's RX-pop at driver-up time). Re-enable DRIVER_OK now that
     * we know where the trigger lives. */
    virtio_driver_ok(devBase);
    LOGF(log, (CONST_STRPTR)"virtio: DRIVER_OK set, status=%02lx (device may now use queues)\n",
         (ULONG)virtio_read_status(devBase));

    /* Phase 6k/l: unit task processes queued CMD_READ on ISR signal.
     * PHASE 7q: task ALSO creates its own begin_port + waits on it —
     * BeginIO PutMsg's ioreqs there for the task to dispatch. Port
     * MUST be created inside the task because signals are per-task. */
    BOOL task_ok = vn_task_start(devBase, iexec);
    LOGF(log, (CONST_STRPTR)"unit task: %s (task=%p mask=%08lx begin_port=%p)\n",
         task_ok ? (CONST_STRPTR)"OK" : (CONST_STRPTR)"FAILED",
         devBase->unit_task, (ULONG)devBase->unit_signal_mask,
         devBase->begin_port);

    /* State starts OFFLINE — caller must invoke S2_CONFIGINTERFACE then
     * S2_ONLINE (or the debug VN_DBG_FIRE_IRQ, once online) to make
     * the device live. */
    LOGF(log, (CONST_STRPTR)"Init done: state=OFFLINE (need S2_CONFIGINTERFACE + S2_ONLINE)\n");

    vn_log_close(iexec, &log);
    return (struct Library *)devBase;
}

struct VirtnetBase *_manager_Open(struct DeviceManagerInterface *Self,
                                    struct IOSana2Req *ioreq,
                                    ULONG unitNum, ULONG flags)
{
    struct VirtnetBase *devBase = (struct VirtnetBase *)Self->Data.LibBase;
    (void)flags;

    devBase->dev_Base.dd_Library.lib_OpenCnt++;

    /* Phase-1: only unit 0 is meaningful (single-NIC), but there is no
     * hardware behind it yet. Accept the open so the test app can
     * exercise the shell, then immediately treat BeginIO as no-op. */
    if (unitNum != 0) {
        ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
        ioreq->ios2_Req.io_Unit   = (struct Unit *)-1;
        ioreq->ios2_Req.io_Device = (struct Device *)-1;
        devBase->dev_Base.dd_Library.lib_OpenCnt--;
        return NULL;
    }

    /* Phase 6d: hand out the real unit pointer so exec's async-path
     * dispatch finds a valid MsgPort. Increment unit_OpenCnt to mirror
     * lib_OpenCnt so exec sees the unit as actively used. */
    ioreq->ios2_Req.io_Unit   = &devBase->vn_unit;
    ioreq->ios2_Req.io_Error  = 0;
    /* Phase 7a (per pa6t_eth): mark the message as replied so exec's
     * subsequent SendIO/DoIO on this ioreq sees the reply-state and
     * short-circuits any pending-queue check. pa6t sets this ONLY on
     * Open success. An earlier commit bundled this with a crashy
     * auto-online-in-CONFIGINTERFACE change; that was reverted, but
     * the NT_REPLYMSG line went with it. Re-adding it in isolation. */
    ioreq->ios2_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    devBase->vn_unit.unit_OpenCnt++;
    devBase->dev_Base.dd_Library.lib_Flags &= ~LIBF_DELEXP;

    /* -------- Phase 6j-1: per-opener state ----------
     * Allocate a V1000Opener for this Open. Parse ios2_BufferManagement
     * (SANA-II Rev 4 tag list) for S2_CopyFromBuff and S2_CopyToBuff —
     * the caller-supplied function pointers we'll invoke via
     * IUtility->CallHookPkt on TX and RX. NULL is fine (RAW-only
     * clients like testtx don't set them). */
    struct V1000Opener *op = (struct V1000Opener *)devBase->IExec->AllocVecTags(
        sizeof(struct V1000Opener),
        AVT_Type,           MEMF_SHARED,
        AVT_ClearWithValue, 0,
        TAG_END);
    if (!op) {
        /* Non-fatal for legacy RAW-only callers, but flag the error field
         * to signal the caller that cooked-mode won't work. */
        devBase->IExec->DebugPrintF("[virtnet] Open: opener alloc failed\n");
        /* Continue anyway — testopen etc. don't rely on opener state. */
    } else {
        /* Roadshow (observed empirically via cmdlog): treats
         * ios2_BufferManagement as an INPUT-ONLY cookie. Roadshow
         * saves the tag list pointer it passed at OpenDevice and
         * re-sends that SAME pointer on every subsequent IORequest.
         * It does NOT read back a driver-rewritten cookie. So we
         * keep op->bm_cookie = the tag list pointer (matches what
         * subsequent BeginIO calls pass) and DO NOT overwrite
         * ios2_BufferManagement. An earlier version of this driver
         * rewrote it to point at our V1000Opener, and every
         * subsequent CMD_READ/S2_BROADCAST lookup failed because
         * Roadshow kept sending its original tag list pointer. */
        struct TagItem *tags = (struct TagItem *)ioreq->ios2_BufferManagement;
        if (tags && devBase->IUtility) {
            /* Roadshow can supply hook pointers for either the plain
             * S2_CopyToBuff / S2_CopyFromBuff (byte-oriented) OR the
             * 32-bit-aligned variants (S2_CopyToBuff32 etc). It always
             * supplies AT LEAST ONE. We treat them all the same at the
             * call site — CallHookPkt hands off a byte pointer, and
             * Roadshow's hooks internally cope with either alignment.
             * Falling back through the variants means we can bind to
             * a Roadshow that only provides the 32/16-bit forms. */
            /* Prefer the OS4 Hook-style variants (S2_CopyToBuff32 / 16
             * -- SANA2CopyHookMsg via CallHookPkt) over the classic
             * Rev 4 S2_CopyToBuff (68k asm fn ptr via EmulateTags).
             * Roadshow supplies at least one; when it supplies both,
             * calling the classic one as a Hook* is what put us at
             * DSI 0x01855a0c on first RX delivery. */
            op->copy_to_buff = (APTR)devBase->IUtility->GetTagData(
                S2_CopyToBuff32, 0, tags);
            op->copy_to_tag = op->copy_to_buff ? S2_CopyToBuff32 : 0;
            if (!op->copy_to_buff) {
                op->copy_to_buff = (APTR)devBase->IUtility->GetTagData(
                    S2_CopyToBuff16, 0, tags);
                op->copy_to_tag = op->copy_to_buff ? S2_CopyToBuff16 : 0;
            }
            if (!op->copy_to_buff) {
                op->copy_to_buff = (APTR)devBase->IUtility->GetTagData(
                    S2_CopyToBuff, 0, tags);
                op->copy_to_tag = op->copy_to_buff ? S2_CopyToBuff : 0;
            }

            op->copy_from_buff = (APTR)devBase->IUtility->GetTagData(
                S2_CopyFromBuff32, 0, tags);
            op->copy_from_tag = op->copy_from_buff ? S2_CopyFromBuff32 : 0;
            if (!op->copy_from_buff) {
                op->copy_from_buff = (APTR)devBase->IUtility->GetTagData(
                    S2_CopyFromBuff16, 0, tags);
                op->copy_from_tag = op->copy_from_buff ? S2_CopyFromBuff16 : 0;
            }
            if (!op->copy_from_buff) {
                op->copy_from_buff = (APTR)devBase->IUtility->GetTagData(
                    S2_CopyFromBuff, 0, tags);
                op->copy_from_tag = op->copy_from_buff ? S2_CopyFromBuff : 0;
            }

            devBase->IExec->DebugPrintF(
                "[virtnet] Open: copy_to=%p tag=0x%08lx  copy_from=%p tag=0x%08lx\n",
                op->copy_to_buff, (unsigned long)op->copy_to_tag,
                op->copy_from_buff, (unsigned long)op->copy_from_tag);
        }
        op->bm_cookie = ioreq->ios2_BufferManagement;   /* the tag list */
        devBase->IExec->NewList((struct List *)&op->read_queue);
        devBase->IExec->NewList((struct List *)&op->event_queue);
        devBase->IExec->ObtainSemaphore(&devBase->opener_lock);
        devBase->IExec->AddTail((struct List *)&devBase->opener_list,
                                 (struct Node *)&op->node);
        devBase->IExec->ReleaseSemaphore(&devBase->opener_lock);
    }

    return devBase;
}

BPTR _manager_Close(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq)
{
    struct VirtnetBase *devBase = (struct VirtnetBase *)Self->Data.LibBase;
    BPTR seglist = (BPTR)NULL;

    /* Find + remove this caller's Opener from the list. Matching by
     * bm_cookie — since Phase 7a's BM-cookie rewrite, cookie is now
     * a pointer to the opener itself (self-referential). Roadshow
     * calls CloseDevice with the same cookie it received at Open.
     *
     * CRITICAL: drain the opener's read_queue BEFORE freeing. Roadshow
     * (and any well-behaved caller) is supposed to AbortIO its pending
     * requests first, but if any survive to Close, we MUST reply them
     * — otherwise the caller's WaitIO deadlocks forever and their
     * request memory leaks. Reply with IOERR_ABORTED, same as if
     * AbortIO had been called. Also removes the opener from the
     * driver-side list atomically so the unit_task's process_rx
     * doesn't race and try to deliver into a freed opener. */
    {
        struct ExecIFace *IExec = devBase->IExec;
        struct V1000Opener *op, *found = NULL;
        APTR cookie = ioreq->ios2_BufferManagement;
        IExec->ObtainSemaphore(&devBase->opener_lock);
        for (op = (struct V1000Opener *)devBase->opener_list.mlh_Head;
             op->node.mln_Succ;
             op = (struct V1000Opener *)op->node.mln_Succ) {
            if (op->bm_cookie == cookie) { found = op; break; }
        }
        if (found) {
            /* Drain queued CMD_READs while still holding opener_lock —
             * this locks out process_rx from adding/removing anything.
             * A Node is at offset 0 of IOSana2Req (through io_Message
             * at offset 0 of ios2_Req at offset 0), so cast directly. */
            struct Node *node;
            while ((node = IExec->RemHead(
                        (struct List *)&found->read_queue)) != NULL) {
                struct IOSana2Req *pending = (struct IOSana2Req *)node;
                pending->ios2_Req.io_Error = IOERR_ABORTED;
                IExec->ReplyMsg((struct Message *)pending);
            }
            /* Phase 7a: drain event_queue too — S2_ONEVENT requests
             * that never fired need to be replied to before we free
             * the opener, else the caller's WaitIO deadlocks. */
            while ((node = IExec->RemHead(
                        (struct List *)&found->event_queue)) != NULL) {
                struct IOSana2Req *pending = (struct IOSana2Req *)node;
                pending->ios2_Req.io_Error = IOERR_ABORTED;
                IExec->ReplyMsg((struct Message *)pending);
            }
            IExec->Remove((struct Node *)&found->node);
        }
        IExec->ReleaseSemaphore(&devBase->opener_lock);
        if (found) IExec->FreeVec(found);
    }

    ioreq->ios2_Req.io_Unit   = (struct Unit *)-1;
    ioreq->ios2_Req.io_Device = (struct Device *)-1;

    if (devBase->vn_unit.unit_OpenCnt > 0)
        devBase->vn_unit.unit_OpenCnt--;
    devBase->dev_Base.dd_Library.lib_OpenCnt--;

#ifdef DEBUG
    /* Development builds: force expunge on last-close so each testopen
     * run re-runs Init and picks up freshly pushed binaries. In release
     * builds we behave normally (stay resident until LIBF_DELEXP set). */
    if (devBase->dev_Base.dd_Library.lib_OpenCnt == 0) {
        seglist = _manager_Expunge(Self);
    }
#else
    if (devBase->dev_Base.dd_Library.lib_OpenCnt == 0
        && (devBase->dev_Base.dd_Library.lib_Flags & LIBF_DELEXP))
    {
        seglist = _manager_Expunge(Self);
    }
#endif

    return seglist;
}

BPTR _manager_Expunge(struct DeviceManagerInterface *Self)
{
    struct VirtnetBase *devBase = (struct VirtnetBase *)Self->Data.LibBase;
    struct ExecIFace *IExec = devBase->IExec;
    BPTR seglist = (BPTR)NULL;

    if (devBase->dev_Base.dd_Library.lib_OpenCnt == 0) {
        seglist = devBase->dev_SegList;
        IExec->Remove((struct Node *)devBase);

        /* Release resources in reverse-acquire order.
         *
         * FIRST: mask interrupts + remove the IntServer so a late-arriving
         * interrupt can't dereference base fields we're about to free. */
        if (devBase->bar0) {
            volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
            e1000_write32(mmio, E1000_REG_IMC, 0xFFFFFFFFUL);
            (void)e1000_read32(mmio, E1000_REG_ICR);
        }
        if (devBase->irq_installed) {
            IExec->RemIntServer(devBase->irq_vector, &devBase->irq_node);
            devBase->irq_installed = FALSE;
        }

        /* Phase 6k: stop the unit task before freeing anything it may
         * still reference. Order: quiesce HW (above) → RemIntServer
         * (so ISR can't run) → stop task (so it can't process late-
         * arriving signals against freed memory). */
        vn_task_stop(devBase, IExec);

        /* Free any Openers still on the list (there shouldn't be any at
         * Expunge time — Close should have cleaned up — but defensive). */
        {
            struct V1000Opener *op;
            IExec->ObtainSemaphore(&devBase->opener_lock);
            while ((op = (struct V1000Opener *)
                    IExec->RemHead((struct List *)&devBase->opener_list)) != NULL) {
                IExec->FreeVec(op);
            }
            IExec->ReleaseSemaphore(&devBase->opener_lock);
        }

        if (devBase->tx_scratch) { IExec->FreeVec(devBase->tx_scratch); devBase->tx_scratch = NULL; }
        if (devBase->rx_buffers) { IExec->FreeVec(devBase->rx_buffers); devBase->rx_buffers = NULL; }
        if (devBase->tx_ring)    { IExec->FreeVec(devBase->tx_ring);    devBase->tx_ring    = NULL; }
        if (devBase->rx_ring)    { IExec->FreeVec(devBase->rx_ring);    devBase->rx_ring    = NULL; }
        if (devBase->bar0) {
            devBase->pciDevice->FreeResourceRange(devBase->bar0);
            devBase->bar0 = NULL;
        }
        if (devBase->pciDevice) {
            devBase->IPCI->FreeDevice(devBase->pciDevice);
            devBase->pciDevice = NULL;
        }
        if (devBase->IPCI) {
            IExec->DropInterface((struct Interface *)devBase->IPCI);
            devBase->IPCI = NULL;
        }
        if (devBase->ExpansionBase) {
            IExec->CloseLibrary(devBase->ExpansionBase);
            devBase->ExpansionBase = NULL;
        }
        if (devBase->IUtility) {
            IExec->DropInterface((struct Interface *)devBase->IUtility);
            devBase->IUtility = NULL;
        }
        if (devBase->UtilityBase) {
            IExec->CloseLibrary(devBase->UtilityBase);
            devBase->UtilityBase = NULL;
        }

        IExec->DebugPrintF("[virtnet] Expunge: goodbye.\n");
        IExec->DeleteLibrary((struct Library *)devBase);
    } else {
        devBase->dev_Base.dd_Library.lib_Flags |= LIBF_DELEXP;
    }
    return seglist;
}

/* -------- Phase 6a: online/offline hardware helpers ----------
 * Called from BeginIO's S2_ONLINE / S2_OFFLINE cases (below). Keeping
 * the actual register writes here rather than inline in the dispatch
 * so the state-machine logic stays readable and the hardware sequence
 * can be reused by future code paths (S2_CONFIGINTERFACE that changes
 * MAC while online would need offline-then-online semantics, for
 * example). */
#define E1000_RCTL_EN     (1U << 1)
#define E1000_RCTL_UPE    (1U << 3)   /* Unicast Promiscuous — accept any dst MAC */
#define E1000_RCTL_MPE    (1U << 4)   /* Multicast Promiscuous — accept any multicast */
#define E1000_RCTL_BAM    (1U << 15)
#define E1000_RCTL_SECRC  (1U << 26)
#define E1000_TCTL_EN     (1U << 1)
#define E1000_TCTL_PSP    (1U << 3)
#define E1000_TCTL_CT(v)   (((v) & 0xff)  << 4)
#define E1000_TCTL_COLD(v) (((v) & 0x3ff) << 12)
#define E1000_INTR_TXDW    (1U << 0)
#define E1000_INTR_LSC     (1U << 2)
#define E1000_INTR_RXT0    (1U << 7)

static void vn_online_hw(struct VirtnetBase *base)
{
    if (!base->bar0) return;
    volatile void *mmio = (volatile void *)base->bar0->BaseAddress;
    /* Phase 6l: enable UPE/MPE to eliminate RCTL filtering as a suspect —
     * MAC-filter miss would silently drop the ARP reply from QEMU. With
     * promiscuous on, any frame QEMU sends to the wire lands in the ring. */
    e1000_write32(mmio, E1000_REG_RCTL,
                  E1000_RCTL_EN | E1000_RCTL_UPE | E1000_RCTL_MPE
                  | E1000_RCTL_BAM | E1000_RCTL_SECRC);
    e1000_write32(mmio, E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP
                                       | E1000_TCTL_CT(0x0F) | E1000_TCTL_COLD(0x40));
    e1000_write32(mmio, E1000_REG_IMS, E1000_INTR_LSC | E1000_INTR_RXT0 | E1000_INTR_TXDW);
}

static void vn_offline_hw(struct VirtnetBase *base)
{
    if (!base->bar0) return;
    volatile void *mmio = (volatile void *)base->bar0->BaseAddress;
    e1000_write32(mmio, E1000_REG_IMC, 0xFFFFFFFFUL);
    (void)e1000_read32(mmio, E1000_REG_ICR);
    e1000_write32(mmio, E1000_REG_RCTL, 0);
    e1000_write32(mmio, E1000_REG_TCTL, 0);
}

/* PHASE 7q: BeginIO is now just a thin PutMsg — the unit task
 * dispatches each command in ITS own context (see vn_task_body).
 * This is the rolsen74/amy_skeletons pattern — running command
 * handlers inline in the caller's task context (i.e. Roadshow's
 * AddInterface task) is what caused the DSI guru at 0x01855a0c.
 * See docs/DEBUGGING.md phase 7p for the full analysis. */
void _manager_BeginIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq)
{
    struct VirtnetBase *devBase = (struct VirtnetBase *)Self->Data.LibBase;
    struct ExecIFace *IExec = devBase->IExec;

    /* Standard SANA-II: reject QUICK-io (we always process
     * asynchronously via the unit task). */
    ioreq->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    ioreq->ios2_Req.io_Flags &= ~IOF_QUICK;
    ioreq->ios2_Req.io_Error = 0;
    ioreq->ios2_WireError    = 0;

    /* Phase 8c: fast-path DBG_STATUS + DBG_CMDLOG handling directly in
     * BeginIO — no queueing to unit task. These commands are pure reads
     * of VirtnetBase state and safe to run in the caller's context.
     * Critical because if the unit task crashes, tests calling these
     * commands via DoIO would hang forever waiting for the reply. */
    UWORD cmd = ioreq->ios2_Req.io_Command;
    if (cmd == VN_DBG_STATUS || cmd == VN_DBG_CMDLOG ||
        cmd == VN_DBG_DQBUF   || cmd == VN_DBG_CFGBUF ||
        cmd == VN_DBG_DUMPTX) {
        vn_dispatch_ioreq(devBase, ioreq);
        return;
    }

    if (devBase->begin_port) {
        IExec->PutMsg(devBase->begin_port, (struct Message *)ioreq);
    } else {
        /* Unit task not up (shouldn't happen if Init succeeded);
         * fall back to synchronous dispatch so caller doesn't hang. */
        vn_dispatch_ioreq(devBase, ioreq);
    }
}

/* PHASE 7q: the actual command dispatcher — runs in the unit task's
 * context, invoked from vn_task_body when a message arrives on
 * begin_port. This was the body of _manager_BeginIO pre-refactor. */
static void vn_dispatch_ioreq(struct VirtnetBase *devBase, struct IOSana2Req *ioreq)
{
    struct ExecIFace *IExec = devBase->IExec;

    /* Phase 8f: cmdlog write at DISPATCH ENTRY, not exit. When the
     * switch-body crashes (e.g., DSI 0x01855a0c inside CMD_WRITE),
     * the tail cmdlog write in the exit path never runs, so we lose
     * the record of the crashing command. Write the dispatch stub
     * NOW; the exit path will (harmlessly) overwrite the same slot
     * with post-processing state (ioerr/wire/etc.). */
    devBase->last_dispatched_cmd = ioreq->ios2_Req.io_Command;

    switch (ioreq->ios2_Req.io_Command) {

    case S2_DEVICEQUERY: {
        /* SANA-II-NOTES §3.1 + §5. The caller passes a pointer to a
         * Sana2DeviceQuery in ios2_StatData with SizeAvailable set;
         * we fill in the OUT fields and set SizeSupplied. Values
         * hardcoded for a QEMU-emulated 82540EM — BPS reports 1 Gbps
         * because QEMU never negotiates below max. */
        /* SANA-II Rev 4 §3.1: the Sana2DeviceQuery struct is passed in
         * ios2_StatData. Roadshow follows the spec strictly — ios2_Data
         * contains a stale pointer from prior request reuse (cmdlog
         * consistently shows data=0x6EDExxxx with len=24 — that's a
         * DIFFERENT buffer, not our query struct).
         *
         * PRIOR BUG: we read ios2_Data first, then StatData as fallback.
         * That wrote our MTU/HW/BPS values into the wrong buffer.
         * Roadshow's real query struct stayed all-zero, so
         * QueryInterfaceTagList reported MTU=0 / HardwareType=0 and
         * ConfigureNetInterface UP failed with ENOBUFS because
         * request-alloc size = 0 + overhead. */
        struct Sana2DeviceQuery *q = (struct Sana2DeviceQuery *)ioreq->ios2_StatData;
        if (!q) q = (struct Sana2DeviceQuery *)ioreq->ios2_Data;
        if (!q) {
            ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
            ioreq->ios2_WireError    = S2WERR_NULL_POINTER;
            break;
        }
        /* Reference implementation via kas1e/pa6t_eth's SANA-II Rev 7
         * Ethernet driver - it uses strict offsetof gating per field,
         * caps SizeSupplied at SizeAvailable, and never writes past
         * caller's declared buffer. Olaf Barthel (Roadshow author)
         * confirms this contract in his own SANA-II client:
         *
         *    s2dq.SizeAvailable = sizeof(s2dq) - sizeof(s2dq.RawMTU);
         *    // "The SizeAvailable field must not suggest a larger
         *    //  value than the driver is prepared to accept, or
         *    //  strange things may happen."
         *
         * Roadshow allocates a small (24-byte) Sana2DeviceQuery for
         * its probe pass and passes SizeAvailable to match. If we
         * write past that, we clobber Roadshow-heap fields adjacent
         * to the query struct - one of which is the size arg for
         * Roadshow's post-CONFIG "alloc a receive packet" call.
         * That's the smoking gun for the observed "Could not
         * allocate a network packet of 0 bytes" NetLogViewer error
         * that persisted across every S2_DEVICEQUERY field value we
         * tried. The 0 is a real 0, not a NULL-pointer print. */
        /* Phase 7a diagnostic: snapshot the caller's buffer BEFORE we
         * write anything. This lets DBG_DQBUF dump what Roadshow
         * allocated + initialized, so we can see if SizeAvailable is
         * what we expect and whether writes past our SizeSupplied
         * cap are actually landing outside a valid buffer. */
        devBase->deviceq_call_count++;
        devBase->deviceq_sizeavailable = q->SizeAvailable;
        {
            volatile UBYTE *src = (volatile UBYTE *)q;
            volatile UBYTE *dst = devBase->deviceq_before;
            for (int i = 0; i < 48; i++) dst[i] = src[i];
        }

        /* PHASE 7r ROOT CAUSE FIX (rtl8139 comparative RE):
         * Disassembly of rtl8139.device's S2_DEVICEQUERY at 0x1002370
         * shows it uses PACK(2) Sana2DeviceQuery — sizeof = 34, NOT 36.
         * MTU is at offset 18, BPS at 22, HardwareType at 26, RawMTU
         * at 30. Our compilation uses NATURAL PPC alignment so our
         * `q->MTU = X` writes at offset 20 (wrong). Roadshow reads
         * fields at the pack(2) offsets — which for us are still 0
         * plus one AddrFieldSize-padding byte, i.e. garbage. That
         * garbage propagates into the interpreter at 0x01855a0c and
         * crashes.
         *
         * Fix: byte-level writes at rtl8139's exact offsets. Ignore
         * our compiler's `struct Sana2DeviceQuery` layout entirely
         * for the wire representation. Also matches rtl8139's cap
         * of size=34 in SizeSupplied. */
        UBYTE *raw = (UBYTE *)q;
        ULONG size_wire = 34;   /* pack(2) Sana2DeviceQuery */
        /* PHASE 7s: MATCH RTL8139 EXACTLY. rtl8139's handler does:
         *   supply = min(q->SizeAvailable, 34)     — no fallback for 0
         *   CopyMem(template, q, supply)           — copy that many bytes
         *   q->SizeSupplied = supply               — unconditional at +4
         *   *(q + 22) = runtime_BPS                — unconditional at +22
         *
         * If Roadshow passes SizeAvailable=0, rtl8139 copies 0 bytes;
         * only the two post-copy stores fire (at +4 and +22). Our
         * matching version: same shape. Just per-field writes bounded
         * by supply, plus unconditional SizeSupplied (+4) and BPS
         * (+22) — trusting rtl8139's behavior since Roadshow demonstrably
         * accepts it. */
        /* Hardcoded supply=24 — same as the test that WORKED earlier. */
        ULONG supply = 24;
        if (supply > size_wire) supply = size_wire;
        /* Zero-fill the 24 bytes we're going to write into — earlier
         * test with this exact pattern let AddNetInterface succeed
         * and query returned MTU=1500 State=2. */
        for (ULONG i = 0; i < supply; i++) raw[i] = 0;

        #define S2DQ_WU16(off, val)   do { \
            if (supply >= (off) + 2) { \
                raw[(off) + 0] = (UBYTE)(((val) >> 8) & 0xFF); \
                raw[(off) + 1] = (UBYTE)( (val)       & 0xFF); \
            } } while (0)
        #define S2DQ_WU32(off, val)   do { \
            if (supply >= (off) + 4) { \
                raw[(off) + 0] = (UBYTE)(((val) >> 24) & 0xFF); \
                raw[(off) + 1] = (UBYTE)(((val) >> 16) & 0xFF); \
                raw[(off) + 2] = (UBYTE)(((val) >>  8) & 0xFF); \
                raw[(off) + 3] = (UBYTE)( (val)        & 0xFF); \
            } } while (0)

        S2DQ_WU32( 0, q->SizeAvailable);   /* preserve caller's value */
        S2DQ_WU32( 4, supply);             /* SizeSupplied */
        S2DQ_WU32( 8, 0);                  /* DevQueryFormat */
        S2DQ_WU32(12, 0);                  /* DeviceLevel */
        S2DQ_WU16(16, 48);                 /* AddrFieldSize (UWORD) */
        S2DQ_WU32(18, 1500);               /* MTU — pack(2) offset */
        S2DQ_WU32(22, 1000000000UL);       /* BPS — pack(2) offset */
        S2DQ_WU32(26, S2WireType_Ethernet);/* HardwareType — pack(2) offset */
        S2DQ_WU32(30, 1514);               /* RawMTU — pack(2) offset */

        #undef S2DQ_WU16
        #undef S2DQ_WU32
        /* Phase 7a diagnostic AFTER snapshot. */
        {
            volatile UBYTE *src = (volatile UBYTE *)q;
            volatile UBYTE *dst = devBase->deviceq_after;
            for (int i = 0; i < 48; i++) dst[i] = src[i];
        }
        break;
    }

    case NSCMD_DEVICEQUERY: {
        /* New-style device query — Roadshow and other stack layers call
         * this to enumerate supported commands. OS4 exec may ALSO be
         * calling it internally before dispatching commands, then rejecting
         * anything not in the list with IOERR_UNITBUSY. Format from
         * <devices/newstyle.h>: caller sets ios2_Data (via IOStdReq view:
         * io_Data at offset 40 = ios2_SrcAddr[0..3] here — but Roadshow
         * uses IOStdReq, not IOSana2Req, for this call, so we access
         * fields through the IOStdReq layout). */
        struct IOStdReq *sreq = (struct IOStdReq *)ioreq;
        ULONG want = sizeof(struct NSDeviceQueryResult);
        if (!sreq->io_Data || sreq->io_Length < want) {
            ioreq->ios2_Req.io_Error = IOERR_BADLENGTH;
            break;
        }
        struct NSDeviceQueryResult *r = (struct NSDeviceQueryResult *)sreq->io_Data;
        r->DevQueryFormat    = 0;
        r->SizeAvailable     = want;
        r->DeviceType        = NSDEVTYPE_SANA2;
        r->DeviceSubType     = 0;
        r->SupportedCommands = (UWORD *)vn_supported_cmds;
        sreq->io_Actual      = want;
        break;
    }

    case VN_DBG_RECV: {
        /* Phase 6j-3: reap one RX descriptor and deliver via the
         * caller's CopyToBuff hook.
         *
         * Contract:
         *   IN:  ios2_Data           = target cookie (opaque, passed to
         *                              CopyToBuff as schm_To)
         *        ios2_BufferManagement = same as at Open (opener lookup)
         *   OUT: ios2_DataLength     = frame length (payload after 14-byte
         *                              Ethernet header stripped)
         *        ios2_PacketType     = ethertype from the frame
         *        ios2_SrcAddr[0..5]  = source MAC from the frame
         *        io_Error            = 0 on success, IOERR_UNITBUSY if
         *                              no DD-set descriptor
         *
         * Simplification: only walks slot 0 for this first cut. Real
         * driver needs rx_next_clean tracking + RDT refill. */
        if (devBase->state != VN_STATE_ONLINE || !devBase->rx_ring) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
            break;
        }
        volatile UBYTE *rr = (volatile UBYTE *)devBase->rx_ring;
        volatile UBYTE *rd = rr + (0 * VN_DESC_SIZE);  /* slot 0 */
        if (!(rd[12] & 0x01)) {
            ioreq->ios2_Req.io_Error = IOERR_UNITBUSY;
            break;
        }
        /* Length in bytes 8-9 (LE). */
        ULONG frame_len = ((ULONG)rd[9] << 8) | (ULONG)rd[8];
        if (frame_len < 14 || frame_len > VN_RX_BUFSIZE) {
            /* Malformed length — clear descriptor + refuse. */
            rd[12] = 0;
            ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
            break;
        }
        UBYTE *rxbuf = (UBYTE *)devBase->rx_buffers + (0 * VN_RX_BUFSIZE);

        /* Fill SrcAddr from the frame (bytes 6..11). Ethertype = 12..13. */
        for (int i = 0; i < 6; i++) ioreq->ios2_SrcAddr[i] = rxbuf[6 + i];
        ioreq->ios2_PacketType = ((ULONG)rxbuf[12] << 8) | (ULONG)rxbuf[13];
        ULONG payload_len = frame_len - 14;

        /* Try cooked delivery via CopyToBuff hook. If we have an
         * opener with a hook + IUtility, invoke it with schm_To =
         * caller's cookie (ios2_Data), schm_From = payload start
         * (after Ethernet header). */
        struct V1000Opener *op = NULL;
        if (ioreq->ios2_BufferManagement) {
            IExec->ObtainSemaphore(&devBase->opener_lock);
            op = vn_find_opener(devBase, ioreq->ios2_BufferManagement);
            IExec->ReleaseSemaphore(&devBase->opener_lock);
        }
        if (op && op->copy_to_buff && devBase->IUtility && ioreq->ios2_Data) {
            BOOL ok = vn_invoke_copy_to(devBase, op, ioreq, rxbuf + 14, payload_len);
            if (!ok) {
                ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
                ioreq->ios2_WireError    = S2WERR_BUFF_ERROR;
                /* fall through — clear descriptor either way */
            }
        }
        /* If no hook installed, caller can inspect payload directly:
         * we've populated ios2_PacketType / SrcAddr, and they know
         * where their own buffer is. This is for testrx-style callers. */

        ioreq->ios2_DataLength = payload_len;

        /* Reset descriptor status so HW can reuse this slot on wrap.
         * Clean would be: advance RDT to hand the slot back to HW.
         * For the first-cut single-slot demo we just clear DD; the
         * RDT stays where it is. Enough to prove the round-trip. */
        rd[12] = 0;
        break;
    }

    case VN_DBG_STATUS: {
        /* virtnet private diagnostic. Field overloads:
         *   ios2_DataLength = irq_counter (total ISR fires since Init)
         *   ios2_WireError  = last_icr    (bitmap of last causes seen)
         *   ios2_PacketType = state       (VN_STATE_*)
         *   ios2_SrcAddr[0..3] = TPT (Total Packets Transmitted)
         *   ios2_SrcAddr[4..7] = RDH (RX Descriptor Head — advances on each
         *                       packet HW DMAs into our ring; also
         *                       accumulates count of DD-set descriptors)
         *   ios2_DstAddr[0..3] = count of RX descriptors whose STATUS.DD
         *                       bit is currently set (unclaimed packets)
         *   ios2_DstAddr[4..5] = length of first DD-set RX descriptor
         */
        ioreq->ios2_DataLength = devBase->irq_counter;
        ioreq->ios2_WireError  = devBase->last_icr;
        ioreq->ios2_PacketType = devBase->state;
        /* Phase 10g: expose virtio queue state — we don't have bar0
         * (I/O port device), so read used->idx directly from vring
         * memory. */
        if (devBase->state == VN_STATE_ONLINE && devBase->rx_vring) {
            UWORD num = devBase->rx_vring_num;
            UBYTE *used_bytes = ((UBYTE *)devBase->rx_vring) + VRING_USED_OFFSET(num);
            struct vring_used_header *used = (struct vring_used_header *)used_bytes;
            UWORD used_idx = vio_le16_get(&used->idx);
            UWORD last_used = devBase->rx_last_used;
            ULONG pending = (ULONG)(UWORD)(used_idx - last_used);
            UBYTE *avail_bytes = ((UBYTE *)devBase->rx_vring) + VRING_AVAIL_OFFSET(num);
            struct vring_avail_header *avail = (struct vring_avail_header *)avail_bytes;
            UWORD avail_idx = vio_le16_get(&avail->idx);
            ioreq->ios2_SrcAddr[0] = (UBYTE)(used_idx >> 8);
            ioreq->ios2_SrcAddr[1] = (UBYTE)(used_idx);
            ioreq->ios2_SrcAddr[2] = (UBYTE)(last_used >> 8);
            ioreq->ios2_SrcAddr[3] = (UBYTE)(last_used);
            ioreq->ios2_SrcAddr[4] = (UBYTE)(avail_idx >> 8);
            ioreq->ios2_SrcAddr[5] = (UBYTE)(avail_idx);
            ioreq->ios2_DstAddr[0] = (UBYTE)(pending >> 24);
            ioreq->ios2_DstAddr[1] = (UBYTE)(pending >> 16);
            ioreq->ios2_DstAddr[2] = (UBYTE)(pending >> 8);
            ioreq->ios2_DstAddr[3] = (UBYTE)(pending);
        }
        ioreq->ios2_DstAddr[6]  = (UBYTE)(devBase->task_wake_count >> 8);
        ioreq->ios2_DstAddr[7]  = (UBYTE)(devBase->task_wake_count);
        ioreq->ios2_DstAddr[8]  = (UBYTE)(devBase->process_rx_dd_seen >> 8);
        ioreq->ios2_DstAddr[9]  = (UBYTE)(devBase->process_rx_dd_seen);
        ioreq->ios2_DstAddr[10] = (UBYTE)(devBase->process_rx_delivered >> 8);
        ioreq->ios2_DstAddr[11] = (UBYTE)(devBase->process_rx_delivered);
        /* Phase 8f: last_dispatched_cmd in DstAddr[12..13]. Non-zero
         * post-crash tells us what command was being processed when
         * the driver task died. */
        ioreq->ios2_DstAddr[12] = (UBYTE)(devBase->last_dispatched_cmd >> 8);
        ioreq->ios2_DstAddr[13] = (UBYTE)(devBase->last_dispatched_cmd);
        break;
    }

    case VN_DBG_CMDLOG: {
        /* Dump the cmdlog ring into caller's buffer. 40 bytes/entry:
         *   [0..1]  cmd
         *   [2]     flags_in
         *   [3]     (pad)
         *   [4..5]  ioerr
         *   [6..7]  ptype_in
         *   [8..11] wire
         *   [12..15] data_in ptr
         *   [16..19] datalen_out
         *   [20..23] bm_in ptr
         *   [24..29] src_out (6)
         *   [30..35] dst_out (6)
         *   [36..39] (reserved)
         * Total 32 × 40 = 1280 bytes.
         * Returns the ring's head index in ios2_DataLength. */
        UBYTE *out = (UBYTE *)ioreq->ios2_Data;
        if (out && ioreq->ios2_DataLength >= 32 * 40) {
            ULONG head = devBase->cmdlog_head;
            for (int i = 0; i < 32; i++) {
                UBYTE *p = out + (i * 40);
                p[0]  = (UBYTE)(devBase->cmdlog[i].cmd >> 8);
                p[1]  = (UBYTE)(devBase->cmdlog[i].cmd);
                p[2]  = devBase->cmdlog[i].flags_in;
                p[3]  = 0;
                p[4]  = (UBYTE)(devBase->cmdlog[i].ioerr >> 8);
                p[5]  = (UBYTE)(devBase->cmdlog[i].ioerr);
                p[6]  = (UBYTE)(devBase->cmdlog[i].ptype_in >> 8);
                p[7]  = (UBYTE)(devBase->cmdlog[i].ptype_in);
                p[8]  = (UBYTE)(devBase->cmdlog[i].wire >> 24);
                p[9]  = (UBYTE)(devBase->cmdlog[i].wire >> 16);
                p[10] = (UBYTE)(devBase->cmdlog[i].wire >> 8);
                p[11] = (UBYTE)(devBase->cmdlog[i].wire);
                p[12] = (UBYTE)(devBase->cmdlog[i].data_in >> 24);
                p[13] = (UBYTE)(devBase->cmdlog[i].data_in >> 16);
                p[14] = (UBYTE)(devBase->cmdlog[i].data_in >> 8);
                p[15] = (UBYTE)(devBase->cmdlog[i].data_in);
                p[16] = (UBYTE)(devBase->cmdlog[i].datalen_out >> 24);
                p[17] = (UBYTE)(devBase->cmdlog[i].datalen_out >> 16);
                p[18] = (UBYTE)(devBase->cmdlog[i].datalen_out >> 8);
                p[19] = (UBYTE)(devBase->cmdlog[i].datalen_out);
                p[20] = (UBYTE)(devBase->cmdlog[i].bm_in >> 24);
                p[21] = (UBYTE)(devBase->cmdlog[i].bm_in >> 16);
                p[22] = (UBYTE)(devBase->cmdlog[i].bm_in >> 8);
                p[23] = (UBYTE)(devBase->cmdlog[i].bm_in);
                for (int b = 0; b < 6; b++) {
                    p[24 + b] = devBase->cmdlog[i].src_out[b];
                    p[30 + b] = devBase->cmdlog[i].dst_out[b];
                }
                p[36] = p[37] = p[38] = p[39] = 0;
            }
            ioreq->ios2_DataLength = head;
        }
        break;
    }

    case VN_DBG_DUMPTX: {
        /* Dump first 16 bytes of the last-built cooked TX frame so callers
         * can eyeball the on-wire framing (Ethernet dst/src/etype + start
         * of payload). Overloads ios2_SrcAddr[0..15]. */
        if (devBase->tx_scratch) {
            UBYTE *s = (UBYTE *)devBase->tx_scratch;
            for (int i = 0; i < 16; i++) ioreq->ios2_SrcAddr[i] = s[i];
        }
        break;
    }

    case VN_DBG_CFGBUF: {
        /* Dump last S2_CONFIGINTERFACE ioreq snapshot. */
        UBYTE *out = (UBYTE *)ioreq->ios2_Data;
        ULONG c = devBase->config_call_count;
        ioreq->ios2_SrcAddr[0] = (UBYTE)(c >> 24);
        ioreq->ios2_SrcAddr[1] = (UBYTE)(c >> 16);
        ioreq->ios2_SrcAddr[2] = (UBYTE)(c >>  8);
        ioreq->ios2_SrcAddr[3] = (UBYTE)c;
        for (int i = 4; i < 16; i++) ioreq->ios2_SrcAddr[i] = 0;
        if (out && ioreq->ios2_DataLength >= 160) {
            volatile UBYTE *bsrc = devBase->config_before;
            volatile UBYTE *asrc = devBase->config_after;
            for (int i = 0; i < 80; i++) out[i]      = bsrc[i];
            for (int i = 0; i < 80; i++) out[i + 80] = asrc[i];
        }
        break;
    }

    case VN_DBG_DQBUF: {
        /* Dump last S2_DEVICEQUERY buffer snapshot. Header goes to
         * ios2_SrcAddr[0..15]:
         *   [0..3]  = deviceq_call_count (BE)
         *   [4..7]  = deviceq_sizeavailable (BE)
         *   [8..15] = pad
         * Then 96 bytes to caller's ios2_Data:
         *   [0..47]  = before-write snapshot
         *   [48..95] = after-write snapshot */
        UBYTE *out = (UBYTE *)ioreq->ios2_Data;
        ULONG c = devBase->deviceq_call_count;
        ULONG s = devBase->deviceq_sizeavailable;
        ioreq->ios2_SrcAddr[0] = (UBYTE)(c >> 24);
        ioreq->ios2_SrcAddr[1] = (UBYTE)(c >> 16);
        ioreq->ios2_SrcAddr[2] = (UBYTE)(c >>  8);
        ioreq->ios2_SrcAddr[3] = (UBYTE)c;
        ioreq->ios2_SrcAddr[4] = (UBYTE)(s >> 24);
        ioreq->ios2_SrcAddr[5] = (UBYTE)(s >> 16);
        ioreq->ios2_SrcAddr[6] = (UBYTE)(s >>  8);
        ioreq->ios2_SrcAddr[7] = (UBYTE)s;
        for (int i = 8; i < 16; i++) ioreq->ios2_SrcAddr[i] = 0;
        if (out && ioreq->ios2_DataLength >= 96) {
            volatile UBYTE *bsrc = devBase->deviceq_before;
            volatile UBYTE *asrc = devBase->deviceq_after;
            for (int i = 0; i < 48; i++) out[i]      = bsrc[i];
            for (int i = 0; i < 48; i++) out[i + 48] = asrc[i];
        }
        break;
    }

    case VN_DBG_FIRE_IRQ: {
        /* Force one ISR fire via ICS. Only meaningful when we're ONLINE
         * (IMS unmask must have already happened) — otherwise ICR bits
         * set here are just masked and never delivered. Used by testirq
         * to verify the IRQ path end-to-end after state transitions. */
        if (devBase->state != VN_STATE_ONLINE || !devBase->bar0) {
            ioreq->ios2_Req.io_Error = S2ERR_BAD_STATE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
            break;
        }
        volatile void *mmio = (volatile void *)devBase->bar0->BaseAddress;
        e1000_write32(mmio, E1000_REG_ICS, E1000_INTR_LSC);
        /* Return the (probably updated) counter for convenience. */
        ioreq->ios2_DataLength = devBase->irq_counter;
        break;
    }

    case S2_CONFIGINTERFACE: {
        /* SANA-II-NOTES §3.3: caller sets ios2_SrcAddr = MAC to program;
         * all-zeros means "use factory". */
        /* Phase 7a diagnostic: snapshot the WHOLE ioreq before we
         * write anything, so DBG_CFGBUF can dump every field. Roadshow
         * might set some non-obvious field that steers post-CONFIG
         * pool sizing. */
        devBase->config_call_count++;
        {
            volatile UBYTE *src = (volatile UBYTE *)ioreq;
            volatile UBYTE *dst = devBase->config_before;
            for (int i = 0; i < 80; i++) dst[i] = src[i];
        }

        BOOL want_factory = TRUE;
        for (int i = 0; i < 6; i++)
            if (ioreq->ios2_SrcAddr[i] != 0) { want_factory = FALSE; break; }

        if (devBase->state != VN_STATE_OFFLINE) {
            /* Second (or later) config call — accept as no-op IF caller
             * wants factory MAC (or the same MAC we already have). Refuse
             * only actual MAC changes while online / already-configured. */
            if (!want_factory) {
                BOOL same_mac = TRUE;
                for (int i = 0; i < 6; i++)
                    if (ioreq->ios2_SrcAddr[i] != devBase->mac[i]) {
                        same_mac = FALSE; break;
                    }
                if (!same_mac) {
                    ioreq->ios2_Req.io_Error = S2ERR_BAD_STATE;
                    ioreq->ios2_WireError    = S2WERR_IS_CONFIGURED;
                    break;
                }
            }
            /* Phase 7a: MUST write ios2_SrcAddr = station MAC on the
             * reply. Roadshow's DEBUG=YES output "hardware address =
             * 00:00:00:00:00:00" proved that Roadshow reads SrcAddr
             * from CONFIGINTERFACE reply and stores it as the
             * interface MAC. Not writing left zero → Roadshow
             * couldn't build valid Ethernet headers.
             *
             * pa6t_eth "works" without this because pa6t queues
             * CONFIGINTERFACE to a task and only sets io_Error/
             * WireError; but pa6t's Roadshow-side interface config
             * has DHCP=YES, and the DHCP client separately calls
             * S2_GETSTATIONADDRESS. Our static-IP flow may not. */
            for (int i = 0; i < 6; i++) {
                ioreq->ios2_SrcAddr[i] = devBase->mac[i];
                ioreq->ios2_DstAddr[i] = 0xFF;
            }
            /* Phase 7a diagnostic AFTER snapshot (idempotent path). */
            {
                volatile UBYTE *src = (volatile UBYTE *)ioreq;
                volatile UBYTE *dst = devBase->config_after;
                for (int i = 0; i < 80; i++) dst[i] = src[i];
            }
            break;
        }
        if (!want_factory) {
            /* TODO Phase 6b: program RAL/RAH from ios2_SrcAddr. For now
             * we accept the request and refuse to bounce the factory MAC,
             * so callers can pass their own address without an error but
             * the physical MAC on the wire won't actually change until we
             * wire the write. */
        }
        /* Phase 8d: auto-online re-enabled now that S2_SANA2HOOK is
         * handled — Roadshow's Copy* invocations go through a real
         * Hook+SANA2CopyHookMsg, so RX/TX delivery should be safe. */
        vn_online_hw(devBase);
        devBase->state = VN_STATE_ONLINE;
        vn_signal_event(devBase, S2EVENT_ONLINE);
#if 0
        devBase->state = VN_STATE_CONFIGURED;
        /* PHASE 7o GDB SESSION FINDING: found interpreter function
         * entry at 0x018559b0 (takes r3=input-ptr, r4=obj-with-tables).
         *   r5 = *(r4+0)     — state table base
         *   r30 = *(r4+8)    — some pointer
         *   r28 = *(r4+0x14) — handler table base
         *   r8 = r3 (initial input pointer) then walks the bytecode
         *
         * Interpreter loop body (0x01855a0c onwards) reads halfword
         * tokens, indexes state table, dispatches handlers, uses
         * coroutine-style pattern (r22->0x1c saves/restores stack).
         * Later code (0x01855aa4-e4) makes a system call at address
         * 0x018138ec via `sc` instruction.
         *
         * Preceding sibling functions at 0x01855900, 0x01855944:
         * virtual dispatch via r->0x2e8 (a vtable offset) — classic
         * OO pattern.
         *
         * NOT identified yet: what library this is (unusual VMA of
         * 0x0185xxxx is beyond bsdsocket .text end 0x01054414, so
         * likely a different library relocated high). Candidates
         * based on the pattern:
         *   - dos.library MatchPattern (bytecode wildcard matcher)
         *   - rexxsyslib.library (ARexx interpreter)
         *   - Roadshow's own IP config expression parser
         *
         * The 0x018559b0 function's caller (via bl or bctrl) would
         * reveal the context. Search log for LR value 0x018559b4 to
         * find who calls in. Or set gdb.sh break *0x018559b0 and
         * dump r3/r4 on hit — that tells us the input stream + object.
         */
#if 0
        /* PHASE 7n GDB SESSION FINDING: caught real disassembly at
         * 0x01855a0c via QEMU monitor `x/16i 0x01855a0c`. Code is a
         * bytecode/DFA interpreter loop:
         *   0x01855a0c: lhz  r9, 0(r8)         load 2-byte token @ r8
         *   0x01855a10: slwi r0, r9, 2         * 4 (state table index)
         *   0x01855a14: addi r21, r8, 2        advance stream by 2
         *   0x01855a18: lwzx r10, r5, r0       load state entry
         *   0x01855a1c-24: rlwinm bitfield extracts from r10
         *   0x01855a28: lwzx r25, r28, r26     look up handler
         *   0x01855a2c: add  r8, r21, r11      advance stream (r11 = variable len)
         *   0x01855a30: mtctr r25; bctrl        dispatch to handler
         *
         * DEAR from a live guru: 0x6fd42d7c — a heap-region address.
         * The interpreter walks r8 through a bytecode stream. When
         * r8 walks off the end into unmapped heap, the lhz faults.
         *
         * NEXT: identify what BYTECODE stream this is parsing, and
         * where r8 initially points. Likely a Roadshow-internal
         * parser (regex/DFA for something — DNS names? config? IP
         * expression?) invoked once state=ONLINE lets it run. */
#endif
#endif
        /* Phase 7a: Roadshow's DEBUG=YES output proved that Roadshow
         * reads ios2_SrcAddr from CONFIGINTERFACE reply as the
         * interface hardware address. Must set it here. Also fill
         * DstAddr with broadcast per SANA-II §3.3. */
        for (int i = 0; i < 6; i++) {
            ioreq->ios2_SrcAddr[i] = devBase->mac[i];
            ioreq->ios2_DstAddr[i] = 0xFF;
        }
        /* Phase 7a diagnostic AFTER snapshot. */
        {
            volatile UBYTE *src = (volatile UBYTE *)ioreq;
            volatile UBYTE *dst = devBase->config_after;
            for (int i = 0; i < 80; i++) dst[i] = src[i];
        }
        break;
    }

    case S2_ONLINE: {
        /* SANA-II-NOTES §3.4: requires CONFIGURED. On success enables
         * RX/TX and unmasks IRQs, transitions state to ONLINE. */
        if (devBase->state == VN_STATE_ONLINE) break;   /* idempotent */
        if (devBase->state != VN_STATE_CONFIGURED) {
            ioreq->ios2_Req.io_Error = S2ERR_BAD_STATE;
            ioreq->ios2_WireError    = S2WERR_NOT_CONFIGURED;
            break;
        }
        vn_online_hw(devBase);
        devBase->state = VN_STATE_ONLINE;
        vn_signal_event(devBase, S2EVENT_ONLINE);
        break;
    }

    case S2_OFFLINE: {
        /* SANA-II-NOTES §3.5: always succeeds. Disables hardware and
         * demotes to CONFIGURED (config retained; caller can S2_ONLINE
         * again without re-configuring). */
        if (devBase->state == VN_STATE_ONLINE) {
            vn_offline_hw(devBase);
            devBase->state = VN_STATE_CONFIGURED;
        }
        /* Phase 7a: reply any queued S2_ONEVENT requests waiting for
         * S2EVENT_OFFLINE. */
        vn_signal_event(devBase, S2EVENT_OFFLINE);
        break;
    }

    case VN_DBG_SEND:
    case CMD_WRITE:
    case S2_BROADCAST: {
        /* Phase 10f: virtio TX path.
         *
         * Layout in tx_scratch2:
         *   bytes [0..9]  = virtio_net_hdr (all zero — no GSO, no CSUM,
         *                                    no MRG since we didn't
         *                                    negotiate those features)
         *   bytes [10..]  = Ethernet frame
         *
         * Steps:
         *   1. Sanity-check state + size + buffer.
         *   2. Build the frame in tx_scratch2 (either via CopyFromBuff
         *      hook for cooked mode, or direct copy for RAW).
         *   3. Fill TX descriptor 0 with buffer_addr = tx_scratch2 phys,
         *      len = 10 + frame_bytes, flags = 0 (device-read).
         *   4. Push descriptor index 0 onto avail ring, bump idx, barrier.
         *   5. Kick TX queue (virtio_notify_queue).
         *   6. Poll used-ring briefly for completion so we can reuse
         *      the descriptor on next TX.
         *
         * Single-descriptor design is deliberate: we serialise TX at
         * the SANA-II layer (dispatched from unit task, one at a time),
         * so no need for a full ring. Multi-descriptor TX = Phase 10i. */

        if (devBase->state != VN_STATE_ONLINE) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
            break;
        }

        ULONG len = ioreq->ios2_DataLength;
        if (len == 0 || len > 1514 || !ioreq->ios2_Data ||
            !devBase->tx_scratch2 || !devBase->tx_vring) {
            ioreq->ios2_Req.io_Error = S2ERR_MTU_EXCEEDED;
            ioreq->ios2_WireError    = S2WERR_GENERIC_ERROR;
            break;
        }
        ioreq->ios2_WireError = 0;

        UBYTE *dst = (UBYTE *)devBase->tx_scratch2;

        /* Phase 10j-10 MARKER TEST: write a distinctive byte pattern
         * so we can tell in pcap whether ANY of our data reaches QEMU. */
        for (int i = 0; i < 128; i++) dst[i] = (UBYTE)(0xAA ^ (i & 0xFF));

        /* Zero virtio_net_hdr[0..9]. */
        for (int i = 0; i < VIRTIO_NET_HDR_LEN; i++) dst[i] = 0;

        /* Ethernet frame starts at dst + VIRTIO_NET_HDR_LEN. */
        UBYTE *eth = dst + VIRTIO_NET_HDR_LEN;

        struct V1000Opener *op = NULL;
        if (ioreq->ios2_BufferManagement) {
            IExec->ObtainSemaphore(&devBase->opener_lock);
            op = vn_find_opener(devBase, ioreq->ios2_BufferManagement);
            IExec->ReleaseSemaphore(&devBase->opener_lock);
        }
        BOOL cooked = (op != NULL) && (devBase->IUtility != NULL) &&
                      (op->sana2_hook != NULL || op->copy_from_buff != NULL);

        ULONG eth_len;
        if (cooked) {
            for (int i = 0; i < 6; i++) eth[i]     = ioreq->ios2_DstAddr[i];
            for (int i = 0; i < 6; i++) eth[6 + i] = devBase->mac[i];
            uint16 etype = (uint16)(ioreq->ios2_PacketType & 0xFFFF);
            eth[12] = (UBYTE)(etype >> 8);
            eth[13] = (UBYTE)(etype & 0xFF);
            BOOL ok = vn_invoke_copy_from(devBase, op, ioreq, eth + 14, len);
            if (!ok) {
                ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
                ioreq->ios2_WireError    = S2WERR_BUFF_ERROR;
                break;
            }
            eth_len = 14 + len;
        } else {
            UBYTE *src = (UBYTE *)ioreq->ios2_Data;
            for (ULONG i = 0; i < len; i++) eth[i] = src[i];
            eth_len = len;
        }
        /* Minimum Ethernet frame = 60 bytes (padding). Virtio devices
         * are usually happy without it, but keep it for compatibility. */
        volatile UBYTE *pad = eth;
        for (ULONG i = eth_len; i < 60; i++) pad[i] = 0;
        if (eth_len < 60) eth_len = 60;

        ULONG total_bytes = VIRTIO_NET_HDR_LEN + eth_len;

        /* Phase 10j-9: use CachePreDMA — the OS4-blessed primitive for
         * flushing CPU cache AND getting the PCI-bus phys addr in one
         * atomic call. Returns PhysicalAddress with the mapping. Must
         * pair with CachePostDMA after device finishes. Length is
         * in-out — may be reduced if the mapping spans discontiguous
         * physical pages. */
        ULONG pre_len = total_bytes;
        uint32 live_phys = (uint32)(ULONG)IExec->CachePreDMA(
            (CONST_APTR)devBase->tx_scratch2, &pre_len, DMA_ReadFromRAM);
        if (live_phys == 0 || pre_len < total_bytes) {
            /* Fallback: use init-time phys and separate cache flush. */
            IExec->CacheClearE((APTR)devBase->tx_scratch2, total_bytes, CACRF_ClearD);
            live_phys = devBase->tx_scratch2_phys;
        }

        /* Single-descriptor TX (with ANY_LAYOUT negotiated). */
        struct vring_desc *tdesc = (struct vring_desc *)devBase->tx_vring;
        vio_le32_put(&tdesc[0].addr_lo, live_phys);
        vio_le32_put(&tdesc[0].addr_hi, 0);
        vio_le32_put(&tdesc[0].len, total_bytes);
        vio_le16_put(&tdesc[0].flags, 0);
        vio_le16_put(&tdesc[0].next, 0);

        /* Flush descriptor RAM too so QEMU DMA-reads it fresh. */
        IExec->CacheClearE((APTR)devBase->tx_vring, 16, CACRF_ClearD);
        __asm__ volatile ("sync" : : : "memory");

        /* Save diagnostics. */
        devBase->last_copy_to_ptr = (APTR)(ULONG)live_phys;
        devBase->last_copy_to_tag = (ULONG)total_bytes;
        devBase->last_copy_to_size = (ULONG)eth_len;

        /* Push descriptor 0 onto TX avail ring. */
        UWORD tx_num = devBase->tx_vring_num;
        UBYTE *tavail_bytes = ((UBYTE *)devBase->tx_vring) + VRING_AVAIL_OFFSET(tx_num);
        struct vring_avail_header *tavail = (struct vring_avail_header *)tavail_bytes;
        uint16 *tavail_ring = (uint16 *)(tavail_bytes + 4);
        uint16 cur_avail = vio_le16_get(&tavail->idx);
        vio_le16_put(&tavail_ring[cur_avail % tx_num], 0);
        __asm__ volatile ("eieio; sync" : : : "memory");
        vio_le16_put(&tavail->idx, cur_avail + 1);
        __asm__ volatile ("eieio; sync" : : : "memory");
        /* Flush avail ring changes to RAM before the doorbell. */
        IExec->CacheClearE(tavail_bytes, 4 + 2 * tx_num, CACRF_ClearD);
        virtio_notify_queue(devBase, VIRTIO_NET_Q_TX);

        /* Poll TX used ring briefly for completion. */
        UBYTE *tused_bytes = ((UBYTE *)devBase->tx_vring) + VRING_USED_OFFSET(tx_num);
        struct vring_used_header *tused = (struct vring_used_header *)tused_bytes;
        uint16 want_idx = cur_avail + 1;
        BOOL done = FALSE;
        for (int i = 0; i < 10000; i++) {
            __asm__ volatile ("eieio; sync" : : : "memory");
            if (vio_le16_get(&tused->idx) == want_idx) { done = TRUE; break; }
        }
        (void)done;
        /* Bookkeeping only — TX completion doesn't gate our reply. */
        break;
    }

    case S2_READORPHAN:
        /* Frames not claimed by any type-tracked opener. We don't do
         * per-type filtering, so orphan reads are structurally
         * identical to CMD_READ — fall through. */
        /* fallthrough */
    case CMD_READ: {
        /* PHASE 7j RESULT: immediate-success reply (io_Error=0,
         * DataLength=0) ALSO gurus. So the trigger is not "holding
         * the request forever" — it's ANY successful CMD_READ reply.
         * Roadshow's post-CMD_READ codepath is unconditionally buggy
         * at 0x01855a0c when reached. Stayed with reject-while-not-
         * ONLINE. */
        if (devBase->state != VN_STATE_ONLINE) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
            break;
        }
        struct V1000Opener *op = NULL;
        if (ioreq->ios2_BufferManagement) {
            IExec->ObtainSemaphore(&devBase->opener_lock);
            op = vn_find_opener(devBase, ioreq->ios2_BufferManagement);
        } else {
            IExec->ObtainSemaphore(&devBase->opener_lock);
        }
        if (!op) {
            IExec->ReleaseSemaphore(&devBase->opener_lock);
            ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
            ioreq->ios2_WireError    = S2WERR_GENERIC_ERROR;
            break;
        }
        IExec->AddTail((struct List *)&op->read_queue,
                       (struct Node *)&ioreq->ios2_Req.io_Message.mn_Node);
        IExec->ReleaseSemaphore(&devBase->opener_lock);

        /* Clear IOF_QUICK so the caller's WaitIO actually waits for the
         * reply. Also — since we did NOT set io_Error and are NOT
         * ReplyMsg'ing here, return early to skip the tail ReplyMsg. */
        ioreq->ios2_Req.io_Flags &= ~IOF_QUICK;
        return;
    }

    case S2_GETSTATIONADDRESS: {
        /* SANA-II-NOTES §3.2: write factory MAC into ios2_SrcAddr and
         * currently-configured MAC into ios2_DstAddr. Until Phase 6 wires
         * S2_CONFIGINTERFACE (which lets a caller override the MAC), both
         * addresses are the factory value we cached from RAL/RAH. */
        if (!devBase->hw_present) {
            ioreq->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
            ioreq->ios2_WireError    = S2WERR_UNIT_OFFLINE;
            break;
        }
        for (int i = 0; i < 6; i++) {
            ioreq->ios2_SrcAddr[i] = devBase->mac[i];
            ioreq->ios2_DstAddr[i] = devBase->mac[i];
        }
        break;
    }

    /* Phase 7a: Roadshow-required no-op handlers. Advertising these in
     * vn_supported_cmds without a handler makes Roadshow's DHCP fail
     * with "broadcast access not supported". Returning success (0) is
     * fine because our RCTL already accepts broadcast (BAM) and all
     * multicast (MPE), so subscription requests are effectively already
     * honored at the HW level — no per-address filter table to update. */
    case S2_ADDMULTICASTADDRESS:
    case S2_DELMULTICASTADDRESS:
    case S2_ADDMULTICASTADDRESSES:
    case S2_DELMULTICASTADDRESSES:
        /* Success. HW is already accepting broadcast + all multicast. */
        break;

    case S2_TRACKTYPE:
    case S2_UNTRACKTYPE:
        /* Optional per-ethertype packet-count tracking. We don't keep
         * per-type counters yet; report success so Roadshow (which
         * tracks 0x0800/0x0806/0x86DD at bind time) proceeds. */
        break;

    case S2_SANA2HOOK: {
        /* Phase 8d: install Roadshow's per-opener Sana2Hook. Per
         * rolsen/amy_skeletons pattern and SANA-II Rev 7:
         *   - Caller passes struct Sana2Hook* in ios2_Data
         *   - ios2_DataLength >= 20 (min hook + methods list header)
         *   - Sana2Hook has embedded struct Hook + Tag* s2h_Methods
         *     (array of tag IDs the hook services, terminated with
         *      TAG_END = 0)
         * We store the &s2h_Hook (pointer to embedded Hook) in
         * op->sana2_hook. Copy invocation via CallHookPkt is done in
         * vn_invoke_copy_to/from. */
        struct Sana2Hook *hook = (struct Sana2Hook *)ioreq->ios2_Data;
        if (!hook) hook = (struct Sana2Hook *)ioreq->ios2_StatData;
        if (!hook) {
            ioreq->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT;
            ioreq->ios2_WireError    = S2WERR_NULL_POINTER;
            break;
        }
        if (ioreq->ios2_DataLength && ioreq->ios2_DataLength < 20) {
            ioreq->ios2_Req.io_Error = IOERR_BADLENGTH;
            ioreq->ios2_WireError    = 0;
            break;
        }
        struct V1000Opener *op = NULL;
        if (ioreq->ios2_BufferManagement) {
            IExec->ObtainSemaphore(&devBase->opener_lock);
            op = vn_find_opener(devBase, ioreq->ios2_BufferManagement);
            IExec->ReleaseSemaphore(&devBase->opener_lock);
        }
        if (!op) {
            /* No opener yet — spec permits SANA2HOOK before Open in
             * some flows, but we can't associate anything. Fail with
             * a clean error so Roadshow falls back to tag-list. */
            ioreq->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
            ioreq->ios2_WireError    = S2WERR_GENERIC_ERROR;
            break;
        }
        op->sana2_hook         = &hook->s2h_Hook;
        op->sana2_hook_methods = (ULONG *)hook->s2h_Methods;
        /* Success — Roadshow will now use this hook for Copy*Buff. */
        break;
    }

    case S2_ONEVENT: {
        /* SANA-II event notification. Real drivers (pa6t_eth, etc.)
         * QUEUE the request and reply when a matching event fires
         * via vn_signal_event() below. Roadshow subscribes to
         * S2EVENT_ONLINE at bind time and expects that request to
         * stay pending until CONFIGINTERFACE / ONLINE actually
         * brings the interface online - our earlier reply-immediately
         * pattern likely broke Roadshow's post-bind alloc.
         *
         * Two cases:
         *   1. The event mask matches CURRENT state -> reply now
         *      (Roadshow calls S2_ONEVENT AFTER ONLINE; matches
         *      immediately; no need to queue).
         *   2. Otherwise -> queue on opener's event_queue; wait for
         *      state transition to fire vn_signal_event(). */
        ULONG mask = ioreq->ios2_WireError;
        ULONG match = 0;
        if ((mask & S2EVENT_ONLINE)  && devBase->state == VN_STATE_ONLINE)   match |= S2EVENT_ONLINE;
        if ((mask & S2EVENT_OFFLINE) && devBase->state != VN_STATE_ONLINE)   match |= S2EVENT_OFFLINE;
        if (match) {
            ioreq->ios2_WireError = match;
            break;   /* reply immediately with the matched mask */
        }
        /* No match yet — queue this request. Requires an opener to
         * enqueue on. Fall back to immediate-reply if we can't find
         * an opener (defensive). */
        struct V1000Opener *op = NULL;
        if (ioreq->ios2_BufferManagement) {
            IExec->ObtainSemaphore(&devBase->opener_lock);
            op = vn_find_opener(devBase, ioreq->ios2_BufferManagement);
        } else {
            IExec->ObtainSemaphore(&devBase->opener_lock);
        }
        if (op) {
            ioreq->ios2_Req.io_Flags &= ~IOF_QUICK;
            ioreq->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
            IExec->AddTail((struct List *)&op->event_queue,
                           (struct Node *)&ioreq->ios2_Req.io_Message.mn_Node);
            IExec->ReleaseSemaphore(&devBase->opener_lock);
            return;   /* pending — no ReplyMsg */
        }
        IExec->ReleaseSemaphore(&devBase->opener_lock);
        /* No opener — reply with empty mask, io_Error=0. */
        ioreq->ios2_WireError = 0;
        break;
    }

    case S2_GETGLOBALSTATS: {
        /* Fill Sana2DeviceStats with zeros — we don't yet maintain
         * per-driver counters. Roadshow uses this for stats display
         * only; returning zeros is safer than IOERR_NOCMD. */
        struct Sana2DeviceStats *st = (struct Sana2DeviceStats *)ioreq->ios2_StatData;
        if (!st) st = (struct Sana2DeviceStats *)ioreq->ios2_Data;
        if (st) {
            /* volatile stops GCC optimizing the loop into memset() —
             * memset would pull newlib into a resident driver and fail
             * to link. Same trick as the TX zero-pad in CMD_WRITE. */
            volatile UBYTE *p = (volatile UBYTE *)st;
            for (ULONG i = 0; i < sizeof(struct Sana2DeviceStats); i++) p[i] = 0;
        }
        break;
    }

    default:
        /* Every other command still stubs to NOCMD — Phase 4+ will wire
         * them in one by one, following the same fail-soft pattern. */
        ioreq->ios2_Req.io_Error = IOERR_NOCMD;
        break;
    }

    /* Phase 7a: log this BeginIO invocation into the cmdlog ring so
     * DBG_CMDLOG can dump what Roadshow does. Skip if this is the
     * DBG_CMDLOG or DBG_STATUS call itself — those spam the log. */
    if (ioreq->ios2_Req.io_Command != VN_DBG_CMDLOG
        && ioreq->ios2_Req.io_Command != VN_DBG_STATUS
        && ioreq->ios2_Req.io_Command != VN_DBG_DUMPTX) {
        ULONG i = devBase->cmdlog_head & 31;
        devBase->cmdlog[i].cmd         = ioreq->ios2_Req.io_Command;
        devBase->cmdlog[i].flags_in    = ioreq->ios2_Req.io_Flags;
        devBase->cmdlog[i].ioerr       = ioreq->ios2_Req.io_Error;
        devBase->cmdlog[i].wire        = ioreq->ios2_WireError;
        devBase->cmdlog[i].data_in     = (ULONG)ioreq->ios2_Data;
        devBase->cmdlog[i].bm_in       = (ULONG)ioreq->ios2_BufferManagement;
        devBase->cmdlog[i].ptype_in    = (UWORD)ioreq->ios2_PacketType;
        devBase->cmdlog[i].datalen_out = ioreq->ios2_DataLength;
        for (int b = 0; b < 6; b++) {
            devBase->cmdlog[i].src_out[b] = ioreq->ios2_SrcAddr[b];
            devBase->cmdlog[i].dst_out[b] = ioreq->ios2_DstAddr[b];
        }
        devBase->cmdlog_head++;
    }

    /* Phase 8 finding: disabling ReplyMsg here did NOT stop the DSI
     * @ 0x01855a0c triggered by ping. The crash still fires in
     * virtnet-unit task even with all four kill-switches active
     * (copy hooks bypassed, RX disabled, S2_ONLINE rejected, and
     * ReplyMsg elided). So the crash vector is NOT ReplyMsg's
     * PA_SOFTINT-triggered handler. Investigation must resume with
     * a different hypothesis:
     *   - Roadshow may be Cause()-ing a SoftInt that lands in our
     *     task context regardless of which task's Cause was fired
     *     (softints run in a "borrowed" task per OS4 semantics)
     *   - Or our task struct's ln_Name is being reported for a fault
     *     that actually occurred in another task (e.g., Roadshow's
     *     bsdsocket daemon)
     *   - Or something in our BeginIO PutMsg to begin_port has a
     *     side-effect that runs Roadshow code (unlikely for a
     *     PA_SIGNAL port).
     * For now — keep ReplyMsg, keep the counters. */
    devBase->replymsg_skips++;   /* now a "replies done" counter */
    devBase->last_skipped_cmd = ioreq->ios2_Req.io_Command;
    TRACEF(devBase, "REPLY   cmd=%u ioerr=%d wire=0x%08lx\n",
        (unsigned)ioreq->ios2_Req.io_Command,
        (int)ioreq->ios2_Req.io_Error,
        (unsigned long)ioreq->ios2_WireError);
    IExec->ReplyMsg((struct Message *)ioreq);
    TRACEF(devBase, "REPLYED cmd=%u (returned from ReplyMsg)\n",
        (unsigned)ioreq->ios2_Req.io_Command);
}

LONG _manager_AbortIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq)
{
    /* Phase 6l: honest AbortIO. Walk every opener's read_queue looking
     * for this request; if found, unlink and reply with IOERR_ABORTED.
     * Without this, a caller who SendIO's CMD_READ then AbortIO's it
     * (because timeout / user cancel / test cleanup) will have their
     * WaitIO block forever — the request never gets ReplyMsg'd. */
    struct VirtnetBase *base = (struct VirtnetBase *)Self->Data.LibBase;
    struct ExecIFace *IExec = base->IExec;
    struct V1000Opener *op;
    struct MinNode *node, *next;
    BOOL found = FALSE;

    IExec->ObtainSemaphore(&base->opener_lock);
    for (op = (struct V1000Opener *)base->opener_list.mlh_Head;
         op->node.mln_Succ && !found;
         op = (struct V1000Opener *)op->node.mln_Succ) {
        /* Check read_queue first. */
        for (node = op->read_queue.mlh_Head;
             (next = node->mln_Succ) != NULL;
             node = next) {
            if ((APTR)node == (APTR)&ioreq->ios2_Req.io_Message.mn_Node) {
                IExec->Remove((struct Node *)node);
                found = TRUE;
                break;
            }
        }
        if (found) break;
        /* Then event_queue (Phase 7a: S2_ONEVENT is queued). */
        for (node = op->event_queue.mlh_Head;
             (next = node->mln_Succ) != NULL;
             node = next) {
            if ((APTR)node == (APTR)&ioreq->ios2_Req.io_Message.mn_Node) {
                IExec->Remove((struct Node *)node);
                found = TRUE;
                break;
            }
        }
    }
    IExec->ReleaseSemaphore(&base->opener_lock);

    if (!found) return IOERR_NOCMD;   /* not on any queue */
    ioreq->ios2_Req.io_Error = IOERR_ABORTED;
    IExec->ReplyMsg((struct Message *)ioreq);
    return 0;
}
