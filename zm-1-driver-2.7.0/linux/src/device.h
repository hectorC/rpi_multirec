




#ifndef __Z_DEVICE_H__
#define __Z_DEVICE_H__


#include <linux/module.h>
#include <linux/kernel.h>

#include <linux/cdev.h>
#include <linux/kfifo.h>

#include <sound/pcm.h>

#include "usb.h"
#include "ioctl.h"
#include "z_ProtocolEngine.h"
#include "z_CommandEngine.h"
#include "z_CommandEngineDefs.h"

#include "defs.h"




typedef struct z_device_ctx {

    
    int                             index;      

    
    struct z_device_info            info;       

    
    struct {

        struct usb_device*          dev;        
        struct usb_interface*       interface;  

        int                         ep_in;      
        int                         ep_in_size; 

        int                         ep_out;     
        int                         ep_out_size;

        struct z_urb_ctx            urbs_in [Z_MAX_URB_IN_LIST];      
        struct z_urb_ctx            urbs_out[Z_MAX_URB_OUT_LIST];     

        struct timer_list           timer;      
        spinlock_t                  lock;       

        bool                        isDisconnected; 

#if (Z_DEBUG_THROUGHPUT == 1)
        struct timer_list           dbg_timer;          
        uint32_t                    dbg_jiffies;        

        int                         dbg_in_urb_count;   
        int                         dbg_out_urb_count;  
        int                         dbg_raw_count;      
        int                         dbg_dat_count;      
        int                         dbg_frame_count;    
#endif

    } usb;

    
    struct {

        zProtoEngine*               engine;             

        int                         skipFrames;         

    } proto;

    
    struct {

        zCommandEngine*             engine;             

    } ctrl;

    
    struct {

        struct snd_card*            card;               
        struct snd_pcm*             pcm;                
        struct snd_pcm_substream*   substream;          

        struct mutex                lock;               

        struct timer_list           timer;              
        spinlock_t                  timer_lock;         

        uint8_t                     pcm_running;        
        uint32_t                    pcm_sample_bytes;   
        uint32_t                    pcm_buffer_bytes;   
        uint32_t                    pcm_buffer_ptr;     
        uint32_t                    pcm_period_bytes;   
        uint32_t                    pcm_period_ptr;     

        struct kfifo                fifo;               

        int                         testMode;           

    } snd;

    
    struct {

        uint8_t                     controlMode;        
        uint8_t                     color[3];           

    } led;

    
    struct {

        char                        name[32];
        dev_t                       id;
        struct cdev*                dev;

        int                         openCount;

    } chr;

    
    struct {

        uint32_t correctionGains[Z_EEPROM_GAINS_NUM];    
        uint32_t correctionGlobalGain;                   
        uint32_t masterGain;                             

        uint64_t correctionValues[Z_EEPROM_GAINS_NUM];   
        bool     correctionEnabled;                      

        struct snd_kcontrol* masterGainControl;

    } gain;

    
    unsigned int sampleRatesTable[Z_NUM_OF_FREQ_MAX];

    struct snd_pcm_hw_constraint_list z_pcm_rate_constraint;
} z_device_ctx;




extern int z_dev_data_cb(zProtoEngine* engine, void* data, int count);
extern int z_dev_ctrl_cb(zProtoEngine* engine, void* data, int count);

extern int z_dev_notify_cb(zCommandEngine* a_Engine, int32_t a_Code, void* a_Data);

extern void z_dev_set_led_mode      (z_device_ctx* ctx, uint8_t a_Mode);
extern void z_dev_update_led_color  (z_device_ctx* ctx);

extern void z_dev_set_correction_enabled    (z_device_ctx* ctx, uint8_t a_Enabled);

void z_dev_update_gains(z_device_ctx* ctx);


int z_read_eeprom(struct z_device_ctx* a_Ctx, uint8_t a_Buffer[Z_IOCTL_BUFFER_SIZE], uint8_t a_Addr, uint8_t a_BytesToRead);

#endif 
