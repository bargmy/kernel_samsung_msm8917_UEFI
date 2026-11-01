#include <Uefi.h>
#define DEBUG_ERROR 1
#define DEBUG_INFO 2
void DebugPrint(UINTN,const char *,...);
#define DEBUG(expression) DebugPrint expression
