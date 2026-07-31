/* testbsdadd - bypass AddNetInterface, call bsdsocket->AddInterfaceTagList()
 * directly with a minimal tag list. Isolates whether the "of 0 bytes" /
 * ENOBUFS failure comes from AddNetInterface's parse layer or bsdsocket
 * itself.
 *
 * If this test's interface bind succeeds and reports IPREQUESTS>0 in the
 * bsdsocket data, AddNetInterface is the culprit.
 * If it still fails the same way, bsdsocket doesn't recognize
 * IFA_NumReadRequests on this entry point.
 */

#include <exec/errors.h>
#include <exec/exec.h>
#include <utility/tagitem.h>
#include <libraries/bsdsocket.h>
#include <interfaces/bsdsocket.h>
#include <proto/exec.h>
#include <proto/dos.h>

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    struct Library *socketBase = IExec->OpenLibrary("bsdsocket.library", 4);
    if (!socketBase) {
        IDOS->Printf("RESULT: FAIL OpenLibrary(bsdsocket.library, 4)\n");
        return 20;
    }
    struct SocketIFace *ISocket = (struct SocketIFace *)IExec->GetInterface(socketBase, "main", 1, NULL);
    if (!ISocket) {
        IDOS->Printf("RESULT: FAIL GetInterface(bsdsocket main)\n");
        IExec->CloseLibrary(socketBase);
        return 20;
    }

    /* Build tag list — only IPREQUESTS/WRITEREQUESTS. Skip everything
     * else (defaults). If bsdsocket honors these tags on this entry
     * point, offset 204 of the descriptor should become 32. */
    struct TagItem tags[] = {
        { IFA_IPType,           0x800  }, /* Ethernet-encapsulated IP */
        { IFA_ARPType,          0x806  }, /* Ethernet-encapsulated ARP */
        { IFA_NumReadRequests,  32 },
        { IFA_NumWriteRequests, 32 },
        { IFA_NumARPRequests,   32 },
        { TAG_END,              0  },
    };

    /* Use a distinct name so it doesn't collide with the already-added
     * "virtnet" interface. Same driver, different logical instance. */
    /* Use a fresh name each run so we can retry without reboot */
    STRPTR ifname = (argc > 1) ? (STRPTR)argv[1] : (STRPTR)"virtebsd2";
    LONG rc = ISocket->AddInterfaceTagList(
        ifname,
        (STRPTR)"virtnet.device",      /* device_name    */
        0,                                /* unit           */
        tags);
    IDOS->Printf("(name='%s')\n", (const char *)ifname);
    IDOS->Printf("AddInterfaceTagList('virtebsd', 'virtnet.device', 0, {NumReadReq=32, NumWriteReq=32, NumARPReq=32}) = %ld\n",
                 (long)rc);

    if (rc == 0) {
        IDOS->Printf("Success — interface added. Try:\n"
                     "  ConfigureNetInterface virtebsd ADDRESS=192.168.100.15 NETMASK=255.255.255.0 UP\n"
                     "and see if the No-buffer-space-available error is gone.\n");
    } else {
        IDOS->Printf("Failure — rc != 0. Check error semantics in header.\n");
    }

    IExec->DropInterface((struct Interface *)ISocket);
    IExec->CloseLibrary(socketBase);
    IDOS->Printf("RESULT: %s\n", (rc == 0) ? "PASS" : "FAIL");
    return (rc == 0) ? 0 : 20;
}
