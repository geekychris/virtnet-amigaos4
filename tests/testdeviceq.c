/*
 * testdeviceq — verify S2_DEVICEQUERY.
 *
 * Opens virtnet.device unit 0, calls S2_DEVICEQUERY, prints the
 * returned Sana2DeviceQuery struct. On a working driver + present
 * e1000 we expect: HardwareType=1 (Ethernet), MTU=1500, RawMTU=1514,
 * BPS=1_000_000_000, AddrFieldSize=48.
 *
 * Runs as a normal AmigaDOS executable; run via
 * `./scripts/iterate.sh testdeviceq` on the host.
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
    if (!port) { IDOS->Printf("testdeviceq: AllocSysObject(PORT) failed\n"); return 20; }

    struct IOSana2Req *req = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
        ASOIOR_ReplyPort, port,
        ASOIOR_Size, sizeof(struct IOSana2Req),
        TAG_END);
    if (!req) { IExec->FreeSysObject(ASOT_PORT, port); return 20; }

    LONG err = IExec->OpenDevice("virtnet.device", 0, (struct IORequest *)req, 0);
    if (err != 0) {
        IDOS->Printf("testdeviceq: OpenDevice = %ld (FAIL)\n", (long)err);
        IExec->FreeSysObject(ASOT_IOREQUEST, req);
        IExec->FreeSysObject(ASOT_PORT, port);
        return 20;
    }

    struct Sana2DeviceQuery q = { 0 };
    q.SizeAvailable = sizeof(q);
    req->ios2_Req.io_Command = S2_DEVICEQUERY;
    req->ios2_Data = &q;             /* driver checks ios2_Data first */
    req->ios2_StatData = &q;         /* and ios2_StatData as spec-compliant fallback */

    LONG rc = IExec->DoIO((struct IORequest *)req);
    IDOS->Printf("S2_DEVICEQUERY  DoIO=%ld  io_Error=%ld  wireError=%lu\n",
                 (long)rc, (long)req->ios2_Req.io_Error,
                 (unsigned long)req->ios2_WireError);
    if (rc == 0 && req->ios2_Req.io_Error == 0) {
        IDOS->Printf("  SizeSupplied  = %lu\n",  (unsigned long)q.SizeSupplied);
        IDOS->Printf("  DevQueryFormat= %lu\n",  (unsigned long)q.DevQueryFormat);
        IDOS->Printf("  DeviceLevel   = %lu\n",  (unsigned long)q.DeviceLevel);
        IDOS->Printf("  AddrFieldSize = %lu bits\n", (unsigned long)q.AddrFieldSize);
        IDOS->Printf("  MTU           = %lu\n",  (unsigned long)q.MTU);
        IDOS->Printf("  BPS           = %lu\n",  (unsigned long)q.BPS);
        IDOS->Printf("  HardwareType  = %lu (%s)\n", (unsigned long)q.HardwareType,
                     q.HardwareType == S2WireType_Ethernet ? "Ethernet" : "other");
        IDOS->Printf("  RawMTU        = %lu\n",  (unsigned long)q.RawMTU);
    }

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);

    /* Suite pass criteria: DoIO succeeded, io_Error clear, and the returned
     * struct matches expected Ethernet values (defense against silently
     * accepting a device that reports as e.g. IEEE802 or wrong MTU). */
    BOOL pass = (rc == 0)
             && (req->ios2_Req.io_Error == 0)
             && (q.HardwareType == S2WireType_Ethernet)
             && (q.MTU == 1500)
             && (q.RawMTU == 1514)
             && (q.AddrFieldSize == 48);
    if (pass) IDOS->Printf("RESULT: PASS\n");
    else      IDOS->Printf("RESULT: FAIL rc=%ld ioerr=%ld hw=%lu MTU=%lu\n",
                           (long)rc, (long)req->ios2_Req.io_Error,
                           (unsigned long)q.HardwareType, (unsigned long)q.MTU);
    return pass ? 0 : 20;
}
