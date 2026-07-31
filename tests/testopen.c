/*
 * testopen — smoke test for virtnet.device.
 *
 * OpenDevice("virtnet.device", 0, ...), print the result, CloseDevice.
 * Nothing more. If this runs cleanly from an AmigaDOS shell and exits
 * with RETURN_OK, the resident tag + Init/Open/Close path works.
 *
 * Copy the built binary + virtnet.device to DH1: on the OS4 guest,
 * `avail flush` to purge cached copies, then:
 *
 *     DH1:> testopen
 *
 * Expected: "OpenDevice(virtnet.device, 0) = 0 (OK)" on stdout and
 * a DebugPrintF trace visible in the emulator's debug log.
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <devices/sana2.h>

#include <proto/exec.h>
#include <proto/dos.h>

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    struct MsgPort   *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    struct IOSana2Req *req = NULL;

    if (!port) {
        IDOS->Printf("testopen: AllocSysObjectTags(ASOT_PORT) failed\n");
        return 20;
    }

    req = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
                                    ASOIOR_ReplyPort, port,
                                    ASOIOR_Size, sizeof(struct IOSana2Req),
                                    TAG_END);
    if (!req) {
        IDOS->Printf("testopen: AllocSysObjectTags(ASOT_IOREQUEST) failed\n");
        IExec->FreeSysObject(ASOT_PORT, port);
        return 20;
    }

    LONG err = IExec->OpenDevice("virtnet.device", 0,
                                 (struct IORequest *)req, 0);
    IDOS->Printf("OpenDevice(virtnet.device, 0) = %ld (%s)\n",
                 (long)err, err == 0 ? "OK" : "FAIL");

    if (err == 0) {
        IDOS->Printf("  io_Device = %p  io_Unit = %p\n",
                     req->ios2_Req.io_Device, req->ios2_Req.io_Unit);
        IExec->CloseDevice((struct IORequest *)req);
        IDOS->Printf("CloseDevice returned.\n");
    }

    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    /* Suite convention: last line is "RESULT: PASS" or "RESULT: FAIL ..."
     * run-test.sh greps for it to compute a suite pass/fail count. */
    if (err == 0) IDOS->Printf("RESULT: PASS\n");
    else          IDOS->Printf("RESULT: FAIL OpenDevice=%ld\n", (long)err);
    return err == 0 ? 0 : 20;
}
