#include <proto/exec.h>
#include <proto/expansion.h>
#include <expansion/pci.h>
int main() {
    struct PCIDevice *pd = (struct PCIDevice *)0x1000;
    pd->ReadConfigByte(0);
    pd->ReadConfigWord(0);
    pd->ReadConfigLong(0);
    pd->WriteConfigByte(0, 0);
    pd->WriteConfigWord(0, 0);
    pd->WriteConfigLong(0, 0);
    pd->InByte(0);
    pd->InWord(0);
    pd->InLong(0);
    pd->OutByte(0, 0);
    pd->OutWord(0, 0);
    pd->OutLong(0, 0);
    pd->InByteBlock(0, NULL, 0, 0, 0);
    pd->InWordBlock(0, NULL, 0, 0, 0);
    pd->InLongBlock(0, NULL, 0, 0, 0);
    pd->OutByteBlock(0, NULL, 0, 0, 0);
    pd->OutWordBlock(0, NULL, 0, 0, 0);
    pd->OutLongBlock(0, NULL, 0, 0, 0);
    pd->MapInterrupt();
    return 0;
}
