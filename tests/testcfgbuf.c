/* testcfgbuf - dump the last S2_CONFIGINTERFACE ioreq snapshot. */
#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "../include/virtnet.h"

static void hexdump(const char *label, const UBYTE *b, ULONG n)
{
    IDOS->Printf("%s:\n  ", (const char *)label);
    for (ULONG i = 0; i < n; i++) {
        IDOS->Printf("%02lx ", (unsigned long)b[i]);
        if ((i & 15) == 15) IDOS->Printf("\n  ");
    }
    if (n & 15) IDOS->Printf("\n");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!port) return 20;
    struct IOSana2Req *req = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, port, ASOIOR_Size, sizeof(struct IOSana2Req), TAG_END);
    if (!req) return 20;
    LONG err = IExec->OpenDevice("virtnet.device", 0, (struct IORequest *)req, 0);
    if (err) { IDOS->Printf("RESULT: FAIL Open=%ld\n", (long)err); return 20; }
    UBYTE buf[160];
    for (int i = 0; i < 160; i++) buf[i] = 0xEE;
    req->ios2_Req.io_Command = VN_DBG_CFGBUF;
    req->ios2_Req.io_Error   = 0;
    req->ios2_Data           = buf;
    req->ios2_DataLength     = sizeof(buf);
    for (int i = 0; i < 16; i++) req->ios2_SrcAddr[i] = 0;
    IExec->DoIO((struct IORequest *)req);
    ULONG cnt = ((ULONG)req->ios2_SrcAddr[0] << 24) | ((ULONG)req->ios2_SrcAddr[1] << 16)
              | ((ULONG)req->ios2_SrcAddr[2] << 8)  |  (ULONG)req->ios2_SrcAddr[3];
    IDOS->Printf("S2_CONFIGINTERFACE calls seen: %lu\n", (unsigned long)cnt);
    if (cnt > 0) {
        hexdump("ioreq BEFORE (80 bytes)", buf,      80);
        hexdump("ioreq AFTER  (80 bytes)", buf + 80, 80);
    }
    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    IDOS->Printf("RESULT: PASS\n");
    return 0;
}
