/*
 * sanashunt.device — Roadshow trace proxy over rtl8139.device.
 *
 * See ../include/shunt.h for the "why". Structure copied from
 * virte1000/src/device.c — same OS4 device-driver skeleton (resident
 * tag in writable .data, DeviceManagerInterface vector table, 68k
 * jump table, init-tag CLT_DataSize etc.).
 *
 * BeginIO strategy:
 *   1. Snapshot the incoming ioreq fields into the log ring
 *   2. Acquire fwd_lock (protects the shared backend IORequest)
 *   3. Copy caller's ios2_* into fwd_req (but keep fwd_req's own
 *      io_Device / io_Unit / io_Message so the reply-port routes to
 *      us, not to the caller)
 *   4. Synchronously DoIO(fwd_req) — rtl8139 handles the request
 *   5. Copy fwd_req's post-DoIO fields back into caller's ioreq
 *   6. Release fwd_lock
 *   7. Log the outgoing fields
 *   8. ReplyMsg(caller) unless IOF_QUICK
 *
 * CMD_READ / CMD_WRITE / S2_BROADCAST are NOT forwarded — they'd
 * need BufferManagement hook translation (caller's CopyToBuff hook
 * is defined in caller's address space; rtl8139 doesn't know about
 * it). Return IOERR_NOCMD. Bind-phase tracing doesn't need them.
 */

#include "shunt.h"

#include <exec/exectags.h>
#include <exec/interfaces.h>
#include <exec/resident.h>
#include <exec/errors.h>
#include <exec/execbase.h>

#include <devices/newstyle.h>

#include <dos/dos.h>
#include <dos/dosextens.h>
#include <interfaces/dos.h>

/* File-based Init log. IMPORTANT: this function must NOT declare any
 * mutable statics of its own — anything with a runtime initializer
 * lands in .data BEFORE dev_res, which pushes the resident tag off
 * the 4-byte alignment OS4's ResidentScan expects. That silently
 * breaks AutoInit (`version` still sees the tag string but the
 * kernel never invokes _manager_Init). Every call opens the file
 * afresh in MODE_READWRITE + seek-end so multiple lines accumulate. */
static void shunt_init_log(struct ExecIFace *IExec, const char *line)
{
    struct Library *DOSBase = IExec->OpenLibrary((CONST_STRPTR)"dos.library", 51);
    if (!DOSBase) return;
    struct DOSIFace *IDOS = (struct DOSIFace *)
        IExec->GetInterface(DOSBase, (CONST_STRPTR)"main", 1, NULL);
    if (IDOS) {
        BPTR fh = IDOS->Open((CONST_STRPTR)"RAM:sanashunt-init.log", MODE_READWRITE);
        if (!fh) fh = IDOS->Open((CONST_STRPTR)"RAM:sanashunt-init.log", MODE_NEWFILE);
        if (fh) {
            IDOS->ChangeFilePosition(fh, 0, OFFSET_END);
            IDOS->FPuts(fh, (CONST_STRPTR)line);
            IDOS->FPuts(fh, (CONST_STRPTR)"\n");
            IDOS->Close(fh);
        }
        IExec->DropInterface((struct Interface *)IDOS);
    }
    IExec->CloseLibrary(DOSBase);
}

/* ---------- Manager interface (Obtain/Release/vectors) ---------- */
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

extern struct Library *_manager_Init(struct Library *library, BPTR seglist, struct Interface *exec);
extern struct SanaShuntBase *_manager_Open(struct DeviceManagerInterface *Self,
                                           struct IOSana2Req *ioreq,
                                           ULONG unitNum, ULONG flags);
extern BPTR _manager_Close(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq);
extern BPTR _manager_Expunge(struct DeviceManagerInterface *Self);
extern void _manager_BeginIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq);
extern LONG _manager_AbortIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq);

