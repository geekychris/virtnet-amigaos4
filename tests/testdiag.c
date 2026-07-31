/*
 * testdiag - dump the extended cmdlog ring so we can see per-call field
 * details of what Roadshow (or whatever else) did to the driver.
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>
#include <devices/newstyle.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "../include/virtnet.h"

static const char *cmd_name(UWORD c)
{
    switch (c) {
    case CMD_INVALID:              return "INVALID";
    case CMD_RESET:                return "CMD_RESET";
    case CMD_READ:                 return "CMD_READ";
    case CMD_WRITE:                return "CMD_WRITE";
    case CMD_UPDATE:               return "CMD_UPDATE";
    case CMD_CLEAR:                return "CMD_CLEAR";
    case CMD_STOP:                 return "CMD_STOP";
    case CMD_START:                return "CMD_START";
    case CMD_FLUSH:                return "CMD_FLUSH";
    case NSCMD_DEVICEQUERY:        return "NSCMD_DEVICEQUERY";
    case S2_DEVICEQUERY:           return "S2_DEVICEQUERY";
    case S2_GETSTATIONADDRESS:     return "S2_GETSTATIONADDRESS";
    case S2_CONFIGINTERFACE:       return "S2_CONFIGINTERFACE";
    case S2_ADDMULTICASTADDRESS:   return "S2_ADDMULTICASTADDRESS";
    case S2_DELMULTICASTADDRESS:   return "S2_DELMULTICASTADDRESS";
    case S2_MULTICAST:             return "S2_MULTICAST";
    case S2_BROADCAST:             return "S2_BROADCAST";
    case S2_TRACKTYPE:             return "S2_TRACKTYPE";
    case S2_UNTRACKTYPE:           return "S2_UNTRACKTYPE";
    case S2_GETTYPESTATS:          return "S2_GETTYPESTATS";
    case S2_GETSPECIALSTATS:       return "S2_GETSPECIALSTATS";
    case S2_GETGLOBALSTATS:        return "S2_GETGLOBALSTATS";
    case S2_ONEVENT:               return "S2_ONEVENT";
    case S2_READORPHAN:            return "S2_READORPHAN";
    case S2_ONLINE:                return "S2_ONLINE";
    case S2_OFFLINE:               return "S2_OFFLINE";
    case S2_ADDMULTICASTADDRESSES: return "S2_ADDMULTICASTADDRESSES";
    case S2_DELMULTICASTADDRESSES: return "S2_DELMULTICASTADDRESSES";
    case S2_SANA2HOOK:             return "S2_SANA2HOOK";
    case S2_CONNECT:               return "S2_CONNECT";
    case S2_DISCONNECT:            return "S2_DISCONNECT";
    default:                       return "?";
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
    if (!req) return 20;

    LONG err = IExec->OpenDevice("virtnet.device", 0, (struct IORequest *)req, 0);
    if (err) { IDOS->Printf("RESULT: FAIL Open=%ld\n", (long)err); return 20; }

    UBYTE buf[32 * 40];
    for (int i = 0; i < 32 * 40; i++) buf[i] = 0;
    req->ios2_Req.io_Command = VN_DBG_CMDLOG;
    req->ios2_Req.io_Error   = 0;
    req->ios2_WireError      = 0;
    req->ios2_Data           = buf;
    req->ios2_DataLength     = sizeof(buf);
    IExec->DoIO((struct IORequest *)req);
    ULONG head = req->ios2_DataLength;
    IDOS->Printf("cmdlog: head=%lu (%lu entries recorded)\n",
                 (unsigned long)head, (unsigned long)(head < 32 ? head : 32));

    ULONG n = head < 32 ? head : 32;
    ULONG start = head < 32 ? 0 : (head & 31);
    for (ULONG i = 0; i < n; i++) {
        ULONG slot = (start + i) & 31;
        UBYTE *p = buf + (slot * 40);
        UWORD cmd   = ((UWORD)p[0] << 8) | (UWORD)p[1];
        UBYTE flags = p[2];
        WORD  ioerr = (WORD)(((UWORD)p[4] << 8) | (UWORD)p[5]);
        UWORD ptype = ((UWORD)p[6] << 8) | (UWORD)p[7];
        ULONG wire  = ((ULONG)p[8]  << 24) | ((ULONG)p[9]  << 16)
                    | ((ULONG)p[10] << 8)  |  (ULONG)p[11];
        ULONG dptr  = ((ULONG)p[12] << 24) | ((ULONG)p[13] << 16)
                    | ((ULONG)p[14] << 8)  |  (ULONG)p[15];
        ULONG dlen  = ((ULONG)p[16] << 24) | ((ULONG)p[17] << 16)
                    | ((ULONG)p[18] << 8)  |  (ULONG)p[19];
        ULONG bm    = ((ULONG)p[20] << 24) | ((ULONG)p[21] << 16)
                    | ((ULONG)p[22] << 8)  |  (ULONG)p[23];
        IDOS->Printf("[%3lu] %-24s flg=0x%02lx err=%ld wire=0x%lx ptype=0x%lx\n"
                     "      data=0x%08lx len=%lu bm=0x%08lx\n"
                     "      src=%02lx:%02lx:%02lx:%02lx:%02lx:%02lx  dst=%02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
                     (unsigned long)i, cmd_name(cmd),
                     (unsigned long)flags, (long)ioerr, (unsigned long)wire, (unsigned long)ptype,
                     (unsigned long)dptr, (unsigned long)dlen, (unsigned long)bm,
                     (unsigned long)p[24], (unsigned long)p[25], (unsigned long)p[26],
                     (unsigned long)p[27], (unsigned long)p[28], (unsigned long)p[29],
                     (unsigned long)p[30], (unsigned long)p[31], (unsigned long)p[32],
                     (unsigned long)p[33], (unsigned long)p[34], (unsigned long)p[35]);
    }

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    IDOS->Printf("RESULT: PASS\n");
    return 0;
}
