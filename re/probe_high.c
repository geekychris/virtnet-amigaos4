#include <proto/exec.h>
int main() {
    IExec->CachePreDMA(NULL, NULL, 0);
    IExec->CacheClearE(NULL, 0, 0);
    IExec->CachePostDMA(NULL, NULL, 0);
    IExec->GetDMAList(NULL, NULL, 0, 0);
    IExec->StartDMA(NULL, 0, 0);
    IExec->EndDMA(NULL, 0, 0);
    return 0;
}