static const APTR _manager_Vectors[] = {
    (APTR)_manager_Obtain,
    (APTR)_manager_Release,
    (APTR)NULL,             /* Expunge slot on Interface — unused */
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

/* 68k-compat jump table (same rationale as virte1000). */
static const APTR _manager_Vectors68K[] = {
    (APTR)_manager_Open,
    (APTR)_manager_Close,
    (APTR)_manager_Expunge,
    (APTR)NULL,
    (APTR)_manager_BeginIO,
    (APTR)_manager_AbortIO,
    (APTR)-1,
};

static const char verstag[] __attribute__((used)) = "\0$VER: " SHUNT_VERSTRING;

static struct TagItem dev_init_tags[] = {
    {CLT_DataSize,     sizeof(struct SanaShuntBase)},
    {CLT_Interfaces,   (ULONG)devInterfaces},
    {CLT_InitFunc,     (ULONG)_manager_Init},
    {CLT_Vector68K,    (ULONG)_manager_Vectors68K},
    {CLT_NoLegacyIFace, FALSE},
    {TAG_END,          0},
};

/* Resident struct — writable .data (VSD lesson). */
static struct Resident dev_res __attribute__((used)) = {
    RTC_MATCHWORD,
    (struct Resident *)&dev_res,
    (struct Resident *)(&dev_res + 1),
    RTF_NATIVE | RTF_COLDSTART | RTF_AUTOINIT,
    SHUNT_VER,
    NT_DEVICE,
    0,
    SHUNT_NAME,
    SHUNT_VERSTRING,
    (APTR)dev_init_tags,
};

/* Shell entry — device, not runnable. */
int _start(char *argstring, int arglen, struct ExecBase *sysbase)
{
    (void)argstring; (void)arglen;
    struct ExecIFace *IExec = (struct ExecIFace *)sysbase->MainInterface;
    IExec->DebugPrintF(SHUNT_NAME " is a device — install to DEVS:Networks/ "
                       "and bind via Roadshow AddNetInterface. "
                       "Cannot run from shell.\n");
    return 20;
}

/* ---------- Init: STRIPPED TO MINIMUM for shunt-bootstrap debugging.
 * If Open succeeds with this, our Init body is the problem. If Open
 * still returns -1, resident tag / kernel wiring is the problem. */
struct Library *_manager_Init(struct Library *library, BPTR seglist, struct Interface *exec)
{
    (void)seglist; (void)exec;
    struct SanaShuntBase *base = (struct SanaShuntBase *)library;
    struct ExecIFace *IExec = (struct ExecIFace *)exec;

    shunt_init_log(IExec, "MINIMAL Init entered");
    return library;
}

/* Full Init disabled during debug — see above. */
struct Library *_manager_Init_full(struct Library *library, BPTR seglist, struct Interface *exec)
{
    struct SanaShuntBase *base = (struct SanaShuntBase *)library;
    struct ExecIFace *IExec = (struct ExecIFace *)exec;

    shunt_init_log(IExec, "Init entered");
    IExec->DebugPrintF("[sanashunt] Init: " SHUNT_VERSTRING "\n");
    shunt_init_log(IExec, "Init: past DebugPrintF");

    base->IExec       = IExec;
    base->dev_SegList = seglist;
    IExec->NewList((struct List *)&base->opener_list);
    IExec->InitSemaphore(&base->opener_lock);
    IExec->InitSemaphore(&base->fwd_lock);
    base->backend_open = FALSE;
    base->log_head     = 0;
    /* volatile zero-fill via loop stops GCC from calling memset (which
     * would pull newlib into a resident driver and fail to link). */
    {
        volatile UBYTE *p = (volatile UBYTE *)base->log;
        for (ULONG i = 0; i < sizeof(base->log); i++) p[i] = 0;
    }

    /* Allocate a MsgPort + IOSana2Req for backend forwarding. Backend
     * replies land on this port. Since we DoIO synchronously under
     * fwd_lock, no need for a real signal handler — DoIO Waits on the
     * port for us. */
    base->fwd_port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!base->fwd_port) {
        IExec->DebugPrintF("[sanashunt] Init: fwd_port alloc failed\n");
        shunt_init_log(IExec, "Init: fwd_port alloc FAILED");
        return NULL;
    }
    shunt_init_log(IExec, "Init: fwd_port alloc OK");
    base->fwd_req = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, base->fwd_port,
        ASOIOR_Size,      sizeof(struct IOSana2Req),
        TAG_END);
    if (!base->fwd_req) {
        IExec->DebugPrintF("[sanashunt] Init: fwd_req alloc failed\n");
        shunt_init_log(IExec, "Init: fwd_req alloc FAILED");
        IExec->FreeSysObject(ASOT_PORT, base->fwd_port);
        return NULL;
    }
    shunt_init_log(IExec, "Init: fwd_req alloc OK");

    /* Open the backend. If this fails (e.g. rtl8139 doesn't allow
     * second opener while Roadshow already holds it bound), we still
     * return the base — Roadshow's bind attempt will surface via
     * missing forwarded replies, which is diagnostic in itself. */
    LONG err = IExec->OpenDevice(
        (CONST_STRPTR)SHUNT_BACKEND_DEVICE, SHUNT_BACKEND_UNIT,
        (struct IORequest *)base->fwd_req, 0);
    if (err) {
        IExec->DebugPrintF("[sanashunt] Init: OpenDevice(%s, %lu) failed err=%ld\n",
                           SHUNT_BACKEND_DEVICE, (unsigned long)SHUNT_BACKEND_UNIT,
                           (long)err);
        shunt_init_log(IExec, "Init: backend OpenDevice FAILED (leaving backend_open=FALSE)");
        /* Leave backend_open=FALSE. Init still succeeds so Roadshow
         * can Open sanashunt and we can log its command attempts. */
    } else {
        base->backend_open = TRUE;
        IExec->DebugPrintF("[sanashunt] Init: backend %s unit %lu opened OK\n",
                           SHUNT_BACKEND_DEVICE, (unsigned long)SHUNT_BACKEND_UNIT);
        shunt_init_log(IExec, "Init: backend OpenDevice OK");
    }

    shunt_init_log(IExec, "Init: returning library");
    return library;
}

