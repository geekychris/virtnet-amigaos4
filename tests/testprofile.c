/* testprofile — read the per-stage TB counters from virtnet.device.
 *
 * Usage:
 *   DH1:testprofile
 *
 * Output: one line per counter, plus derived averages (cycles per
 * TX packet, per RX call, cycles-per-µs given TB=100MHz).
 *
 * Run AFTER a perf test (e.g. iperf3 -t 10) so the counters have
 * meaningful sample counts. Counters accumulate driver-lifetime, so
 * to isolate a specific test's contribution, run testprofile once
 * before, once after, and subtract. */

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "../include/virtnet.h"

/* TB frequency on sam460ex: nominal 100 MHz, so 100 cycles = 1 µs.
 * If your target differs, adjust — worst case the "cycles per µs"
 * column is off but the RELATIVE stage sizes are still correct. */
#define TB_HZ 100000000UL

static ULONG read_be32(UBYTE *p) {
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) |
           ((ULONG)p[2] << 8)  |  (ULONG)p[3];
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

    LONG err = IExec->OpenDevice("virtnet.device", 0,
                                 (struct IORequest *)req, 0);
    if (err) {
        IDOS->Printf("OpenDevice virtnet failed: %ld\n", (long)err);
        IExec->FreeSysObject(ASOT_IOREQUEST, req);
        IExec->FreeSysObject(ASOT_PORT, port);
        return 20;
    }

    UBYTE buf[44];
    for (int i = 0; i < 44; i++) buf[i] = 0;

    req->ios2_Req.io_Command = VN_DBG_PROFILE;
    req->ios2_Req.io_Error   = 0;
    req->ios2_Data           = buf;
    req->ios2_DataLength     = 44;
    IExec->DoIO((struct IORequest *)req);
    if (req->ios2_Req.io_Error) {
        IDOS->Printf("DBG_PROFILE failed: ioerr=%ld\n",
                     (long)req->ios2_Req.io_Error);
        goto out;
    }

    ULONG tx_calls  = read_be32(buf + 0);
    ULONG tx_total  = read_be32(buf + 4);
    ULONG tx_cook   = read_be32(buf + 8);
    ULONG tx_flush  = read_be32(buf + 12);
    ULONG tx_ring   = read_be32(buf + 16);
    ULONG tx_notify = read_be32(buf + 20);
    ULONG rx_calls  = read_be32(buf + 24);
    ULONG rx_pkts   = read_be32(buf + 28);
    ULONG rx_total  = read_be32(buf + 32);
    ULONG rx_hook   = read_be32(buf + 36);
    ULONG tx_hook   = read_be32(buf + 40);

    IDOS->Printf("--- virtnet TX profile ---\n");
    IDOS->Printf("  tx_calls              = %10lu\n", tx_calls);
    if (tx_calls) {
        IDOS->Printf("  avg cycles / TX pkt   = %10lu\n", tx_total  / tx_calls);
        IDOS->Printf("    cook               = %10lu (%2lu%%)\n",
                     tx_cook / tx_calls,
                     tx_total ? (100UL * tx_cook / tx_total) : 0);
        IDOS->Printf("      of which hook    = %10lu (%2lu%% of cook)\n",
                     tx_hook / tx_calls,
                     tx_cook ? (100UL * tx_hook / tx_cook) : 0);
        IDOS->Printf("    flush              = %10lu (%2lu%%)\n",
                     tx_flush / tx_calls,
                     tx_total ? (100UL * tx_flush / tx_total) : 0);
        IDOS->Printf("    ring               = %10lu (%2lu%%)\n",
                     tx_ring / tx_calls,
                     tx_total ? (100UL * tx_ring / tx_total) : 0);
        IDOS->Printf("    notify             = %10lu (%2lu%%)\n",
                     tx_notify / tx_calls,
                     tx_total ? (100UL * tx_notify / tx_total) : 0);
        IDOS->Printf("  avg time / TX pkt     = %10lu us (assuming TB=100MHz)\n",
                     (tx_total / tx_calls) / 100UL);
    }

    IDOS->Printf("--- virtnet RX profile ---\n");
    IDOS->Printf("  rx_calls              = %10lu (batches)\n", rx_calls);
    IDOS->Printf("  rx_pkts               = %10lu (frames delivered)\n", rx_pkts);
    if (rx_calls) {
        IDOS->Printf("  avg cycles / rx batch = %10lu\n", rx_total / rx_calls);
        IDOS->Printf("    of which hook       = %10lu\n", rx_hook / rx_calls);
    }
    if (rx_pkts) {
        IDOS->Printf("  avg pkts per batch    = %10lu\n", rx_pkts / rx_calls);
        IDOS->Printf("  avg cycles per RX pkt = %10lu\n", rx_total / rx_pkts);
        IDOS->Printf("  avg time per RX pkt   = %10lu us\n",
                     (rx_total / rx_pkts) / 100UL);
    }

    IDOS->Printf("\nRaw counters (for delta calculation):\n");
    IDOS->Printf("  tx_calls    %10lu\n", tx_calls);
    IDOS->Printf("  tx_c_total  %10lu\n", tx_total);
    IDOS->Printf("  tx_c_cook   %10lu\n", tx_cook);
    IDOS->Printf("  tx_c_flush  %10lu\n", tx_flush);
    IDOS->Printf("  tx_c_ring   %10lu\n", tx_ring);
    IDOS->Printf("  tx_c_notify %10lu\n", tx_notify);
    IDOS->Printf("  rx_calls    %10lu\n", rx_calls);
    IDOS->Printf("  rx_pkts     %10lu\n", rx_pkts);
    IDOS->Printf("  rx_c_total  %10lu\n", rx_total);
    IDOS->Printf("  rx_c_hook   %10lu\n", rx_hook);

out:
    IExec->CloseDevice((struct IORequest *)req);
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);
    return 0;
}
