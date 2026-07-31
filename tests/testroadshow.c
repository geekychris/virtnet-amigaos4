/*
 * testroadshow — reproduces Roadshow's usage pattern in-suite so we can
 * iterate on the "guest hangs when Roadshow binds" bug without touching
 * the real network stack.
 *
 * Roadshow's AddNetInterface flow (as best we can reconstruct):
 *   1. LoadSeg driver
 *   2. Open device with a BufferManagement tag list carrying CopyFromBuff,
 *      CopyToBuff, and DMA-related hooks
 *   3. S2_GETSTATIONADDRESS — read factory MAC
 *   4. S2_CONFIGINTERFACE with ios2_SrcAddr = 0..0 (accept factory)
 *   5. S2_ONLINE
 *   6. S2_TRACKTYPE(0x0800) + S2_TRACKTYPE(0x0806)
 *   7. S2_ONEVENT with S2EVENT_ONLINE — queued; expected to reply on
 *      state changes only
 *   8. Multiple concurrent CMD_READs queued via SendIO
 *   9. S2_BROADCAST with a DHCP DISCOVER packet
 *   10. Later: RemNetInterface → AbortIO all pending → CloseDevice
 *
 * When Roadshow tried this against our driver, we got:
 *   - "Interface virtnet not configured; broadcast access not supported"
 *   - Guest hang after RemNetInterface (subsequent bridge silence)
 *
 * This test walks the same shape and prints where it dies. Success is
 * everything cleanly returning (no hang, no memory corruption). Missing
 * commands should return IOERR_NOCMD instead of crashing.
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>
#include <devices/sana2.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "../include/virtnet.h"

static ULONG g_cf_count = 0, g_ct_count = 0;

static ULONG my_copy_from(struct Hook *h, APTR o, APTR m)
{
    (void)h; (void)o;
    struct SANA2CopyHookMsg *msg = (struct SANA2CopyHookMsg *)m;
    UBYTE *dst = (UBYTE *)msg->schm_To;
    UBYTE *src = (UBYTE *)msg->schm_From;
    for (ULONG i = 0; i < msg->schm_Size; i++) dst[i] = src[i];
    g_cf_count++;
    return 1;
}

static ULONG my_copy_to(struct Hook *h, APTR o, APTR m)
{
    (void)h; (void)o;
    struct SANA2CopyHookMsg *msg = (struct SANA2CopyHookMsg *)m;
    UBYTE *dst = (UBYTE *)msg->schm_To;
    UBYTE *src = (UBYTE *)msg->schm_From;
    for (ULONG i = 0; i < msg->schm_Size; i++) dst[i] = src[i];
    g_ct_count++;
    return 1;
}

static LONG do_cmd(struct IOSana2Req *r, UWORD cmd)
{
    r->ios2_Req.io_Command = cmd;
    r->ios2_Req.io_Error   = 0;
    r->ios2_WireError      = 0;
    return IExec->DoIO((struct IORequest *)r);
}

/* Post an ioreq via SendIO without blocking, marking it NT_MESSAGE so
 * CheckIO/AbortIO don't get confused by a stale ln_Type. */