/* ---------- Open ---------- */
struct SanaShuntBase *_manager_Open(struct DeviceManagerInterface *Self,
                                    struct IOSana2Req *ioreq,
                                    ULONG unitNum, ULONG flags)
{
    struct SanaShuntBase *base = (struct SanaShuntBase *)Self->Data.LibBase;
    struct ExecIFace *IExec = base->IExec;
    (void)unitNum; (void)flags;

    base->dev_Base.dd_Library.lib_OpenCnt++;
    base->dev_Base.dd_Library.lib_Flags &= ~LIBF_DELEXP;

    /* Track opener (informational — cookie == tag list; not
     * rewritten, since Roadshow re-sends its tag list every BeginIO). */
    struct ShuntOpener *op = IExec->AllocVecTags(
        sizeof(struct ShuntOpener),
        AVT_Type,           MEMF_SHARED,
        AVT_ClearWithValue, 0,
        TAG_END);
    if (op) {
        op->bm_cookie = ioreq->ios2_BufferManagement;
        IExec->ObtainSemaphore(&base->opener_lock);
        IExec->AddTail((struct List *)&base->opener_list, (struct Node *)&op->node);
        IExec->ReleaseSemaphore(&base->opener_lock);
    }

    ioreq->ios2_Req.io_Device = (struct Device *)base;
    ioreq->ios2_Req.io_Unit   = (struct Unit *)base;   /* placeholder */
    ioreq->ios2_Req.io_Error  = 0;
    ioreq->ios2_WireError     = 0;
    return base;
}

/* ---------- Close ---------- */
BPTR _manager_Close(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq)
{
    struct SanaShuntBase *base = (struct SanaShuntBase *)Self->Data.LibBase;
    struct ExecIFace *IExec = base->IExec;
    BPTR seglist = (BPTR)NULL;

    /* Remove this caller's opener from the list. */
    APTR cookie = ioreq->ios2_BufferManagement;
    struct ShuntOpener *op, *found = NULL;
    IExec->ObtainSemaphore(&base->opener_lock);
    for (op = (struct ShuntOpener *)base->opener_list.mlh_Head;
         op->node.mln_Succ;
         op = (struct ShuntOpener *)op->node.mln_Succ) {
        if (op->bm_cookie == cookie) { found = op; break; }
    }
    if (found) IExec->Remove((struct Node *)&found->node);
    IExec->ReleaseSemaphore(&base->opener_lock);
    if (found) IExec->FreeVec(found);

    ioreq->ios2_Req.io_Unit   = (struct Unit *)-1;
    ioreq->ios2_Req.io_Device = (struct Device *)-1;

    base->dev_Base.dd_Library.lib_OpenCnt--;

#ifdef DEBUG
    if (base->dev_Base.dd_Library.lib_OpenCnt == 0)
        seglist = _manager_Expunge(Self);
#else
    if (base->dev_Base.dd_Library.lib_OpenCnt == 0
        && (base->dev_Base.dd_Library.lib_Flags & LIBF_DELEXP))
        seglist = _manager_Expunge(Self);
#endif
    return seglist;
}

