




#ifndef Z_PROTOCOL_ENGINE_H
#define Z_PROTOCOL_ENGINE_H

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

#define Z_PROTOCOL_ENGINE_POOL_TAG  'trpZ'

#include "z_Types.h"

#include <wdm.h>
#include <windef.h>
#include <stdarg.h>

#elif (WINDOWS_USER_MODULE == 1)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#elif (MACOS_USER_MODULE == 1)

#include <DriverKit/IOLib.h>

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
#include <string.h>

#endif


#define Z_ENGINE_MAX_CHANNELS   40  


enum
{
    Z_ENGINE_IDLE       = 0,        
    Z_ENGINE_CTRL_WORD,             
    Z_ENGINE_DATA_MSB,              
    Z_ENGINE_DATA_LSB               
};


typedef struct zProtoEngine
{
    int             verboseLevel;   

    void*           userContext;    
    void*           userData;       

    int             channelCount;   
    int             bitsPerSample;  

#if (LINUX_KERNEL_MODULE == 1)
    spinlock_t      lock;           
    unsigned long   lockFlags;      
#elif (MACOS_KERNEL_MODULE == 1)
    IOSimpleLock*   lock;           
#elif (WINDOWS_KERNEL_MODULE == 1)
    KSPIN_LOCK      lock;           
    KIRQL           lockFlags;
#elif (WINDOWS_USER_MODULE == 1)
    HANDLE          mutex;          
#endif

    uint8_t         rxBlock[8];     
    int             rxBlockPtr;     

    int             rxState;        
    uint32_t        rxWord;         
    int             rxWordPtr;      

    int32_t         rxSamples[Z_ENGINE_MAX_CHANNELS];   
    int             rxSamplePtr;                        

    
    int         (*dataCallback)(struct zProtoEngine* a_Engine, void* a_Data, int a_Count); 
    int         (*ctrlCallback)(struct zProtoEngine* a_Engine, void* a_Data, int a_Count); 

} zProtoEngine;

typedef int (*zProtoCallback)(zProtoEngine* a_Engine, void* a_Data, int a_Count);   


void            z_ProtoLog         (zProtoEngine* a_Engine, int a_Level, const char* a_Format, ...);

zProtoEngine*   z_ProtoCreate(void);
void            z_ProtoInit(zProtoEngine* engine, void* a_UserContext, void* a_UserData);
zProtoEngine*   z_ProtoDelete(zProtoEngine* a_Engine);

int             z_ProtoReset       (zProtoEngine* a_Engine);

int z_ProtoResetStreamFormat(zProtoEngine* a_Engine);

int             z_ProtoSetDataCallback (zProtoEngine* a_Engine, zProtoCallback a_Callback);
int             z_ProtoSetCtrlCallback (zProtoEngine* a_Engine, zProtoCallback a_Callback);

int             z_ProtoDecode      (zProtoEngine* a_Engine, uint8_t* a_Buffer, int a_Length);

int             z_ProtoEncodeCtrl  (zProtoEngine* a_Engine, uint32_t a_CtrlWord, uint8_t* a_Buffer, int* a_Length);

#ifdef __cplusplus
}
#endif

#endif