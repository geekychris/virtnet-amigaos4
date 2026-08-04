/* testrawstat — dump ALL 16 bytes of SrcAddr and DstAddr from
 * VN_DBG_STATUS, so we can see the extra TX-diagnostic fields
 * added to device.c without another rebuild+deploy cycle for a
 * teststat rewrite. */

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "../include/virtnet.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!port) return 20;
    struct IOSana2Req *req = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, port,
        ASOIOR_Size, sizeof(struct IOSana2Req),
        TAG_END);
    if (!req) return 20;

    LONG err = IExec->OpenDevice("virtnet.device", 0, (struct IORequest *)req, 0);
    if (err) { IDOS->Printf("FAIL Open=%ld\n", (long)err); return 20; }

    /* First send a CMD_WRITE so the TX diagnostic fields get updated. */
    UBYTE frame[64];
    for (int i = 0; i < 64; i++) frame[i] = (UBYTE)i;
    for (int i = 0; i < 6; i++) req->ios2_DstAddr[i] = 0xFF;
    req->ios2_PacketType     = 0x0800;
    req->ios2_Req.io_Command = CMD_WRITE;
    req->ios2_Req.io_Error   = 0;
    req->ios2_DataLength     = 64;
    req->ios2_Data           = frame;
    LONG wrc = IExec->DoIO((struct IORequest *)req);
    IDOS->Printf("CMD_WRITE: DoIO=%ld ioerr=%ld wire=0x%lx\n",
                 (long)wrc, (long)req->ios2_Req.io_Error,
                 (unsigned long)req->ios2_WireError);

    /* Now DBG_STATUS. */
    req->ios2_Req.io_Command = VN_DBG_STATUS;
    req->ios2_Req.io_Error   = 0;
    IExec->DoIO((struct IORequest *)req);

    IDOS->Printf("state=%lu irq=%lu icr=0x%08lx\n",
        (unsigned long)req->ios2_PacketType,
        (unsigned long)req->ios2_DataLength,
        (unsigned long)req->ios2_WireError);

    IDOS->Printf("SrcAddr[0..15]:");
    for (int i = 0; i < 16; i++) IDOS->Printf(" %02lx", (unsigned long)req->ios2_SrcAddr[i]);
    IDOS->Printf("\n");
    IDOS->Printf("DstAddr[0..15]:");
    for (int i = 0; i < 16; i++) IDOS->Printf(" %02lx", (unsigned long)req->ios2_DstAddr[i]);
    IDOS->Printf("\n");

    /* Decode the TX diagnostic fields per the overload documented
     * in device.c's VN_DBG_STATUS block. */
    ULONG tap  = ((ULONG)req->ios2_SrcAddr[6] << 24)
               | ((ULONG)req->ios2_SrcAddr[7] << 16)
               | ((ULONG)req->ios2_SrcAddr[8] << 8)
               |  (ULONG)req->ios2_SrcAddr[9];
    UWORD w    = ((UWORD)req->ios2_SrcAddr[10] << 8) | req->ios2_SrcAddr[11];
    UWORD r    = ((UWORD)req->ios2_SrcAddr[12] << 8) | req->ios2_SrcAddr[13];
    UWORD tu   = ((UWORD)req->ios2_SrcAddr[14] << 8) | req->ios2_SrcAddr[15];
    UWORD di   = ((UWORD)req->ios2_DstAddr[14] << 8) | req->ios2_DstAddr[15];
    IDOS->Printf("TX diag: tavail_addr=0x%08lx written=%u readback=%u used_idx=%u avail_ring[0]=%u\n",
                 (unsigned long)tap, (unsigned)w, (unsigned)r, (unsigned)tu, (unsigned)di);

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    return 0;
}