/* ---------- Expunge ---------- */
BPTR _manager_Expunge(struct DeviceManagerInterface *Self)
{
    struct SanaShuntBase *base = (struct SanaShuntBase *)Self->Data.LibBase;
    struct ExecIFace *IExec = base->IExec;

    if (base->dev_Base.dd_Library.lib_OpenCnt != 0) {
        base->dev_Base.dd_Library.lib_Flags |= LIBF_DELEXP;
        return (BPTR)NULL;
    }

    BPTR seglist = base->dev_SegList;
    IExec->Remove((struct Node *)&base->dev_Base.dd_Library.lib_Node);

    if (base->backend_open && base->fwd_req) {
        IExec->CloseDevice((struct IORequest *)base->fwd_req);
        base->backend_open = FALSE;
    }
    if (base->fwd_req)  { IExec->FreeSysObject(ASOT_IOREQUEST, base->fwd_req);  base->fwd_req  = NULL; }
    if (base->fwd_port) { IExec->FreeSysObject(ASOT_PORT,      base->fwd_port); base->fwd_port = NULL; }

    IExec->DeleteLibrary(&base->dev_Base.dd_Library);
    IExec->DebugPrintF("[sanashunt] Expunge: goodbye.\n");
    return seglist;
}

/* ---------- BeginIO ---------- */
static void shunt_log_entry(struct SanaShuntBase *base,
                            struct IOSana2Req *in,   /* caller's ioreq — for before-snapshot */
                            BOOL before)             /* TRUE = record inputs, FALSE = record outputs */
{
    ULONG i = base->log_head & 63;
    if (before) {
        base->log[i].cmd        = in->ios2_Req.io_Command;
        base->log[i].flags_in   = in->ios2_Req.io_Flags;
        base->log[i].data_in    = (ULONG)in->ios2_Data;
        base->log[i].datalen_in = in->ios2_DataLength;
        base->log[i].bm_in      = (ULONG)in->ios2_BufferManagement;
        base->log[i].ptype_in   = (UWORD)in->ios2_PacketType;
        for (int b = 0; b < 6; b++) {
            base->log[i].src_in[b] = in->ios2_SrcAddr[b];
            base->log[i].dst_in[b] = in->ios2_DstAddr[b];
        }
    } else {
        base->log[i].ioerr_out   = in->ios2_Req.io_Error;
        base->log[i].wire_out    = in->ios2_WireError;
        base->log[i].datalen_out = in->ios2_DataLength;
        base->log[i].ptype_out   = (UWORD)in->ios2_PacketType;
        for (int b = 0; b < 6; b++) {
            base->log[i].src_out[b] = in->ios2_SrcAddr[b];
            base->log[i].dst_out[b] = in->ios2_DstAddr[b];
        }
        base->log_head++;   /* advance only after full BEFORE + AFTER */
    }
}

