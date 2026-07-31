/*
 * testrx — prove RX at the hardware level.
 *
 * Sends a broadcast ARP request for QEMU's user-mode gateway
 * (192.168.100.2), waits ~1 s, and asks the driver whether any RX
 * descriptors got DMA'd. The gateway responds to ARP, so a healthy
 * RX path shows either RDH advanced from 0 or at least one RX
 * descriptor's DD bit set.
 *
 * Layout of the ARP frame (Ethernet II + ARP):
 *   [0..5]   dst MAC = ff:ff:ff:ff:ff:ff  (broadcast)
 *   [6..11]  src MAC = our factory MAC
 *   [12..13] ethertype = 0x0806 (ARP)
 *   [14..15] hardware type = 0x0001 (Ethernet)
 *   [16..17] protocol type = 0x0800 (IPv4)
 *   [18]     HW addr len = 6
 *   [19]     proto addr len = 4
 *   [20..21] operation = 0x0001 (request)
 *   [22..27] sender HW addr = our MAC
 *   [28..31] sender proto addr = 192.168.100.10 (arbitrary; QEMU doesn't
 *                                                enforce, we're not DHCP-ed)
 *   [32..37] target HW addr = 00:00:00:00:00:00 (unknown, being asked)
 *   [38..41] target proto addr = 192.168.100.2 (QEMU's gateway address in
 *                                               our custom subnet)
 * Total: 42 bytes, padded to 60 by the driver's TCTL.PSP if needed.
 */

#include <exec/errors.h>
#include <exec/io.h>
#include <devices/sana2.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "../include/virtnet.h"

static LONG do_cmd(struct IOSana2Req *req, UWORD cmd)
{
    req->ios2_Req.io_Command = cmd;
    req->ios2_Req.io_Error   = 0;
    req->ios2_WireError      = 0;
    return IExec->DoIO((struct IORequest *)req);
}

/* Unpack fields the driver stashed via VN_DBG_STATUS. */
static ULONG unpack_be32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16)
         | ((ULONG)p[2] << 8)  |  (ULONG)p[3];
}
static ULONG unpack_be16(const UBYTE *p)
{
    return ((ULONG)p[0] << 8) | (ULONG)p[1];
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

    LONG err = IExec->OpenDevice("virtnet.device", 0, (struct IORequest *)req, 0);
    if (err != 0) { IDOS->Printf("RESULT: FAIL OpenDevice=%ld\n", (long)err); goto out2; }

    for (int i = 0; i < 16; i++) req->ios2_SrcAddr[i] = 0;
    do_cmd(req, S2_CONFIGINTERFACE);
    LONG rc_on = do_cmd(req, S2_ONLINE);
    if (rc_on != 0) { IDOS->Printf("RESULT: FAIL ONLINE=%ld\n", (long)rc_on); goto out; }

    /* Get our MAC address so we can populate the ARP src fields. */
    do_cmd(req, S2_GETSTATIONADDRESS);
    UBYTE my_mac[6];
    for (int i = 0; i < 6; i++) my_mac[i] = req->ios2_SrcAddr[i];
    IDOS->Printf("our MAC: %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n",
                 (unsigned long)my_mac[0], (unsigned long)my_mac[1],
                 (unsigned long)my_mac[2], (unsigned long)my_mac[3],
                 (unsigned long)my_mac[4], (unsigned long)my_mac[5]);

    /* Baseline RX state. */
    do_cmd(req, VN_DBG_STATUS);
    ULONG rdh_before = unpack_be32(req->ios2_SrcAddr + 4);
    ULONG dd_before  = unpack_be32(req->ios2_DstAddr + 0);
    IDOS->Printf("RX before send: RDH=%lu dd_count=%lu\n",
                 (unsigned long)rdh_before, (unsigned long)dd_before);

    /* Build the ARP request frame. */
    UBYTE frame[60];
    for (int i = 0; i < 60; i++) frame[i] = 0;
    for (int i = 0; i < 6;  i++) frame[i] = 0xff;                    /* dst = bcast */
    for (int i = 0; i < 6;  i++) frame[6 + i] = my_mac[i];           /* src = us */
    frame[12] = 0x08; frame[13] = 0x06;                              /* ethertype = ARP */
    frame[14] = 0x00; frame[15] = 0x01;                              /* HW type = Ethernet */
    frame[16] = 0x08; frame[17] = 0x00;                              /* Proto type = IPv4 */
    frame[18] = 6;    frame[19] = 4;                                 /* addr lens */
    frame[20] = 0x00; frame[21] = 0x01;                              /* op = request */
    for (int i = 0; i < 6;  i++) frame[22 + i] = my_mac[i];          /* sender HW = us */
    frame[28] = 192; frame[29] = 168; frame[30] = 100; frame[31] = 10; /* sender IP */
    /* target HW = 0 (bytes 32-37 already zero) */
    frame[38] = 192; frame[39] = 168; frame[40] = 100; frame[41] = 2;  /* target IP = QEMU gw */

    req->ios2_Req.io_Command = CMD_WRITE;
    req->ios2_Req.io_Error   = 0;
    req->ios2_WireError      = 0;
    req->ios2_PacketType     = 0x0806;
    req->ios2_Data           = frame;
    req->ios2_DataLength     = sizeof(frame);
    for (int i = 0; i < 6; i++) req->ios2_DstAddr[i] = 0xff;

    LONG rc_tx = IExec->DoIO((struct IORequest *)req);
    IDOS->Printf("ARP request sent: rc=%ld io_Error=%ld\n",
                 (long)rc_tx, (long)req->ios2_Req.io_Error);

    /* Give QEMU's user-mode NAT time to see the ARP and reply. */
    IDOS->Delay(50);   /* 1 second */

    /* Check RX state — did anything land? */
    do_cmd(req, VN_DBG_STATUS);
    ULONG rdh_after  = unpack_be32(req->ios2_SrcAddr + 4);
    ULONG dd_after   = unpack_be32(req->ios2_DstAddr + 0);
    ULONG first_len  = unpack_be16(req->ios2_DstAddr + 4);
    ULONG irq_count  = req->ios2_DataLength;
    ULONG last_icr   = req->ios2_WireError;
    IDOS->Printf("RX after wait:  RDH=%lu dd_count=%lu first_len=%lu\n",
                 (unsigned long)rdh_after, (unsigned long)dd_after,
                 (unsigned long)first_len);
    IDOS->Printf("  irq_counter=%lu last_icr=0x%08lx\n",
                 (unsigned long)irq_count, (unsigned long)last_icr);

    do_cmd(req, S2_OFFLINE);

out:
    IExec->CloseDevice((struct IORequest *)req);
out2:
    IExec->FreeSysObject(ASOT_IOREQUEST, req);
    IExec->FreeSysObject(ASOT_PORT, port);

    /* PASS if the hardware advanced RDH or set a descriptor's DD bit,
     * meaning QEMU DMA'd a frame into our ring. Frame content is a
     * separate concern — that lands with Phase 6j (per-opener state
     * + CopyToBuff hooks). */
    BOOL got_rx = (rdh_after != rdh_before) || (dd_after > 0);
    if (got_rx) {
        IDOS->Printf("RESULT: PASS (RX ring received a frame)\n");
    } else {
        IDOS->Printf("RESULT: FAIL no RX (RDH=%lu dd=%lu). QEMU NAT may not\n"
                     "        ARP-reply on n1's subnet — retry with a\n"
                     "        different target or check RCTL config.\n",
                     (unsigned long)rdh_after, (unsigned long)dd_after);
    }
    return got_rx ? 0 : 20;
}
