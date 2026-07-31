#include <proto/exec.h>
#include <exec/exectags.h>
int main() {
    APTR a = IExec->AllocVec(1024, 0);
    APTR b = IExec->AllocVecTags(1024, TAG_END);
    APTR c = IExec->AllocMem(1024, 0);
    APTR d = IExec->AllocPooled(NULL, 1024);
    APTR e = IExec->AllocSysObjectTags(3, TAG_END);
    return 0;
}
