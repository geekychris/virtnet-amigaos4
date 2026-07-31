/*
 * testmac — verify S2_GETSTATIONADDRESS.
 *
 * Opens virtnet.device unit 0, calls S2_GETSTATIONADDRESS, prints
 * both the factory MAC (ios2_SrcAddr) and current MAC (ios2_DstAddr).
 * For an unconfigured driver these are identical (the factory MAC).
 * QEMU's default second-NIC MAC is 52:54:00:12:34:57.
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>

#include <proto/exec.h>
#include <proto/dos.h>

static void print_mac(const char *label, const UBYTE *m)
{
    IDOS->Printf("  %s = %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
                 (STRPTR)label,
                 (unsigned long)m[0], (unsigned long)m[1], (unsigned long)m[2],
                 (unsigned long)m[3], (unsigned long)m[4], (unsigned long)m[5]);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    struct MsgPort   *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!port) return 20;

    struct IOSana2Req *req = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, port,
        ASOIOR_Size, sizeof(struct IOSana2Req),
        TAG_END);
    if (!req) { IExec->FreeSysObject(ASOT_PORT, port); return 20; }

    LONG err = IExec->OpenDevice("virtnet.device", 0, (struct IORequest *)req, 0);
    if (err != 0) {
        IDOS->Printf("testmac: OpenDevice = %ld (FAIL)\n", (long)err);
        IExec->FreeSysObject(ASOT_IOREQUEST, req);
        IExec->FreeSysObject(ASOT_PORT, port);
        return 20;
    }

    req->ios2_Req.io_Command = S2_GETSTATIONADDRESS;
    LONG rc = IExec->DoIO((struct IORequest *)req);
    IDOS->Printf("S2_GETSTATIONADDRESS  DoIO=%ld  io_Error=%ld  wireError=%lu\n",
                 (long)rc, (long)req->ios2_Req.io_Error,
                 (unsigned long)req->ios2_WireError);
    if (rc == 0 && req->ios2_Req.io_Error == 0) {
        print_mac("factory (SrcAddr)", req->ios2_SrcAddr);
        print_mac("current (DstAddr)", req->ios2_DstAddr);
    }

    /* Sanity: MAC must be non-zero, non-broadcast, and QEMU-locally-admin
     * (first octet 0x52). All-zero would mean RAL/RAH read returned nothing. */
    BOOL nonzero = FALSE;
    for (int i = 0; i < 6; i++)
        if (req->ios2_SrcAddr[i] != 0) { nonzero = TRUE; break; }
    BOOL not_bcast = !(req->ios2_SrcAddr[0] == 0xff && req->ios2_SrcAddr[1] == 0xff);

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);

    BOOL pass = (rc == 0) && (req->ios2_Req.io_Error == 0)
             && nonzero && not_bcast;
    if (pass) IDOS->Printf("RESULT: PASS\n");
    else      IDOS->Printf("RESULT: FAIL rc=%ld ioerr=%ld nonzero=%d\n",
                           (long)rc, (long)req->ios2_Req.io_Error, (int)nonzero);
    return pass ? 0 : 20;
}