void _manager_BeginIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq)
{
    struct SanaShuntBase *base = (struct SanaShuntBase *)Self->Data.LibBase;
    struct ExecIFace *IExec = base->IExec;

    /* Skip our own debug commands. */
    UWORD cmd = ioreq->ios2_Req.io_Command;
    if (cmd == SHUNT_DBG_STATUS) {
        ioreq->ios2_DataLength = base->log_head;
        ioreq->ios2_PacketType = base->backend_open ? 1 : 0;
        ioreq->ios2_Req.io_Error = 0;
        goto reply;
    }
    if (cmd == SHUNT_DBG_CMDLOG) {
        UBYTE *out = (UBYTE *)ioreq->ios2_Data;
        ULONG entry_size = sizeof(base->log[0]);
        if (out && ioreq->ios2_DataLength >= 64 * entry_size) {
            /* Straight memcpy via byte-at-a-time (no newlib memcpy). */
            volatile UBYTE *src = (volatile UBYTE *)base->log;
            volatile UBYTE *dst = (volatile UBYTE *)out;
            ULONG total = 64 * entry_size;
            for (ULONG i = 0; i < total; i++) dst[i] = src[i];
            ioreq->ios2_DataLength = base->log_head;
        }
        ioreq->ios2_Req.io_Error = 0;
        goto reply;
    }

    /* CMD_READ / CMD_WRITE / S2_BROADCAST can't be forwarded without
     * hook translation. Return IOERR_NOCMD so Roadshow moves on. */
    if (cmd == CMD_READ || cmd == CMD_WRITE || cmd == S2_BROADCAST
        || cmd == S2_MULTICAST || cmd == S2_READORPHAN) {
        /* Still log so we see when Roadshow tried. */
        shunt_log_entry(base, ioreq, TRUE);
        ioreq->ios2_Req.io_Error = IOERR_NOCMD;
        ioreq->ios2_WireError    = S2WERR_GENERIC_ERROR;
        shunt_log_entry(base, ioreq, FALSE);
        goto reply;
    }

    /* Everything else — forward to backend. */
    shunt_log_entry(base, ioreq, TRUE);

    if (!base->backend_open) {
        ioreq->ios2_Req.io_Error = IOERR_OPENFAIL;
        shunt_log_entry(base, ioreq, FALSE);
        goto reply;
    }

    IExec->ObtainSemaphore(&base->fwd_lock);

    /* Copy caller's ios2 fields into fwd_req. Preserve fwd_req's own
     * io_Device / io_Unit / io_Message (reply port + node) so the
     * backend replies to US. */
    struct IOSana2Req *fr = base->fwd_req;
    fr->ios2_Req.io_Command = ioreq->ios2_Req.io_Command;
    fr->ios2_Req.io_Flags   = ioreq->ios2_Req.io_Flags & ~IOF_QUICK;  /* DoIO wants no QUICK */
    fr->ios2_Req.io_Error   = 0;
    fr->ios2_WireError      = ioreq->ios2_WireError;
    fr->ios2_PacketType     = ioreq->ios2_PacketType;
    fr->ios2_DataLength     = ioreq->ios2_DataLength;
    fr->ios2_Data           = ioreq->ios2_Data;   /* same pointer — caller's buffer */
    fr->ios2_StatData       = ioreq->ios2_StatData;
    fr->ios2_BufferManagement = ioreq->ios2_BufferManagement;
    for (int b = 0; b < SANA2_MAX_ADDR_BYTES; b++) {
        fr->ios2_SrcAddr[b] = ioreq->ios2_SrcAddr[b];
        fr->ios2_DstAddr[b] = ioreq->ios2_DstAddr[b];
    }

    /* Synchronous forward. */
    IExec->DoIO((struct IORequest *)fr);

    /* Copy reply fields back. */
    ioreq->ios2_Req.io_Error = fr->ios2_Req.io_Error;
    ioreq->ios2_WireError    = fr->ios2_WireError;
    ioreq->ios2_PacketType   = fr->ios2_PacketType;
    ioreq->ios2_DataLength   = fr->ios2_DataLength;
    /* ios2_Data was the same pointer; backend wrote through it. */
    for (int b = 0; b < SANA2_MAX_ADDR_BYTES; b++) {
        ioreq->ios2_SrcAddr[b] = fr->ios2_SrcAddr[b];
        ioreq->ios2_DstAddr[b] = fr->ios2_DstAddr[b];
    }

    IExec->ReleaseSemaphore(&base->fwd_lock);

    shunt_log_entry(base, ioreq, FALSE);

reply:
    if (ioreq->ios2_Req.io_Flags & IOF_QUICK) return;
    IExec->ReplyMsg((struct Message *)ioreq);
}

/* ---------- AbortIO ---------- */
LONG _manager_AbortIO(struct DeviceManagerInterface *Self, struct IOSana2Req *ioreq)
{
    /* Nothing queued in the shunt itself (all forwarding is
     * synchronous under fwd_lock). Return NOCMD — caller should not
     * see any request pending anyway. */
    (void)Self; (void)ioreq;
    return IOERR_NOCMD;
}
