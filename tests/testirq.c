/*
 * testirq — verify IRQ delivery end-to-end.
 *
 * Sequence:
 *   1. OpenDevice     (Init runs; state=OFFLINE; IRQ hook installed)
 *   2. S2_CONFIGINTERFACE (OFFLINE → CONFIGURED)
 *   3. S2_ONLINE      (CONFIGURED → ONLINE; RCTL/TCTL/IMS live)
 *   4. VN_DBG_STATUS  → capture baseline counter
 *   5. VN_DBG_FIRE_IRQ → driver writes ICS.LSC; ISR fires, counter++
 *   6. VN_DBG_STATUS  → capture new counter
 * PASS iff counter increased.
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "../include/virtnet.h"   /* VN_STATE_*, VN_DBG_* */

static LONG do_cmd(struct IOSana2Req *req, UWORD cmd)
{
    req->ios2_Req.io_Command = cmd;
    req->ios2_Req.io_Error   = 0;
    req->ios2_WireError      = 0;
    return IExec->DoIO((struct IORequest *)req);
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
        IDOS->Printf("testirq: OpenDevice = %ld (FAIL)\n", (long)err);
        IExec->FreeSysObject(ASOT_IOREQUEST, req);
        IExec->FreeSysObject(ASOT_PORT, port);
        return 20;
    }

    /* SrcAddr all-zeros → "use factory MAC" per SANA-II Rev 7 §3.3. */
    for (int i = 0; i < 16; i++) req->ios2_SrcAddr[i] = 0;
    LONG rc_cfg = do_cmd(req, S2_CONFIGINTERFACE);
    IDOS->Printf("S2_CONFIGINTERFACE  DoIO=%ld ioerr=%ld wire=%lu\n",
                 (long)rc_cfg, (long)req->ios2_Req.io_Error,
                 (unsigned long)req->ios2_WireError);

    LONG rc_on = do_cmd(req, S2_ONLINE);
    IDOS->Printf("S2_ONLINE           DoIO=%ld ioerr=%ld wire=%lu\n",
                 (long)rc_on, (long)req->ios2_Req.io_Error,
                 (unsigned long)req->ios2_WireError);

    do_cmd(req, VN_DBG_STATUS);
    ULONG before = req->ios2_DataLength;
    IDOS->Printf("VN_DBG_STATUS    counter=%lu icr=0x%08lx state=%lu\n",
                 (unsigned long)req->ios2_DataLength,
                 (unsigned long)req->ios2_WireError,
                 (unsigned long)req->ios2_PacketType);

    do_cmd(req, VN_DBG_FIRE_IRQ);
    IDOS->Printf("VN_DBG_FIRE_IRQ  DoIO=  ioerr=%ld  new_counter=%lu\n",
                 (long)req->ios2_Req.io_Error,
                 (unsigned long)req->ios2_DataLength);

    do_cmd(req, VN_DBG_STATUS);
    ULONG after = req->ios2_DataLength;
    IDOS->Printf("VN_DBG_STATUS    counter=%lu icr=0x%08lx state=%lu\n",
                 (unsigned long)req->ios2_DataLength,
                 (unsigned long)req->ios2_WireError,
                 (unsigned long)req->ios2_PacketType);

    /* Try to leave the device idle for the next test run. Errors here
     * don't affect the PASS/FAIL verdict for testirq — Expunge would
     * do the same masking on next reload anyway. */
    do_cmd(req, S2_OFFLINE);

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);

    BOOL pass = (rc_cfg == 0 && req->ios2_Req.io_Error != S2ERR_BAD_STATE)
             && (rc_on == 0)
             && (after > before);
    if (pass) IDOS->Printf("RESULT: PASS\n");
    else      IDOS->Printf("RESULT: FAIL cfg=%ld on=%ld before=%lu after=%lu\n",
                           (long)rc_cfg, (long)rc_on,
                           (unsigned long)before, (unsigned long)after);
    return pass ? 0 : 20;
}
