/*
 * testshuntdiag - dump sanashunt's log ring so we can see the full
 * BEFORE + AFTER field snapshot of every command Roadshow sent
 * through the shunt to rtl8139.
 *
 * Deploy sanashunt.device to DEVS:Networks/, push a Roadshow config
 * pointing at it (see ../roadshow/shunt), let Roadshow bind, then
 * run this to dump the trace.
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>
#include <devices/newstyle.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "../include/shunt.h"

static const char *cmd_name(UWORD c)
{
    switch (c) {
    case CMD_INVALID:              return "INVALID";
    case CMD_READ:                 return "CMD_READ";
    case CMD_WRITE:                return "CMD_WRITE";
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

    LONG err = IExec->OpenDevice("sanashunt2.device", 0, (struct IORequest *)req, 0);
    if (err) {
        IDOS->Printf("RESULT: FAIL sanashunt Open=%ld\n", (long)err);
        return 20;
    }

    /* Quick status: backend_open? */
    req->ios2_Req.io_Command = SHUNT_DBG_STATUS;
    req->ios2_Req.io_Error   = 0;
    IExec->DoIO((struct IORequest *)req);
    ULONG head_status = req->ios2_DataLength;
    ULONG backend_ok  = req->ios2_PacketType;
    IDOS->Printf("shunt status: backend_open=%lu log_head=%lu\n",
                 (unsigned long)backend_ok, (unsigned long)head_status);

    /* Now dump the log. */
    ULONG entry_size = 44;   /* keep in sync with SanaShuntBase.log[] entry */
    UBYTE buf[64 * 48];       /* extra slack in case struct pads */
    for (ULONG i = 0; i < sizeof(buf); i++) buf[i] = 0;
    req->ios2_Req.io_Command = SHUNT_DBG_CMDLOG;
    req->ios2_Req.io_Error   = 0;
    req->ios2_Data           = buf;
    req->ios2_DataLength     = sizeof(buf);
    IExec->DoIO((struct IORequest *)req);
    ULONG head = req->ios2_DataLength;
    IDOS->Printf("cmdlog: head=%lu (%lu entries recorded)\n",
                 (unsigned long)head, (unsigned long)(head < 64 ? head : 64));

    /* The exact struct layout is defined in shunt.h. We rely on the
     * compiled sizeof matching between shunt.c and this test. */
    struct LogEntry {
        UWORD  cmd;
        UBYTE  flags_in, _pad0;
        ULONG  data_in;
        ULONG  datalen_in;
        ULONG  bm_in;
        UWORD  ptype_in;
        UBYTE  src_in[6];
        UBYTE  dst_in[6];
        WORD   ioerr_out;
        ULONG  wire_out;
        ULONG  datalen_out;
        UWORD  ptype_out;
        UBYTE  src_out[6];
        UBYTE  dst_out[6];
    };
    ULONG our_size = sizeof(struct LogEntry);
    IDOS->Printf("(local entry sizeof = %lu; using this to walk buffer)\n",
                 (unsigned long)our_size);
    (void)entry_size;

    ULONG n = head < 64 ? head : 64;
    ULONG start = head < 64 ? 0 : (head & 63);
    for (ULONG i = 0; i < n; i++) {
        ULONG slot = (start + i) & 63;
        struct LogEntry *e = (struct LogEntry *)(buf + slot * our_size);
        IDOS->Printf("[%2lu] %-24s flg=0x%02lx err=%ld wire=0x%lx\n"
                     "     IN : data=0x%08lx len=%lu bm=0x%08lx ptype=0x%lx\n"
                     "          src=%02x:%02x:%02x:%02x:%02x:%02x dst=%02x:%02x:%02x:%02x:%02x:%02x\n"
                     "     OUT: len=%lu ptype=0x%lx\n"
                     "          src=%02x:%02x:%02x:%02x:%02x:%02x dst=%02x:%02x:%02x:%02x:%02x:%02x\n",
                     (unsigned long)i, cmd_name(e->cmd),
                     (unsigned long)e->flags_in, (long)e->ioerr_out, (unsigned long)e->wire_out,
                     (unsigned long)e->data_in, (unsigned long)e->datalen_in,
                     (unsigned long)e->bm_in, (unsigned long)e->ptype_in,
                     e->src_in[0],e->src_in[1],e->src_in[2],e->src_in[3],e->src_in[4],e->src_in[5],
                     e->dst_in[0],e->dst_in[1],e->dst_in[2],e->dst_in[3],e->dst_in[4],e->dst_in[5],
                     (unsigned long)e->datalen_out, (unsigned long)e->ptype_out,
                     e->src_out[0],e->src_out[1],e->src_out[2],e->src_out[3],e->src_out[4],e->src_out[5],
                     e->dst_out[0],e->dst_out[1],e->dst_out[2],e->dst_out[3],e->dst_out[4],e->dst_out[5]);
    }

    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    IDOS->Printf("RESULT: PASS\n");
    return 0;
}
