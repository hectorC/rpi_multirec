




#ifndef Z_COMMAND_H
#define Z_COMMAND_H

#ifdef __cplusplus
extern "C" {
#endif


#if (LINUX_KERNEL_MODULE == 1)
#pragma message("LINUX_KERNEL_MODULE")
#include <linux/kernel.h>
#include <linux/slab.h>

#elif (MACOS_KERNEL_MODULE == 1)
#pragma message("MACOS_KERNEL_MODULE")
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>

#elif (WINDOWS_KERNEL_MODULE == 1)
#pragma message("WINDOWS_KERNEL_MODULE")
#define Z_CMD_POOL_TAG  'dmcZ'

#include "z_Types.h"

#include <wdm.h>
#include <windef.h>

#elif (MACOS_USER_MODULE == 1)

#include <DriverKit/IOLib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#else

#pragma message("USER_MODULE")
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#endif




enum
{
    Z_CMD_I2C = 0,      
    Z_CMD_VERSION_ROM,  
    Z_CMD_REG,          
    Z_CMD_COMMIT,       
    Z_CMD_LED           
};


typedef struct
{
    uint8_t     address;                
    uint16_t    value;                  
    int32_t     waitMs;                 
} i2cRegister;


typedef struct zCommand
{
    int                 type;           

#if (WINDOWS_KERNEL_MODULE == 1 || WINDOWS_USER_MODULE == 1)
#pragma warning( disable: 4201 ) 
#endif
    union
    {
        
        struct
        {
            uint8_t     chip;           
            uint8_t     wr;             
            i2cRegister reg;            
        }               i2c;            

        
        struct
        {
            uint8_t     address;        
        }               versionRom;

        
        struct
        {
            uint8_t     wr;             
            uint8_t     address;        
            uint16_t    value;          
        }               reg;            

        
        struct
        {
            uint8_t     red;            
            uint8_t     green;          
            uint8_t     blue;           
        }               led;            
    };

    uint32_t            waitMs;         

} zCommand;


zCommand*       z_CmdNew            (void);
zCommand*       z_CmdDelete         (zCommand* a_Cmd);

zCommand*       z_CmdNewRegRd       (uint8_t a_Address);
zCommand*       z_CmdNewRegWr       (uint8_t a_Address, uint16_t a_Value);
zCommand*       z_CmdNewVersionRd   (uint8_t a_Address);
zCommand*       z_CmdNewI2cRd       (int a_Chip, uint8_t a_Address, int a_WaitMs);
zCommand*       z_CmdNewI2cWr       (int a_Chip, uint8_t a_Address, uint16_t a_Value, int a_WaitMs);

zCommand*       z_CmdNewCommit      (void);
zCommand*       z_CmdNewLedColor    (uint8_t a_Red, uint8_t a_Green, uint8_t a_Blue);

#ifdef __cplusplus
}
#endif

#endif

