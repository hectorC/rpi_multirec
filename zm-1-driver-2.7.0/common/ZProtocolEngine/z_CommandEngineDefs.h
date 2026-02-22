


#ifndef Z_COMMAND_ENGINE_DEFS_H
#define Z_COMMAND_ENGINE_DEFS_H


#define Z_VERSION_ROM_SIZE_LOG2     3                                
#define Z_VERSION_ROM_SIZE          (1<<(Z_VERSION_ROM_SIZE_LOG2+1)) 

#define Z_NOTIFY_VERSION_ROM        1   
#define Z_NOTIFY_LED_BRIGHTNESS     2   

#define Z_CAP_BRIGHTNESS            0x01    

#define Z_NUM_OF_FREQ_INFINEON 1
#define Z_NUM_OF_FREQ_ANALOG 9
#define Z_NUM_OF_FREQ_DIGITAL 6
#define Z_NUM_OF_FREQ_MAX 9

#endif
