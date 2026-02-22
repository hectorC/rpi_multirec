



 
#ifndef Z_DEFS_H
#define Z_DEFS_H

#include <linux/version.h>

#ifndef NULL
#define NULL    0
#endif

#ifndef uint8_t
#define uint8_t unsigned char
#endif

#ifndef uint16_t
#define uint16_t unsigned short
#endif

#ifndef uint32_t
#define uint32_t unsigned int
#endif

#ifndef uint64_t
#define uint64_t long unsigned int
#endif

#ifndef int32_t
#define int32_t int
#endif

#ifndef va_list
#define va_list char*
#endif

typedef struct defs {
    uint32_t    Z_067F7118AAD81EF6BD0C;                 
    uint32_t    Z_1473D97CC6C2D067367F;     
    uint32_t    Z_025538939104A5F99E72;         

    uint32_t    Z_63E875E7FC1AC5FDBFD4;              
    uint16_t    Z_F6798FE13CF899722B23;       
} z_defs;

extern const z_defs Z_Defines;


typedef struct z_device_info {
    char                            deviceFriendlyName   [64];  
    char                            cardFriendlyName     [64];  
    char                            serialNumber         [64];  
} z_device_info;

int push_context(void* ctx, int deviceType);
void* get_context(int a_Ctx);
int remove_context_ptr(void* ctx);
int remove_context(int a_CtxIdx);



#ifndef Z_DRIVER_NAME
#define Z_DRIVER_NAME               "zm_1_driver"  
#endif


#define Z_DEBUG_INIT                0   
#define Z_DEBUG_URB                 0   
#define Z_DEBUG_THROUGHPUT          0   
#define Z_DEBUG_PCM                 0   
#define Z_DEBUG_MIXER               0   
#define Z_DEBUG_FIFO                0   
#define Z_DEBUG_CHARDEV             0   
#define Z_DEBUG_PROTO_VERBOSE       0   
#define Z_DEBUG_COMMAND_VERBOSE     0   
#define Z_DEBUG_CORRECTIONS         0   

#define FTDI_VID                    0x0403      
#define FTDI_PID                    0x6010      

#define ZYLIA_PID                   0x73F8      

#define FTDI_EEPROM_SIZE            128 

#define Z_MASTER_CORRECTION_ENABLED 0



#define Z_MAX_URB_IN_LIST           128


#define Z_MAX_URB_OUT_LIST          1


#define Z_CARD_NAME_PREFIX          "Zylia"             
#define Z_CARD_NAME_ADD_SN          1                   
#define Z_CARD_NAME_ADD_SUFFIX      0                   


#define Z_PCM_NAME                  "PCM"   

#define Z_PCM_MIN_CHANNELS          1       
#define Z_PCM_MAX_CHANNELS          40      

#define Z_PCM_MIN_PERIOD_SAMPLES    1       
#define Z_PCM_MAX_PERIOD_SAMPLES    2048    

#define Z_PCM_MIN_PERIODS           1       
#define Z_PCM_MAX_PERIODS           32      

#define Z_PCM_MIN_SAMPLE_BITS       16      
#define Z_PCM_MAX_SAMPLE_BITS       24      

#define Z_PCM_MIN_PERIOD_BYTES      (Z_PCM_MIN_CHANNELS * Z_PCM_MIN_PERIOD_SAMPLES * Z_PCM_MIN_SAMPLE_BITS/8) 
#define Z_PCM_MAX_PERIOD_BYTES      (Z_PCM_MAX_CHANNELS * Z_PCM_MAX_PERIOD_SAMPLES * Z_PCM_MAX_SAMPLE_BITS/8) 

#define Z_PCM_MAX_BUFFER            (Z_PCM_MAX_PERIOD_BYTES * Z_PCM_MAX_PERIODS) 


#define Z_MIXER_NAME                "ZM-1 Mixer"    

#define Z_DIGITAL_GAIN_MIN           0      
#define Z_DIGITAL_GAIN_MAX          +70     
#define Z_DIGITAL_GAIN_STEP          1      


#define Z_COLOR_MIN                 0x00
#define Z_COLOR_MAX                 0xFF
#define Z_COLOR_STEP                0x10


enum Z_LED_CONTROL {
    Z_LED_CONTROL_AUTOMATIC = 0,
    Z_LED_CONTROL_MANUAL = 1
};


#define Z_EEPROM_GAINS_NUM          19      
#define Z_EEPROM_GAINS_ADDR         0x33    


#define Z_CHARDEV_NAME              "zylia-zm-1s_%d"    




#define PRINT(fmt, args...) printk(KERN_DEBUG "%s(%d): " fmt "\n", __FUNCTION__, __LINE__, ## args)



#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,15,0)
#define Z_KERNEL_HAVE_TIMER_SETUP 1
#endif

#endif 
