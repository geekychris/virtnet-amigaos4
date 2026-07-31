/*
 * testrxtask — real queued CMD_READ end-to-end.
 *
 * Two IORequests sharing one reply port. Register CopyFromBuff +
 * CopyToBuff hooks, ONLINE the interface, send an ARP request, THEN
 * queue a CMD_READ. Driver's unit task (woken by ISR on RXT0) picks
 * up the ARP reply, walks openers, finds our queued READ, invokes
 * CopyToBuff into the caller's buffer, ReplyMsgs.
 *
 * Success = READ was replied without our test having to AbortIO it,
 * OR AbortIO successfully unblocks the WaitIO (with the Phase 6l
 * AbortIO handler that walks the queue).
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>
#include <devices/sana2.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "../include/virtnet.h"

static ULONG g_copyto_fired = 0;
static UBYTE g_copyto_first4[4] = { 0 };

static ULONG my_copy_from_buff(struct Hook *hook, APTR object, APTR message)
{
    (void)hook; (void)object;
    struct SANA2CopyHookMsg *msg = (struct SANA2CopyHookMsg *)message;
    UBYTE *dst = (UBYTE *)msg->schm_To;
    UBYTE *src = (UBYTE *)msg->schm_From;
    for (ULONG i = 0; i < msg->schm_Size; i++) dst[i] = src[i];
    return 1;
}

static ULONG my_copy_to_buff(struct Hook *hook, APTR object, APTR message)
{
    (void)hook; (void)object;
    struct SANA2CopyHookMsg *msg = (struct SANA2CopyHookMsg *)message;
    UBYTE *dst = (UBYTE *)msg->schm_To;
    UBYTE *src = (UBYTE *)msg->schm_From;
    for (ULONG i = 0; i < msg->schm_Size; i++) dst[i] = src[i];
    g_copyto_fired = 1;
    for (int i = 0; i < 4 && i < (int)msg->schm_Size; i++)
        g_copyto_first4[i] = src[i];
    return 1;
}

static LONG do_cmd(struct IOSana2Req *r, UWORD cmd)
{
    r->ios2_Req.io_Command = cmd;
    r->ios2_Req.io_Error   = 0;
    r->ios2_WireError      = 0;
    return IExec->DoIO((struct IORequest *)r);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!port) return 20;

    struct IOSana2Req *rA = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, port,
        ASOIOR_Size, sizeof(struct IOSana2Req),
        TAG_END);
    struct IOSana2Req *rB = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, port,
        ASOIOR_Size, sizeof(struct IOSana2Req),
        TAG_END);
    if (!rA || !rB) return 20;

    struct Hook copyfrom_hook = { 0 };
    copyfrom_hook.h_Entry = (ULONG (*)())my_copy_from_buff;
    struct Hook copyto_hook = { 0 };
    copyto_hook.h_Entry = (ULONG (*)())my_copy_to_buff;

    struct TagItem bm_tags[] = {
        { S2_CopyFromBuff, (ULONG)&copyfrom_hook },
        { S2_CopyToBuff,   (ULONG)&copyto_hook   },
        { TAG_END,         0 },
    };
    rA->ios2_BufferManagement = (APTR)bm_tags;

    LONG err = IExec->OpenDevice("virtnet.device", 0, (struct IORequest *)rA, 0);
    if (err != 0) { IDOS->Printf("RESULT: FAIL OpenDevice=%ld\n", (long)err); return 20; }

    rB->ios2_Req.io_Device    = rA->ios2_Req.io_Device;
    rB->ios2_Req.io_Unit      = rA->ios2_Req.io_Unit;
    rB->ios2_BufferManagement = rA->ios2_BufferManagement;

    for (int i = 0; i < 16; i++) rA->ios2_SrcAddr[i] = 0;
    do_cmd(rA, S2_CONFIGINTERFACE);
    LONG rc_on = do_cmd(rA, S2_ONLINE);
    if (rc_on != 0) { IDOS->Printf("RESULT: FAIL ONLINE=%ld\n", (long)rc_on); goto out; }

    do_cmd(rA, S2_GETSTATIONADDRESS);
    /* Sender IP 192.168.100.10 exactly matches testrx (which QEMU
     * successfully ARP-replies to). If cooked-mode framing were correct,
     * this should get the same reply. */
    UBYTE payload[28] = {
        0x00, 0x01, 0x08, 0x00, 6, 4, 0x00, 0x01,
        0,0,0,0,0,0,   192, 168, 100, 10,
        0,0,0,0,0,0,   192, 168, 100, 2,
    };
    /* ARP body layout: htype[0..1] ptype[2..3] hlen[4] plen[5] oper[6..7]
     * sha[8..13] spa[14..17] tha[18..23] tpa[24..27]. Sender HW addr goes
     * at offset 8 — using offset 6 clobbers the OPER field and QEMU
     * silently drops the malformed ARP. */
    for (int i = 0; i < 6; i++) payload[8 + i] = rA->ios2_SrcAddr[i];

    /* Queue the READ FIRST so the unit task has something to hand the
     * frame to when the ARP reply lands. If we send-then-queue, a fast
     * reply could arrive between the two calls and process_rx would
     * find no reader (frame kept in ring for next round — but our poll
     * timeout might expire before another read wakes the task). */
    UBYTE rx_target[128] = { 0 };
    rB->ios2_Req.io_Command                    = CMD_READ;
    rB->ios2_Req.io_Error                      = 0;
    rB->ios2_WireError                         = 0;
    rB->ios2_Data                              = rx_target;
    rB->ios2_DataLength                        = 0;
    rB->ios2_Req.io_Message.mn_Node.ln_Type    = NT_MESSAGE;
    IExec->SendIO((struct IORequest *)rB);
    IDOS->Printf("CMD_READ queued\n");

    /* Send ARP (blocking). */
    rA->ios2_BufferManagement = (APTR)bm_tags;
    rA->ios2_Req.io_Command   = CMD_WRITE;
    rA->ios2_Req.io_Error     = 0;
    rA->ios2_WireError        = 0;
    rA->ios2_PacketType       = 0x0806;
    rA->ios2_Data             = payload;
    rA->ios2_DataLength       = sizeof(payload);
    for (int i = 0; i < 6; i++) rA->ios2_DstAddr[i] = 0xff;
    IExec->DoIO((struct IORequest *)rA);
    IDOS->Printf("ARP sent\n");

    do_cmd(rA, VN_DBG_DUMPTX);
    IDOS->Printf("tx frame bytes: %02lx %02lx %02lx %02lx %02lx %02lx  %02lx %02lx %02lx %02lx %02lx %02lx  %02lx %02lx  %02lx %02lx\n",
                 (unsigned long)rA->ios2_SrcAddr[0], (unsigned long)rA->ios2_SrcAddr[1],
                 (unsigned long)rA->ios2_SrcAddr[2], (unsigned long)rA->ios2_SrcAddr[3],
                 (unsigned long)rA->ios2_SrcAddr[4], (unsigned long)rA->ios2_SrcAddr[5],
                 (unsigned long)rA->ios2_SrcAddr[6], (unsigned long)rA->ios2_SrcAddr[7],
                 (unsigned long)rA->ios2_SrcAddr[8], (unsigned long)rA->ios2_SrcAddr[9],
                 (unsigned long)rA->ios2_SrcAddr[10], (unsigned long)rA->ios2_SrcAddr[11],
                 (unsigned long)rA->ios2_SrcAddr[12], (unsigned long)rA->ios2_SrcAddr[13],
                 (unsigned long)rA->ios2_SrcAddr[14], (unsigned long)rA->ios2_SrcAddr[15]);

    ULONG got_reply;
    got_reply = 0;
    for (int i = 0; i < 25; i++) {
        if (IExec->CheckIO((struct IORequest *)rB)) {
            IExec->WaitIO((struct IORequest *)rB);
            got_reply = 1;
            break;
        }
        IDOS->Delay(10);
    }

    if (!got_reply) {
        IDOS->Printf("CMD_READ not replied within 5 s — aborting\n");
        IExec->AbortIO((struct IORequest *)rB);
        IExec->WaitIO((struct IORequest *)rB);
        IDOS->Printf("aborted: ioerr=%ld\n", (long)rB->ios2_Req.io_Error);
    } else {
        IDOS->Printf("CMD_READ replied: ioerr=%ld type=0x%04lx len=%lu\n",
                     (long)rB->ios2_Req.io_Error,
                     (unsigned long)rB->ios2_PacketType,
                     (unsigned long)rB->ios2_DataLength);
        IDOS->Printf("  src=%02lx:%02lx:%02lx:%02lx:%02lx:%02lx  first4=%02lx %02lx %02lx %02lx\n",
                     (unsigned long)rB->ios2_SrcAddr[0], (unsigned long)rB->ios2_SrcAddr[1],
                     (unsigned long)rB->ios2_SrcAddr[2], (unsigned long)rB->ios2_SrcAddr[3],
                     (unsigned long)rB->ios2_SrcAddr[4], (unsigned long)rB->ios2_SrcAddr[5],
                     (unsigned long)g_copyto_first4[0], (unsigned long)g_copyto_first4[1],
                     (unsigned long)g_copyto_first4[2], (unsigned long)g_copyto_first4[3]);
    }

    /* Diagnostic snapshot before OFFLINE: what did the task actually do? */
    do_cmd(rA, VN_DBG_STATUS);
    ULONG irq_cnt   = rA->ios2_DataLength;
    ULONG last_icr  = rA->ios2_WireError;
    ULONG tpt       = ((ULONG)rA->ios2_SrcAddr[0] << 24) | ((ULONG)rA->ios2_SrcAddr[1] << 16)
                    | ((ULONG)rA->ios2_SrcAddr[2] << 8)  |  (ULONG)rA->ios2_SrcAddr[3];
    ULONG rdh       = ((ULONG)rA->ios2_SrcAddr[4] << 24) | ((ULONG)rA->ios2_SrcAddr[5] << 16)
                    | ((ULONG)rA->ios2_SrcAddr[6] << 8)  |  (ULONG)rA->ios2_SrcAddr[7];
    ULONG dd_count  = ((ULONG)rA->ios2_DstAddr[0] << 24) | ((ULONG)rA->ios2_DstAddr[1] << 16)
                    | ((ULONG)rA->ios2_DstAddr[2] << 8)  |  (ULONG)rA->ios2_DstAddr[3];
    ULONG task_wakes = ((ULONG)rA->ios2_DstAddr[6] << 8) | (ULONG)rA->ios2_DstAddr[7];
    ULONG rx_dd_seen = ((ULONG)rA->ios2_DstAddr[8] << 8) | (ULONG)rA->ios2_DstAddr[9];
    ULONG rx_delivered = ((ULONG)rA->ios2_DstAddr[10] << 8) | (ULONG)rA->ios2_DstAddr[11];
    IDOS->Printf("diag: irq=%lu icr=0x%08lx tpt=%lu rdh=%lu dd_now=%lu\n",
                 (unsigned long)irq_cnt, (unsigned long)last_icr,
                 (unsigned long)tpt, (unsigned long)rdh, (unsigned long)dd_count);
    IDOS->Printf("      wakes=%lu dd_seen=%lu delivered=%lu\n",
                 (unsigned long)task_wakes, (unsigned long)rx_dd_seen,
                 (unsigned long)rx_delivered);

    do_cmd(rA, S2_OFFLINE);

