/*
 * testtx_cooked — send a frame using the SANA-II "cooked" path.
 *
 * Unlike testtx (which hands the driver a pre-framed L2 packet via
 * SANA2IOF_RAW-equivalent semantics), this test uses the standard
 * SANA-II flow that Roadshow / any real network stack would use:
 *
 *   1. At Open time, pass a buffer-management tag list containing
 *      S2_CopyFromBuff → our own hook.
 *   2. At CMD_WRITE time, ios2_Data is an opaque "cookie" pointer
 *      to whatever payload representation the caller wants.
 *   3. Driver builds the 14-byte Ethernet header and calls back into
 *      our CopyFromBuff hook (via IUtility->CallHookPkt) to move the
 *      payload bytes into the driver's TX buffer.
 *
 * Verifies: TPT increments (real hardware transmission through the
 * cooked path).
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>
#include <devices/sana2.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "../include/virtnet.h"

/* Our CopyFromBuff callback. Standard OS4 Hook signature — invoked via
 * IUtility->CallHookPkt(hook, object, message). object=IOSana2Req,
 * message=SANA2CopyHookMsg. We just memcpy the payload from schm_From
 * (the cookie) to schm_To (the driver's TX buffer) and return TRUE. */
static ULONG my_copy_from_buff(struct Hook *hook, APTR object, APTR message)
{
    (void)hook; (void)object;
    struct SANA2CopyHookMsg *msg = (struct SANA2CopyHookMsg *)message;
    UBYTE *dst = (UBYTE *)msg->schm_To;
    UBYTE *src = (UBYTE *)msg->schm_From;
    for (ULONG i = 0; i < msg->schm_Size; i++) dst[i] = src[i];
    return 1;   /* TRUE = success */
}

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

    /* Set up the copy-from-buff hook + a tag list that references it,
     * before OpenDevice. Driver reads ios2_BufferManagement at Open
     * time to resolve the S2_CopyFromBuff tag → h_Entry function. */
    struct Hook copyfrom_hook = { 0 };
    copyfrom_hook.h_Entry = (ULONG (*)())my_copy_from_buff;

    struct TagItem bm_tags[] = {
        { S2_CopyFromBuff, (ULONG)&copyfrom_hook },
        { TAG_END,         0 },
    };
    req->ios2_BufferManagement = (APTR)bm_tags;

    LONG err = IExec->OpenDevice("virtnet.device", 0, (struct IORequest *)req, 0);
    if (err != 0) {
        IDOS->Printf("RESULT: FAIL OpenDevice=%ld\n", (long)err);
        IExec->FreeSysObject(ASOT_IOREQUEST, req);
        IExec->FreeSysObject(ASOT_PORT, port);
        return 20;
    }

    for (int i = 0; i < 16; i++) req->ios2_SrcAddr[i] = 0;
    do_cmd(req, S2_CONFIGINTERFACE);
    LONG rc_on = do_cmd(req, S2_ONLINE);
    if (rc_on != 0) { IDOS->Printf("RESULT: FAIL ONLINE=%ld\n", (long)rc_on); goto out; }

    /* e1000 statistics registers self-clear on read — see testtx.c. */
    (void)read_tpt(req);
    ULONG tpt_before = read_tpt(req);
    IDOS->Printf("TPT before cooked send: %lu\n", (unsigned long)tpt_before);

    /* Payload = the "cookie" — arbitrary bytes we want the driver to send
     * after building an Ethernet header on our behalf. Use a small ARP-
     * shape body so the hook copy is verifiable. */
    UBYTE payload[28] = {
        0x00, 0x01,             /* HW type: Ethernet */
        0x08, 0x00,             /* Proto type: IPv4 */
        6, 4,                   /* addr lens */
        0x00, 0x01,             /* op: request */
        0,0,0,0,0,0,            /* sender HW (filled after we get our MAC) */
        192, 168, 100, 10,      /* sender IP */
        0,0,0,0,0,0,            /* target HW (zero — unknown) */
        192, 168, 100, 2,       /* target IP: QEMU gateway */
    };
    do_cmd(req, S2_GETSTATIONADDRESS);
    for (int i = 0; i < 6; i++) payload[6 + i] = req->ios2_SrcAddr[i];

    /* CMD_WRITE, cooked. Set BufferManagement AGAIN (do_cmd calls above
     * may have overwritten it via reply message) — driver matches on
     * this to find the opener. */
    req->ios2_BufferManagement = (APTR)bm_tags;
    req->ios2_Req.io_Command = CMD_WRITE;
    req->ios2_Req.io_Error   = 0;
    req->ios2_WireError      = 0;
    req->ios2_PacketType     = 0x0806;   /* ARP */
    req->ios2_Data           = payload;  /* cookie — passed to hook as schm_From */
    req->ios2_DataLength     = sizeof(payload);
    for (int i = 0; i < 6; i++) req->ios2_DstAddr[i] = 0xff;   /* broadcast */

    LONG rc = IExec->DoIO((struct IORequest *)req);
    IDOS->Printf("cooked CMD_WRITE  DoIO=%ld  io_Error=%ld  wire=0x%lx\n",
                 (long)rc, (long)req->ios2_Req.io_Error,
                 (unsigned long)req->ios2_WireError);

    ULONG tpt_after = read_tpt(req);
    IDOS->Printf("TPT after cooked send:  %lu (delta=%ld)\n",
                 (unsigned long)tpt_after,
                 (long)((long)tpt_after - (long)tpt_before));

    do_cmd(req, S2_OFFLINE);

out:
    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);

    BOOL sent = (rc == 0) && (req->ios2_Req.io_Error == 0)
             && (tpt_after == tpt_before + 1);
    if (sent) {
        IDOS->Printf("RESULT: PASS (cooked frame egressed via CopyFromBuff hook)\n");
    } else {
        IDOS->Printf("RESULT: FAIL rc=%ld ioerr=%ld tpt=%lu->%lu\n",
                     (long)rc, (long)req->ios2_Req.io_Error,
                     (unsigned long)tpt_before, (unsigned long)tpt_after);
    }
    return sent ? 0 : 20;
}
