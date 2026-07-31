#ifndef SHUNT_H
#define SHUNT_H

#include <exec/devices.h>
#include <exec/interfaces.h>
#include <exec/io.h>
#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <exec/ports.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <devices/sana2.h>
#include <utility/hooks.h>

#include <proto/exec.h>

#define SHUNT_NAME       "sanashunt2.device"
#define SHUNT_VER        0    /* Matches virte1000's DEVVER; kernel appears
                               * to reject Init on non-zero versions - see
                               * shunt.c Init comment. Real revision is
                               * tracked in SHUNT_REV / verstag. */
#define SHUNT_REV        1
#define SHUNT_VERSTRING  "sanashunt.device 0.1 (Roadshow trace proxy) " __DATE__

#define SHUNT_BACKEND_DEVICE "rtl8139.device"
#define SHUNT_BACKEND_UNIT   0

/*
 * sanashunt.device — a SANA-II proxy that forwards every command it
 * receives to a backend device (default: rtl8139.device) and records
 * both the incoming request and the outgoing reply in an in-memory
 * ring buffer.
 *
 * Purpose: when Roadshow can't bind to our virte1000.device, we can
 * bind it to sanashunt instead. Since sanashunt just forwards to
 * rtl8139 (which we know Roadshow already binds cleanly for
 * `interface`), the bind will complete — and the shunt log shows
 * the exact command sequence + field values Roadshow used on a
 * successful bind. Diff against virte1000's cmdlog to identify
 * what field/response virte1000 is missing.
 *
 * NOT a production SANA-II driver — omits CMD_READ/CMD_WRITE
 * forwarding (would require BufferManagement hook translation);
 * those return IOERR_NOCMD. Enough for bind-phase tracing.
 */

/* Per-opener state — one per OpenDevice call. */
struct ShuntOpener
{
    struct MinNode  node;
    APTR            bm_cookie;   /* caller's ios2_BufferManagement tag list */
};

/* Library base. Kernel wires the jump table off the embedded Device. */
struct SanaShuntBase
{
    struct Device      dev_Base;
    struct ExecIFace  *IExec;
    BPTR               dev_SegList;

    /* Backend: rtl8139.device. Opened once at Init; every BeginIO
     * from any caller forwards via fwd_req under fwd_lock. */
    struct MsgPort         *fwd_port;
    struct IOSana2Req      *fwd_req;
    struct SignalSemaphore  fwd_lock;
    BOOL                    backend_open;

    /* Opener tracking (informational — no lookup semantics). */
    struct MinList          opener_list;
    struct SignalSemaphore  opener_lock;

    /* Diagnostic ring: full BEFORE + AFTER field snapshot per BeginIO. */
    volatile ULONG   log_head;
    struct {
        /* Incoming (as Roadshow sent it): */
        UWORD  cmd;
        UBYTE  flags_in;
        UBYTE  _pad0;
        ULONG  data_in;
        ULONG  datalen_in;
        ULONG  bm_in;
        UWORD  ptype_in;
        UBYTE  src_in[6];
        UBYTE  dst_in[6];
        /* Outgoing (rtl8139's reply as we forwarded it back to Roadshow): */
        WORD   ioerr_out;
        ULONG  wire_out;
        ULONG  datalen_out;
        UWORD  ptype_out;
        UBYTE  src_out[6];
        UBYTE  dst_out[6];
    } log[64];
};

/* Private debug command IDs (won't clash with SANA-II or NSCMD ranges). */
#define SHUNT_DBG_STATUS   0xF001   /* returns backend_open + log_head */
#define SHUNT_DBG_CMDLOG   0xF002   /* dumps the log ring into ios2_Data.
                                     * caller supplies buffer of at least
                                     * 64 * sizeof(log entry) bytes.
                                     * DataLength returns head index. */

#endif
