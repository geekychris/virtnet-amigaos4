#include <proto/exec.h>
int main() {
    IExec->AvailMem(0);
    IExec->AllocMem(0, 0);
    IExec->AllocPooled(NULL, 0);
    return 0;
}
