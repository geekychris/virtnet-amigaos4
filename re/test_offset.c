#include <proto/exec.h>
int main() { 
    APTR r = IExec->OpenResource("newmemory.resource");
    APTR r2 = IExec->GetInterface(NULL, "main", 1, NULL);
    IExec->AddIntServer(48, NULL);
    return r ? 1 : 0;
}
