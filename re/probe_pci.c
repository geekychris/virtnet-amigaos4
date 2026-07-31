#include <proto/exec.h>
#include <proto/expansion.h>
#include <expansion/pci.h>

struct PCIIFace *IPCI;
extern struct PCIIFace *IPCI;

int main() {
    struct PCIDevice *pd = (struct PCIDevice *)0x1000;
    ULONG a = pd->InLong(0x100);
    pd->OutLong(0x104, 0x12345678);
    ULONG b = pd->ReadConfigLong(0x10);
    pd->WriteConfigLong(0x10, 0);
    ULONG c = pd->MapInterrupt();
    return (int)a;
}
