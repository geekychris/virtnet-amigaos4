/* teststat — minimal DBG_STATUS call. Prints state + last_dispatched_cmd
 * so we can see what the driver was last handling before crashing. */

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

    req->ios2_Req.io_Command = VN_DBG_STATUS;
    req->ios2_Req.io_Error   = 0;
    IExec->DoIO((struct IORequest *)req);

    UWORD last_cmd = ((UWORD)req->ios2_DstAddr[12] << 8) | req->ios2_DstAddr[13];
    IDOS->Printf("state=%lu irq=%lu icr=0x%08lx last_dispatched_cmd=0x%04x\n",
        (unsigned long)req->ios2_PacketType,
        (unsigned long)req->ios2_DataLength,
        (unsigned long)req->ios2_WireError,
        (unsigned)last_cmd);

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    IDOS->Printf("OK\n");
    return 0;
}
