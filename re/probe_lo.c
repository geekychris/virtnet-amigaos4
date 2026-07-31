#include <proto/exec.h>
int main() {
    IExec->Obtain();
    IExec->Release();
    IExec->AddHead(NULL, NULL);
    IExec->AddMemHandler(NULL);
    IExec->AddTail(NULL, NULL);
    IExec->AllocPooled(NULL, 0);
    IExec->AllocVec(0, 0);
    IExec->AllocVecPooled(NULL, 0);
    IExec->AvailMem(0);
    IExec->CopyMem(NULL, NULL, 0);
    IExec->CopyMemQuick(NULL, NULL, 0);
    IExec->Enqueue(NULL, NULL);
    IExec->FindName(NULL, NULL);
    IExec->FindIName(NULL, NULL);
    IExec->Forbid();
    IExec->FreeEntry(NULL);
    IExec->FreePooled(NULL, NULL, 0);
    IExec->FreeVec(NULL);
    IExec->FreeVecPooled(NULL, NULL);
    IExec->Insert(NULL, NULL, NULL);
    return 0;
}
