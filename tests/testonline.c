/*
 * testonline — exercise the SANA-II state machine.
 *
 * Cycle:  OFFLINE → CONFIGURED → ONLINE → CONFIGURED → ONLINE
 * Verify: state field via VN_DBG_STATUS after each step.
 * Also: second S2_CONFIGINTERFACE from CONFIGURED must return
 *       S2ERR_BAD_STATE / S2WERR_IS_CONFIGURED (spec §3.3).
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>

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

static ULONG read_state(struct IOSana2Req *req)
{
    do_cmd(req, VN_DBG_STATUS);
    return req->ios2_PacketType;
}

static const char *state_name(ULONG s)
{
    switch (s) {
    case VN_STATE_OFFLINE:    return "OFFLINE";
    case VN_STATE_CONFIGURED: return "CONFIGURED";
    case VN_STATE_ONLINE:     return "ONLINE";
    default:                     return "<unknown>";
    }
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
        IDOS->Printf("testonline: OpenDevice = %ld (FAIL)\n", (long)err);
        IExec->FreeSysObject(ASOT_IOREQUEST, req);
        IExec->FreeSysObject(ASOT_PORT, port);
        return 20;
    }

    ULONG s0 = read_state(req);
    IDOS->Printf("initial state: %lu (%s)\n", (unsigned long)s0, state_name(s0));

    for (int i = 0; i < 16; i++) req->ios2_SrcAddr[i] = 0;   /* factory MAC */
    LONG rc_cfg = do_cmd(req, S2_CONFIGINTERFACE);
    ULONG s1 = read_state(req);
    IDOS->Printf("after CONFIGINTERFACE: rc=%ld ioerr=%ld state=%s\n",
                 (long)rc_cfg, (long)req->ios2_Req.io_Error, state_name(s1));

    LONG rc_on1 = do_cmd(req, S2_ONLINE);
    ULONG s2 = read_state(req);
    IDOS->Printf("after ONLINE:          rc=%ld ioerr=%ld state=%s\n",
                 (long)rc_on1, (long)req->ios2_Req.io_Error, state_name(s2));

    LONG rc_off = do_cmd(req, S2_OFFLINE);
    ULONG s3 = read_state(req);
    IDOS->Printf("after OFFLINE:         rc=%ld ioerr=%ld state=%s\n",
                 (long)rc_off, (long)req->ios2_Req.io_Error, state_name(s3));

    /* Second CONFIGINTERFACE must be rejected per §3.3 — but from
     * CONFIGURED state, not OFFLINE. Since we're at CONFIGURED (came
     * back from ONLINE via OFFLINE), this exercises the guard. */
    LONG rc_cfg2 = do_cmd(req, S2_CONFIGINTERFACE);
    LONG cfg2_err  = req->ios2_Req.io_Error;
    ULONG cfg2_wire = req->ios2_WireError;
    IDOS->Printf("second CONFIGINTERFACE: rc=%ld ioerr=%ld wire=%lu (expect ioerr=%d wire=%d)\n",
                 (long)rc_cfg2, (long)cfg2_err, (unsigned long)cfg2_wire,
                 (int)S2ERR_BAD_STATE, (int)S2WERR_IS_CONFIGURED);

    LONG rc_on2 = do_cmd(req, S2_ONLINE);
    ULONG s5 = read_state(req);
    IDOS->Printf("after ONLINE #2:       rc=%ld ioerr=%ld state=%s\n",
                 (long)rc_on2, (long)req->ios2_Req.io_Error, state_name(s5));

    /* Leave idle. */
    do_cmd(req, S2_OFFLINE);

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);

    BOOL pass = (s0 == VN_STATE_OFFLINE)
             && (rc_cfg == 0 && s1 == VN_STATE_CONFIGURED)
             && (rc_on1 == 0 && s2 == VN_STATE_ONLINE)
             && (rc_off == 0 && s3 == VN_STATE_CONFIGURED)
             && (cfg2_err == S2ERR_BAD_STATE && cfg2_wire == S2WERR_IS_CONFIGURED)
             && (rc_on2 == 0 && s5 == VN_STATE_ONLINE);
    if (pass) IDOS->Printf("RESULT: PASS\n");
    else      IDOS->Printf("RESULT: FAIL states=%s->%s->%s->%s cfg2=%ld/%lu on2=%s\n",
                           state_name(s0), state_name(s1),
                           state_name(s2), state_name(s3),
                           (long)cfg2_err, (unsigned long)cfg2_wire,
                           state_name(s5));
    return pass ? 0 : 20;
}