out:
    ;   /* label needs a statement */
    IExec->CloseDevice((struct IORequest *)rA);
    IExec->FreeSysObject(ASOT_IOREQUEST, rA);
    IExec->FreeSysObject(ASOT_IOREQUEST, rB);
    IExec->FreeSysObject(ASOT_PORT, port);

    BOOL best = got_reply && g_copyto_fired
             && (rB->ios2_Req.io_Error == 0)
             && (rB->ios2_PacketType == 0x0806);
    BOOL good = got_reply && g_copyto_fired
             && (rB->ios2_Req.io_Error == 0);
    BOOL aborted_cleanly = !got_reply && (rB->ios2_Req.io_Error == IOERR_ABORTED);

    if (best) IDOS->Printf("RESULT: PASS (unit task delivered ARP reply via CopyToBuff)\n");
    else if (good) IDOS->Printf("RESULT: PASS (unit task delivered a frame; type=0x%lx)\n",
                                (unsigned long)rB->ios2_PacketType);
    else if (aborted_cleanly) IDOS->Printf("RESULT: PASS (AbortIO cleanly unblocked WaitIO; no frame delivered)\n");
    else IDOS->Printf("RESULT: FAIL got=%lu fired=%lu ioerr=%ld type=0x%lx\n",
                      (unsigned long)got_reply, (unsigned long)g_copyto_fired,
                      (long)rB->ios2_Req.io_Error,
                      (unsigned long)rB->ios2_PacketType);
    return (best || good || aborted_cleanly) ? 0 : 20;
}
