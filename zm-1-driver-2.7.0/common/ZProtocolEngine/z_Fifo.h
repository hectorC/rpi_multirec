




#ifndef Z_FIFO_H
#define Z_FIFO_H

#ifdef __cplusplus
extern "C" {
#endif

#if (LINUX_KERNEL_MODULE == 1)

#include <linux/kernel.h>
#include <linux/slab.h>

#elif (MACOS_KERNEL_MODULE == 1)

#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>

#elif (WINDOWS_KERNEL_MODULE == 1)

#define Z_FIFO_POOL_TAG  'fifZ'

#include "z_Types.h"

#include <wdm.h>
#include <windef.h>

#elif (WINDOWS_USER_MODULE == 1)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#elif (MACOS_USER_MODULE == 1)

#include <DriverKit/IOLib.h>
#include <os/log.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#else

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#endif




typedef struct zFifo
{
    void**          buffer;                     
    int             size;                       

    int             rdPtr;                      
    int             wrPtr;                      

    int             remaining;                  
    int             occupancy;                  

#if (LINUX_KERNEL_MODULE == 1)
    spinlock_t      lock;                       
    unsigned long   lockFlags;                  
#elif (MACOS_KERNEL_MODULE == 1)
    IOSimpleLock*   lock;                       
#elif (WINDOWS_KERNEL_MODULE == 1)
    KSPIN_LOCK      lock;                       
    KIRQL           lockFlags;
#elif (WINDOWS_USER_MODULE == 1)
    HANDLE              mutex;                  
#endif

} zFifo;


zFifo*          z_FifoCreate   (int a_Size);
zFifo*          z_FifoDelete   (zFifo* a_Fifo);

int             z_FifoPush     (zFifo* a_Fifo, void*  a_Data);
int             z_FifoPop      (zFifo* a_Fifo, void** a_Data);
int             z_FifoClear    (zFifo* a_Fifo);

#ifdef __cplusplus
}
#endif

#endif
