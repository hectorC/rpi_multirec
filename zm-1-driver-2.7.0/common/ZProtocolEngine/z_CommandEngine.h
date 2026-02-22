




#ifndef Z_COMMAND_ENGINE_H
#define Z_COMMAND_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif


#if (LINUX_KERNEL_MODULE == 1)

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/jiffies.h>

#elif (MACOS_KERNEL_MODULE == 1)

#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>

#elif (WINDOWS_KERNEL_MODULE == 1)

#define Z_CMD_ENGINE_POOL_TAG  ' ecZ'

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
#include <time.h>

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

#include "z_Command.h"
#include "z_Fifo.h"
#include "z_CommandEngineDefs.h"

struct zCommandEngine_;
typedef int (*NotificationCallback)(struct zCommandEngine_*, int32_t, void*);


typedef struct zCommandEngine_
{
    int                 verboseLevel;               

    void*               userContext;                
    void*               userData;                   

    struct zFifo*       commandQueue;               

    struct zCommand*    currCommand;                
    int64_t             timeoutTime;                
#if (LINUX_KERNEL_MODULE == 1)
    unsigned long       initialJiffies;             
#endif


#if (LINUX_KERNEL_MODULE == 1)
    spinlock_t          lock;                       
    unsigned long       lockFlags;
#elif (MACOS_KERNEL_MODULE == 1)
    IOSimpleLock*       lock;                       
#elif (WINDOWS_KERNEL_MODULE == 1)
    KSPIN_LOCK          lock;                       
    KIRQL               lockFlags;
#elif (WINDOWS_USER_MODULE == 1)
    HANDLE              mutex;                      
#endif

    uint16_t            deviceRegisters[4];         
    uint8_t             caps;                       

    
    struct
    {
        uint8_t         data    [Z_VERSION_ROM_SIZE];   
        char            string  [Z_VERSION_ROM_SIZE+1]; 
        uint8_t         isValid;                        
        uint8_t         isValidRes [Z_VERSION_ROM_SIZE/2]; 
    } versionRom;

    
    struct
    {
        uint8_t         r, g, b;                        
        uint8_t         brightness;                     
    } led;

    
    NotificationCallback notificationCallback; 

} zCommandEngine;



enum
{
    Z_AVG_PASSTHROUGH = 0,  
    Z_AVG_ODD_PASTHROUGH,   
    Z_AVG_EVEN_PASSTHROUGH, 
    Z_AVG_AVERAGE           
};


zCommandEngine* z_CommandEngineCreate(void);
void z_CommandEngineInit   (zCommandEngine* engine, void* a_UserContext, void* a_UserData);
zCommandEngine* z_CommandEngineDelete   (zCommandEngine* a_Engine);

int z_CommandEngineSetNotificationCallback  (zCommandEngine* a_Engine,
                                             NotificationCallback a_Callback);

int z_CommandEnginePush     (zCommandEngine* a_Engine, zCommand* a_Command);
int z_CommandEngineProcess  (zCommandEngine* a_Engine, uint32_t* a_CtrlWord);
int z_CommandEngineReceive  (zCommandEngine* a_Engine, uint32_t  a_CtrlWord);
int z_CommandEngineIsPending(zCommandEngine* a_Engine);

int z_CmdReadRegisters      (zCommandEngine* a_Engine);
int z_CmdReadVersionROM     (zCommandEngine* a_Engine);
void z_CmdClearVersionROM   (zCommandEngine* a_Engine);

int z_CmdEnableStreaming    (zCommandEngine* a_Engine, int a_Enable);
int z_CmdSetSampleRate      (zCommandEngine* a_Engine, int a_SampleRate);
int z_CmdSetSampleFormat    (zCommandEngine* a_Engine, int a_Format);
int z_CmdSetChannelCount    (zCommandEngine* a_Engine, int a_Channels);
int z_CmdSetAveragingMode   (zCommandEngine* a_Engine, int a_Mode);
int z_CmdSetDigitalGain     (zCommandEngine* a_Engine, int a_Gain);
int z_CmdSetLedColor        (zCommandEngine* a_Engine, int a_Red, int a_Green, int a_Blue);
int z_CmdSetLedBrightness   (zCommandEngine* a_Engine, int a_Brightness);
int z_CmdSetTestMode        (zCommandEngine* a_Engine, int a_TestMode);

int z_CmdGetLedBrightness   (zCommandEngine* a_Engine);


void z_GetAvailableSampleRates(const char* a_Serial, unsigned int* a_SampleFrequencyTable, int* a_SampleFrequencyTableSize);

#ifdef __cplusplus
}
#endif

#endif

