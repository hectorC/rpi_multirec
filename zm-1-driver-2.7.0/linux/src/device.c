





#include "snd.h"
#include "device.h"
#include "chardev.h"
#include "ftdi.h"

#include "z_Utils.h"





int z_dev_data_cb(zProtoEngine* engine, void* data, int count) {
    int                     i;
    struct z_device_ctx*    ctx         = (struct z_device_ctx*)engine->userData;
    struct snd_pcm_runtime* runtime     = NULL;

    int64_t                 sample;
    int                     shift;

    uint8_t                 frameBuffer[(Z_ENGINE_MAX_CHANNELS*24)/8];
    int                     frameSize   = 0;

#if (Z_DEBUG_THROUGHPUT == 1)
    ctx->usb.dbg_frame_count++;
#endif

    
    if(!ctx->snd.pcm_running)
        return 0;

    
    if(ctx->proto.skipFrames)
    {
        ctx->proto.skipFrames--;
        return 0;
    }

    
    runtime = ctx->snd.substream->runtime;

    
    if(runtime->format == SNDRV_PCM_FORMAT_S16_LE)
    {
        frameSize = runtime->channels * 2;
        shift     = (engine->bitsPerSample == 24) ? 8 : 0;

        
        if (!ctx->gain.correctionEnabled) {

            for(i=0; i<runtime->channels; ++i)
            {
                if(i < count)   sample = ((int32_t*)data)[i];
                else            sample = 0;

                sample >>= shift;

                ((int16_t*)frameBuffer)[i] = sample;
            }
        }
        
        else {

            int64_t gain;

            for (int i=0; i<runtime->channels; ++i) {
                if(i < count)   sample = ((int32_t*)data)[i];
                else            sample = 0;

                if(i < Z_EEPROM_GAINS_NUM) gain = ctx->gain.correctionValues[i];
                else                       gain = 1L << 32;

                
                sample *= gain;
                sample >>= (shift + 32);

                
                if (sample < -32768) sample = -32768; 
                if (sample >  32767) sample =  32767; 

                ((int16_t*)frameBuffer)[i] = sample;
            }
        }

    }

    
    if (runtime->format == SNDRV_PCM_FORMAT_S24_3LE) {
        frameSize = runtime->channels * 3;
        shift     = (engine->bitsPerSample == 16) ? 8 : 0;

        
        if (!ctx->gain.correctionEnabled) {

            for (int i=0; i<runtime->channels; ++i) {
                int64_t sample = 0;
                if (i < count) sample = ((int32_t*)data)[i];

                sample <<= shift;

                frameBuffer[3*i+0] = (sample & 0x000000FF);
                frameBuffer[3*i+1] = (sample & 0x0000FF00) >> 8;
                frameBuffer[3*i+2] = (sample & 0x00FF0000) >> 16;
            }
        }
        
        else {

            for (int i = 0; i < runtime->channels; ++i) {
                int64_t sample = 0;
                if (i < count) sample = ((int32_t*)data)[i];

                int64_t gain = 1L << 32;
                if (i < Z_EEPROM_GAINS_NUM) gain = ctx->gain.correctionValues[i];

                
                sample *= gain;
                sample >>= (32 - shift);

                
                if (sample < -8388608) sample = -8388608; 
                if (sample >  8388607) sample =  8388607; 

                frameBuffer[3*i+0] = (sample & 0x000000FF);
                frameBuffer[3*i+1] = (sample & 0x0000FF00) >> 8;
                frameBuffer[3*i+2] = (sample & 0x00FF0000) >> 16;
            }
        }
    }

    
    if (frameSize && kfifo_avail(&ctx->snd.fifo) >= frameSize) {
        kfifo_in(&ctx->snd.fifo, frameBuffer, frameSize);
    }
    else {
        PRINT("Audio buffer overflow - %d samples were lost.", frameSize);
    }

    return 0;
}


int z_dev_ctrl_cb(zProtoEngine* engine, void* data, int count) {
    struct z_device_ctx* ctx = (struct z_device_ctx*)engine->userData;

    
    for(int i=0; i<count; ++i)
        z_CommandEngineReceive(ctx->ctrl.engine, ((uint32_t*)data)[i]);

    return 0;
}


