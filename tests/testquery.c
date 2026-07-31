/* testquery — QueryInterfaceTagList on a named interface, dump the
 * fields that would explain the "No buffer space available" error.
 * Usage: testquery <ifname>
 */

#include <exec/exec.h>
#include <utility/tagitem.h>
#include <libraries/bsdsocket.h>
#include <interfaces/bsdsocket.h>
#include <proto/exec.h>
#include <proto/dos.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        IDOS->Printf("usage: testquery <ifname>\n");
        return 20;
    }
    struct Library *socketBase = IExec->OpenLibrary("bsdsocket.library", 4);
    if (!socketBase) { IDOS->Printf("RESULT: FAIL OpenLibrary\n"); return 20; }
    struct SocketIFace *ISocket = (struct SocketIFace *)IExec->GetInterface(socketBase, "main", 1, NULL);
    if (!ISocket) { IDOS->Printf("RESULT: FAIL GetInterface\n"); IExec->CloseLibrary(socketBase); return 20; }

    LONG num_read = -1, max_read = -1, num_write = -1, max_write = -1;
    LONG num_arp = -1, num_read_pending = -1;
    ULONG hw_type = 0xFFFFFFFF, mtu = 0xFFFFFFFF, hw_mtu = 0xFFFFFFFF;
    LONG state = -1, addrbind = -1;

    struct TagItem tags[] = {
        { IFQ_NumReadRequests,        (ULONG)&num_read },
        { IFQ_MaxReadRequests,        (ULONG)&max_read },
        { IFQ_NumWriteRequests,       (ULONG)&num_write },
        { IFQ_MaxWriteRequests,       (ULONG)&max_write },
        { IFQ_NumReadRequestsPending, (ULONG)&num_read_pending },
        { IFQ_HardwareType,           (ULONG)&hw_type },
        { IFQ_MTU,                    (ULONG)&mtu },
        { IFQ_HardwareMTU,            (ULONG)&hw_mtu },
        { IFQ_State,                  (ULONG)&state },
        { IFQ_AddressBindType,        (ULONG)&addrbind },
        { TAG_END, 0 },
    };
    LONG rc = ISocket->QueryInterfaceTagList((STRPTR)argv[1], tags);
    IDOS->Printf("QueryInterfaceTagList('%s') = %ld\n", (const char *)argv[1], (long)rc);
    IDOS->Printf("  NumReadRequests   = %ld\n", (long)num_read);
    IDOS->Printf("  MaxReadRequests   = %ld\n", (long)max_read);
    IDOS->Printf("  NumWriteRequests  = %ld\n", (long)num_write);
    IDOS->Printf("  MaxWriteRequests  = %ld\n", (long)max_write);
    IDOS->Printf("  NumReadRequestsPending = %ld\n", (long)num_read_pending);
    IDOS->Printf("  HardwareType      = %lu (0x%lx)\n", (unsigned long)hw_type, (unsigned long)hw_type);
    IDOS->Printf("  MTU               = %lu\n", (unsigned long)mtu);
    IDOS->Printf("  HardwareMTU       = %lu\n", (unsigned long)hw_mtu);
    IDOS->Printf("  State             = %ld\n", (long)state);
    IDOS->Printf("  AddressBindType   = %ld\n", (long)addrbind);

    IExec->DropInterface((struct Interface *)ISocket);
    IExec->CloseLibrary(socketBase);
    IDOS->Printf("RESULT: %s\n", (rc == 0) ? "PASS" : "FAIL");
    return (rc == 0) ? 0 : 20;
}