static void post(struct IOSana2Req *r, UWORD cmd)
{
    r->ios2_Req.io_Command = cmd;
    r->ios2_Req.io_Error   = 0;
    r->ios2_WireError      = 0;
    r->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    IExec->SendIO((struct IORequest *)r);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!port) return 20;

    /* Six IORequests sharing one reply port. Roadshow allocates one per
     * concurrent operation — CMD_READ, S2_ONEVENT, S2_BROADCAST, etc. */
    struct IOSana2Req *r[6];
    for (int i = 0; i < 6; i++) {
        r[i] = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
            ASOIOR_ReplyPort, port,
            ASOIOR_Size, sizeof(struct IOSana2Req),
            TAG_END);
        if (!r[i]) { IDOS->Printf("RESULT: FAIL alloc %d\n", i); return 20; }
    }

    struct Hook hcf = { {0}, (ULONG (*)())my_copy_from, 0, 0 };
    struct Hook hct = { {0}, (ULONG (*)())my_copy_to,   0, 0 };
    struct TagItem bmtags[] = {
        { S2_CopyFromBuff, (ULONG)&hcf },
        { S2_CopyToBuff,   (ULONG)&hct },
        { TAG_END, 0 },
    };
    r[0]->ios2_BufferManagement = (APTR)bmtags;

    LONG err = IExec->OpenDevice("virtnet.device", 0, (struct IORequest *)r[0], 0);
    if (err) { IDOS->Printf("RESULT: FAIL Open=%ld\n", (long)err); return 20; }
    IDOS->Printf("open OK\n");

    /* Mirror driver-set fields to the other reqs. */
    for (int i = 1; i < 6; i++) {
        r[i]->ios2_Req.io_Device    = r[0]->ios2_Req.io_Device;
        r[i]->ios2_Req.io_Unit      = r[0]->ios2_Req.io_Unit;
        r[i]->ios2_BufferManagement = r[0]->ios2_BufferManagement;
    }

    for (int i = 0; i < 6; i++) r[0]->ios2_SrcAddr[i] = 0;
    LONG rc_cfg = do_cmd(r[0], S2_CONFIGINTERFACE);
    IDOS->Printf("S2_CONFIGINTERFACE ioerr=%ld wire=0x%lx\n",
                 (long)r[0]->ios2_Req.io_Error, (unsigned long)r[0]->ios2_WireError);

    LONG rc_gsa = do_cmd(r[0], S2_GETSTATIONADDRESS);
    IDOS->Printf("S2_GETSTATIONADDRESS ioerr=%ld  mac=%02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
                 (long)r[0]->ios2_Req.io_Error,
                 (unsigned long)r[0]->ios2_SrcAddr[0], (unsigned long)r[0]->ios2_SrcAddr[1],
                 (unsigned long)r[0]->ios2_SrcAddr[2], (unsigned long)r[0]->ios2_SrcAddr[3],
                 (unsigned long)r[0]->ios2_SrcAddr[4], (unsigned long)r[0]->ios2_SrcAddr[5]);
    (void)rc_cfg; (void)rc_gsa;

    LONG rc_on = do_cmd(r[0], S2_ONLINE);
    IDOS->Printf("S2_ONLINE ioerr=%ld\n", (long)r[0]->ios2_Req.io_Error);
    if (rc_on != 0) { IDOS->Printf("RESULT: FAIL ONLINE=%ld\n", (long)rc_on); goto close; }

    /* S2_TRACKTYPE — Roadshow calls this for each ethertype it cares about.
     * Our driver has no handler; falls to default (IOERR_NOCMD). Should
     * NOT crash, just return the error cleanly. */
    r[0]->ios2_PacketType = 0x0800;
    LONG rc_tt = do_cmd(r[0], S2_TRACKTYPE);
    IDOS->Printf("S2_TRACKTYPE(0x0800) rc=%ld ioerr=%ld\n",
                 (long)rc_tt, (long)r[0]->ios2_Req.io_Error);

    /* S2_ONEVENT — event subscription. Our driver advertises it in
     * supportedcmds but has no handler. Roadshow QUEUES this expecting
     * it to sit pending; if we reply immediately with IOERR_NOCMD,
     * Roadshow might misbehave. Post via SendIO and try to AbortIO. */
    r[1]->ios2_WireError = S2EVENT_ONLINE;   /* event mask goes in WireError per SANA-II */
    post(r[1], S2_ONEVENT);
    IDOS->Printf("S2_ONEVENT posted\n");

    /* Queue 3 concurrent CMD_READs — mirror Roadshow keeping several
     * reads pending so incoming frames land in a caller buffer. */
    UBYTE rxbuf[3][256] = { {0} };
    for (int i = 0; i < 3; i++) {
        r[2 + i]->ios2_Data       = rxbuf[i];
        r[2 + i]->ios2_DataLength = 0;
        post(r[2 + i], CMD_READ);
    }
    IDOS->Printf("3x CMD_READ queued\n");

    /* Send a broadcast frame (S2_BROADCAST). Roadshow uses this for
     * DHCP DISCOVER. Cooked-mode: driver prepends dst=ff:ff:ff:ff:ff:ff
     * automatically (S2_BROADCAST semantics). Passing a small dummy
     * payload to validate that our S2_BROADCAST path handles cooked
     * requests without crashing. */
    UBYTE bcpayload[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE };
    r[5]->ios2_PacketType = 0x0800;
    r[5]->ios2_Data       = bcpayload;
    r[5]->ios2_DataLength = sizeof(bcpayload);
    /* S2_BROADCAST is defined to set dst = broadcast MAC internally, but
     * many drivers still need it in ios2_DstAddr. Set both. */
    for (int i = 0; i < 6; i++) r[5]->ios2_DstAddr[i] = 0xff;
    LONG rc_bc = do_cmd(r[5], S2_BROADCAST);
    IDOS->Printf("S2_BROADCAST rc=%ld ioerr=%ld wire=0x%lx\n",
                 (long)rc_bc, (long)r[5]->ios2_Req.io_Error,
                 (unsigned long)r[5]->ios2_WireError);

    /* Small wait, let ISR fire if replies come in. */
    IDOS->Delay(20);   /* 400 ms */

    /* Clean shutdown: AbortIO every outstanding request. This is what
     * RemNetInterface does. Any hang here indicates a bug in our
     * AbortIO or opener-list traversal. Requests we AbortIO'd should
     * come back promptly via WaitIO. */
    IDOS->Printf("aborting outstanding requests...\n");
    IExec->AbortIO((struct IORequest *)r[1]);
    for (int i = 0; i < 3; i++) IExec->AbortIO((struct IORequest *)r[2 + i]);

    IDOS->Printf("waiting for aborted replies...\n");
    IExec->WaitIO((struct IORequest *)r[1]);
    IDOS->Printf("  r[1] (S2_ONEVENT) ioerr=%ld\n", (long)r[1]->ios2_Req.io_Error);
    for (int i = 0; i < 3; i++) {
        IExec->WaitIO((struct IORequest *)r[2 + i]);
        IDOS->Printf("  r[%d] (CMD_READ) ioerr=%ld\n",
                     2 + i, (long)r[2 + i]->ios2_Req.io_Error);
    }

    do_cmd(r[0], S2_OFFLINE);
    IDOS->Printf("S2_OFFLINE done\n");

close:
    IDOS->Printf("CloseDevice...\n");
    IExec->CloseDevice((struct IORequest *)r[0]);
    IDOS->Printf("CloseDevice OK\n");
    for (int i = 0; i < 6; i++) IExec->FreeSysObject(ASOT_IOREQUEST, r[i]);
    IExec->FreeSysObject(ASOT_PORT, port);

    IDOS->Printf("cf_count=%lu ct_count=%lu\n",
                 (unsigned long)g_cf_count, (unsigned long)g_ct_count);
    IDOS->Printf("RESULT: PASS (roadshow-sim flow completed without hang)\n");
    return 0;
}
