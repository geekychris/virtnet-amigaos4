#include <proto/exec.h>
#include <exec/exectags.h>
int main() {
    APTR o1 = IExec->AllocSysObject(ASOT_LIST, NULL);
    APTR o2 = IExec->AllocSysObjectTags(ASOT_MUTEX, TAG_END);
    APTR o3 = IExec->AllocVecTags(1024, TAG_END);
    APTR o4 = IExec->CachePreDMA(NULL, NULL, 0);
    IExec->CacheClearE(NULL, 0, 0);
    return (int)(ULONG)o1;
}
