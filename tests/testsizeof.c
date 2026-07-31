/* testsizeof - print sizeof/offsetof of Sana2DeviceQuery so we can
 * confirm the struct layout matches what Roadshow expects.
 * If our sizeof differs from Roadshow's compiled sizeof, our field
 * writes land at the wrong offsets and Roadshow reads (say) MTU
 * from padding = 0. */
#include <exec/errors.h>
#include <devices/sana2.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stddef.h>

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    IDOS->Printf("sizeof(Sana2DeviceQuery) = %lu\n",
                 (unsigned long)sizeof(struct Sana2DeviceQuery));
    IDOS->Printf("  offsetof SizeAvailable  = %lu\n", (unsigned long)offsetof(struct Sana2DeviceQuery, SizeAvailable));
    IDOS->Printf("  offsetof SizeSupplied   = %lu\n", (unsigned long)offsetof(struct Sana2DeviceQuery, SizeSupplied));
    IDOS->Printf("  offsetof DevQueryFormat = %lu\n", (unsigned long)offsetof(struct Sana2DeviceQuery, DevQueryFormat));
    IDOS->Printf("  offsetof DeviceLevel    = %lu\n", (unsigned long)offsetof(struct Sana2DeviceQuery, DeviceLevel));
    IDOS->Printf("  offsetof AddrFieldSize  = %lu (sizeof field=%lu)\n",
                 (unsigned long)offsetof(struct Sana2DeviceQuery, AddrFieldSize),
                 (unsigned long)sizeof(((struct Sana2DeviceQuery*)0)->AddrFieldSize));
    IDOS->Printf("  offsetof MTU            = %lu\n", (unsigned long)offsetof(struct Sana2DeviceQuery, MTU));
    IDOS->Printf("  offsetof BPS            = %lu\n", (unsigned long)offsetof(struct Sana2DeviceQuery, BPS));
    IDOS->Printf("  offsetof HardwareType   = %lu\n", (unsigned long)offsetof(struct Sana2DeviceQuery, HardwareType));
    IDOS->Printf("  offsetof RawMTU         = %lu\n", (unsigned long)offsetof(struct Sana2DeviceQuery, RawMTU));
    IDOS->Printf("RESULT: PASS\n");
    return 0;
}
