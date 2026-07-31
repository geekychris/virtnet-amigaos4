/*
 * testtx — send one broadcast Ethernet frame via CMD_WRITE.
 *
 * Uses CMD_WRITE now that NSCMD_DEVICEQUERY is implemented. Theory:
 * OS4 exec was rejecting CMD_WRITE with IOERR_UNITBUSY because we
 * hadn't advertised it via the New Style Device supported-commands
 * list. daynaport-amiga/device.c does this too — every SANA-II
 * driver has to declare its command surface via NSCMD_DEVICEQUERY.
 *
 * Sequence:
 *   1. Open + CONFIGINTERFACE + ONLINE
 *   2. NSCMD_DEVICEQUERY sanity check — must return our list
 *   3. TPT baseline via VN_DBG_STATUS
 *   4. CMD_WRITE with a raw broadcast frame
 *   5. TPT again — expect +1
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>
#include <devices/newstyle.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "../include/virtnet.h"

static LONG do_cmd(struct IOSana2Req *req, UWORD cmd)
{
    req->ios2_Req.io_Command = cmd;
    req->ios2_Req.io_Error   = 0;
    req->ios2_WireError      = 0;
    return IExec->DoIO((struct IORequest *)req);
}

static ULONG read_tpt(struct IOSana2Req *req)
{
    do_cmd(req, VN_DBG_STATUS);
    return ((ULONG)req->ios2_SrcAddr[0] << 24)
         | ((ULONG)req->ios2_SrcAddr[1] << 16)
         | ((ULONG)req->ios2_SrcAddr[2] << 8)
         | ((ULONG)req->ios2_SrcAddr[3]);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!port) return 20;
    struct IOSana2Req *req = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, port,
        ASOIOR_Size, sizeof(struct IOSana2Req),
        TAG_END);
    if (!req) { IExec->FreeSysObject(ASOT_PORT, port); return 20; }

    LONG err = IExec->OpenDevice("virtnet.device", 0, (struct IORequest *)req, 0);
    if (err != 0) {
        IDOS->Printf("RESULT: FAIL OpenDevice=%ld\n", (long)err);
        IExec->FreeSysObject(ASOT_IOREQUEST, req);
        IExec->FreeSysObject(ASOT_PORT, port);
        return 20;
    }

    /* Probe NSCMD_DEVICEQUERY — Roadshow-style caller pattern. */
    struct NSDeviceQueryResult nsq = { 0 };
    {
        struct IOStdReq *sreq = (struct IOStdReq *)req;
        req->ios2_Req.io_Command = NSCMD_DEVICEQUERY;
        req->ios2_Req.io_Error   = 0;
        sreq->io_Data            = &nsq;
        sreq->io_Length          = sizeof(nsq);
        LONG rc_nsq = IExec->DoIO((struct IORequest *)req);
        IDOS->Printf("NSCMD_DEVICEQUERY  DoIO=%ld ioerr=%ld type=%u subtype=%u supp=%p\n",
                     (long)rc_nsq, (long)req->ios2_Req.io_Error,
                     (unsigned)nsq.DeviceType, (unsigned)nsq.DeviceSubType,
                     nsq.SupportedCommands);
    }

    for (int i = 0; i < 16; i++) req->ios2_SrcAddr[i] = 0;
    do_cmd(req, S2_CONFIGINTERFACE);
    LONG rc_on = do_cmd(req, S2_ONLINE);
    if (rc_on != 0) { IDOS->Printf("RESULT: FAIL ONLINE=%ld\n", (long)rc_on); goto out; }

    /* e1000 statistics registers self-clear on read. If a previous test
     * in the same suite already sent a frame, TPT would show residual
     * count. Pre-read discards it so the second read is a true baseline. */
    (void)read_tpt(req);
    ULONG tpt_before = read_tpt(req);
    IDOS->Printf("TPT before send: %lu\n", (unsigned long)tpt_before);

    UBYTE frame[60];
    for (int i = 0; i < 60; i++) frame[i] = 0;
    for (int i = 0; i < 6;  i++) frame[i] = 0xff;
    frame[12] = 0x08; frame[13] = 0x06;

    req->ios2_Req.io_Command = CMD_WRITE;
    req->ios2_Req.io_Error   = 0;
    req->ios2_WireError      = 0;
    req->ios2_PacketType     = 0x0806;
    req->ios2_Data           = frame;
    req->ios2_DataLength     = sizeof(frame);
    for (int i = 0; i < 6; i++) req->ios2_DstAddr[i] = 0xff;

    LONG rc = IExec->DoIO((struct IORequest *)req);
    /* Capture the raw CMD_WRITE result BEFORE any further do_cmd calls
     * (do_cmd resets io_Error/wire on each new command). */
    LONG cmdw_rc    = rc;
    LONG cmdw_ioerr = req->ios2_Req.io_Error;
    ULONG cmdw_wire = req->ios2_WireError;
    IDOS->Printf("CMD_WRITE  DoIO=%ld  io_Error=%ld  wire=0x%lx\n",
                 (long)cmdw_rc, (long)cmdw_ioerr, (unsigned long)cmdw_wire);

    ULONG tpt_after = read_tpt(req);
    IDOS->Printf("TPT after send:  %lu (delta=%ld)\n",
                 (unsigned long)tpt_after,
                 (long)((long)tpt_after - (long)tpt_before));

    do_cmd(req, S2_OFFLINE);

out:
    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);

    /* PASS = CMD_WRITE dispatched cleanly AND HW actually sent a frame:
     *   - DoIO returns 0
     *   - io_Error clear
     *   - TPT (Total Packets Transmitted, e1000 hardware stat) incremented
     *     by exactly 1 (proves the frame reached the wire in QEMU) */
    BOOL sent = (cmdw_rc == 0)
             && (cmdw_ioerr == 0)
             && (tpt_after == tpt_before + 1);
    if (sent) {
        IDOS->Printf("RESULT: PASS (frame egressed, TPT +1)\n");
    } else {
        IDOS->Printf("RESULT: FAIL rc=%ld ioerr=%ld wire=0x%lx tpt=%lu->%lu\n",
                     (long)cmdw_rc, (long)cmdw_ioerr,
                     (unsigned long)cmdw_wire,
                     (unsigned long)tpt_before, (unsigned long)tpt_after);
    }
    return sent ? 0 : 20;
}