int z_dev_notify_cb(zCommandEngine* a_Engine, int32_t a_Code, void* a_Data) {
    struct z_device_ctx* ctx = (struct z_device_ctx*)a_Engine->userData;

    (void)ctx; 

    switch (a_Code) {
    case Z_NOTIFY_VERSION_ROM:

#if (Z_DEBUG_COMMAND_VERBOSE > 0)
        PRINT("a_Code = Z_NOTIFY_VERSION_ROM, a_Data = '%s'", (char*)a_Data);
#endif
        return 0;

    case Z_NOTIFY_LED_BRIGHTNESS:

#if (Z_DEBUG_COMMAND_VERBOSE > 0)
        PRINT("a_Code = Z_NOTIFY_LED_BRIGHTNESS, a_Data = %d", *((int32_t*)a_Data));
#endif
        return 0;
    }

    return -1;
}



void z_dev_set_led_mode (z_device_ctx* ctx, uint8_t a_Mode) {

    
    if (a_Mode != Z_LED_CONTROL_AUTOMATIC &&
        a_Mode != Z_LED_CONTROL_MANUAL)
        return;

    
    ctx->led.controlMode = a_Mode;
    
    z_dev_update_led_color(ctx);
}

void z_dev_update_led_color (z_device_ctx* ctx) {

    
    if (ctx->led.controlMode == Z_LED_CONTROL_AUTOMATIC) {

        
        if (ctx->snd.pcm_running) {

            
            if (ctx->snd.testMode) {
                z_CmdSetLedColor(ctx->ctrl.engine, Z_COLOR_MIN, Z_COLOR_MAX, Z_COLOR_MIN);
            
            } else {
                z_CmdSetLedColor(ctx->ctrl.engine, Z_COLOR_MAX, Z_COLOR_MIN, Z_COLOR_MIN);
            }
        }

        
        else {

            
            z_CmdSetLedColor(ctx->ctrl.engine, Z_COLOR_MIN, Z_COLOR_MIN, Z_COLOR_MAX);
        }
    }

    
    if (ctx->led.controlMode == Z_LED_CONTROL_MANUAL) {

        
        z_CmdSetLedColor(ctx->ctrl.engine,
                         ctx->led.color[0],
                         ctx->led.color[1],
                         ctx->led.color[2]);
    }
}

void z_dev_set_correction_enabled(z_device_ctx* ctx, uint8_t a_Enabled) {
    ctx->gain.correctionEnabled = (bool) a_Enabled;
}

int z_read_eeprom(struct z_device_ctx* a_Ctx, uint8_t a_Buffer[Z_IOCTL_BUFFER_SIZE], uint8_t a_Addr, uint8_t a_BytesToRead) {
    const uint8_t size = min(a_BytesToRead, (uint8_t)(Z_IOCTL_BUFFER_SIZE/2)); 

    uint16_t buffer16[Z_IOCTL_BUFFER_SIZE/2];

    for (uint8_t j = 0; j < size * 2; ++j) {
        unsigned short val = 0;
        int res = ftdi_get_eeprom(a_Ctx, a_Addr+j, &val);
        if (res < 0)
            return -1;
#if (Z_DEBUG_CHARDEV == 1)
        PRINT("get addr (0x%02X) result (%d) data (0x%04X)", a_Addr+j, res, val);
#endif
        buffer16[j] = val;
    }
    shortsToBytes(buffer16, size/2, a_Buffer);
    return 0;
}

void z_dev_update_gains(z_device_ctx *ctx) {
#if Z_MASTER_CORRECTION_ENABLED
    
    uint64_t commonGain = (uint64_t)ctx->gain.correctionGlobalGain * ctx->gain.masterGain;
    commonGain >>= 16 ;
#else
    uint64_t commonGain = ctx->gain.masterGain;
#endif

    for (size_t i = 0; i < Z_EEPROM_GAINS_NUM; i++) {
        ctx->gain.correctionValues[i] = ((uint64_t)ctx->gain.correctionGains[i]) * commonGain;
    }
}
