#include <linux/version.h>
#include <linux/timer.h>

#include "mixer.h"
#include "snd.h"

#include "z_CommandEngineDefs.h"

#ifndef from_timer
#define from_timer(var, timer, timer_field) container_of(timer, typeof(*var), timer_field)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,6,0)
#define Z_TIMER_SHUTDOWN_SYNC timer_shutdown_sync
#else
#define Z_TIMER_SHUTDOWN_SYNC del_timer_sync
#endif






static struct snd_pcm_hardware z_snd_params = {

    .info = (   SNDRV_PCM_INFO_MMAP |
                SNDRV_PCM_INFO_INTERLEAVED |
                SNDRV_PCM_INFO_BLOCK_TRANSFER |
                SNDRV_PCM_INFO_MMAP_VALID),

    .formats =          SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_3LE,
    .rates =            SNDRV_PCM_RATE_8000_96000,

    .rate_min =         8000,
    .rate_max =         96000,

    .channels_min =     Z_PCM_MIN_CHANNELS,
    .channels_max =     Z_PCM_MAX_CHANNELS,

    .period_bytes_min = Z_PCM_MIN_PERIOD_BYTES,
    .period_bytes_max = Z_PCM_MAX_PERIOD_BYTES,

    .periods_min =      Z_PCM_MIN_PERIODS,
    .periods_max =      Z_PCM_MAX_PERIODS,

    .buffer_bytes_max = Z_PCM_MAX_BUFFER,
};

void getConstraintList(struct z_device_ctx* a_Ctx) {

    int sampleRatesTableSize = 0;
    z_GetAvailableSampleRates(a_Ctx->info.serialNumber, a_Ctx->sampleRatesTable, &sampleRatesTableSize);

    a_Ctx->z_pcm_rate_constraint.count = sampleRatesTableSize;
    a_Ctx->z_pcm_rate_constraint.list = a_Ctx->sampleRatesTable;
    a_Ctx->z_pcm_rate_constraint.mask = 0;
}




#ifdef Z_KERNEL_HAVE_TIMER_SETUP
static void z_snd_pcm_timer_proc(struct timer_list *t) {
    struct z_device_ctx*    ctx = from_timer(ctx, t, snd.timer);
#else
static void z_snd_pcm_timer_proc(unsigned long data) {
    struct z_device_ctx*    ctx = (struct z_device_ctx*) data;
#endif
    unsigned long           flags;

    uint32_t        time;
    int             period_elapsed = 0;

    uint32_t        max_size;
    uint32_t        ff_size;

    uint32_t        copy_size;
    uint32_t        copy_left;

    uint32_t        seg_size;
    uint32_t        seg_cpy;

    uint8_t*        buf_ptr;

    if(!ctx) {
        PRINT("ctx == NULL");
        return;
    }

    
    spin_lock_irqsave(&ctx->snd.timer_lock, flags);

    
    if(!ctx->snd.pcm_running)
    {
        spin_unlock_irqrestore(&ctx->snd.timer_lock, flags);
        return;
    }

    
    max_size    = ctx->snd.pcm_buffer_bytes - ctx->snd.pcm_sample_bytes;

    
    ff_size     = kfifo_len(&ctx->snd.fifo);





    
    copy_size   = ff_size;
    if(copy_size > max_size) {
        copy_size = max_size;
    }

    
    copy_size /= ctx->snd.pcm_sample_bytes;
    copy_size *= ctx->snd.pcm_sample_bytes;

    
    copy_left = copy_size;
    while(copy_left) {

        
        seg_size = (ctx->snd.pcm_buffer_bytes - ctx->snd.pcm_buffer_ptr);
        seg_cpy  = copy_left;

        if(seg_cpy > seg_size) {
            seg_cpy = seg_size;
        }

        
        buf_ptr  = (uint8_t*)ctx->snd.substream->runtime->dma_area;
        buf_ptr += ctx->snd.pcm_buffer_ptr;

        
        seg_cpy = kfifo_out(&ctx->snd.fifo, buf_ptr, seg_cpy);

        
        
        

        
        copy_left               -= seg_cpy;

        ctx->snd.pcm_buffer_ptr += seg_cpy;
        ctx->snd.pcm_period_ptr += seg_cpy;

        long long unsigned int tmp = ctx->snd.pcm_buffer_ptr;
        ctx->snd.pcm_buffer_ptr = do_div(tmp, ctx->snd.pcm_buffer_bytes);

        if(ctx->snd.pcm_period_ptr >= ctx->snd.pcm_period_bytes) {
            tmp = ctx->snd.pcm_period_ptr;
            ctx->snd.pcm_period_ptr = do_div(tmp, ctx->snd.pcm_period_bytes);
            period_elapsed = 1;
        }
    }

    
    if(copy_size) {

        time    = (HZ * copy_size                ) / (ctx->snd.substream->runtime->rate * ctx->snd.pcm_sample_bytes);
        time    = (time*90) / 100;
    }
    else {
        time    = (HZ * ctx->snd.pcm_period_bytes) / (ctx->snd.substream->runtime->rate * ctx->snd.pcm_sample_bytes);
        time    = (time*90) / 100;
    }

    if(time < 1) {
        time = 1;
    }

    
#if (Z_DEBUG_FIFO == 1)
    if(time > 1) {
        PRINT("fifo = %d, copy = %d, time = %dus", ff_size, copy_size, (time*1000*1000)/HZ);
    }
#endif

    mod_timer(&ctx->snd.timer, jiffies + time);

    
    if(period_elapsed) {
        spin_unlock_irqrestore(&ctx->snd.timer_lock, flags);
        snd_pcm_period_elapsed(ctx->snd.substream);
        return;
    }

    spin_unlock_irqrestore(&ctx->snd.timer_lock, flags);
}




static int z_pcm_test_runtime_params(struct z_device_ctx* ctx, struct snd_pcm_runtime* runtime)
{
    int i, rateOk = 1;

    
    if( runtime->format != SNDRV_PCM_FORMAT_S16_LE &&
        runtime->format != SNDRV_PCM_FORMAT_S24_3LE)
    {
        PRINT("Invalid sample format (%d)", runtime->format);
        return -EINVAL;
    }

    
    if( runtime->channels < z_snd_params.channels_min ||
        runtime->channels > z_snd_params.channels_max) {

        PRINT("Not supported channel count (%d)", runtime->channels);
        return -EINVAL;
    }

    getConstraintList(ctx);

    rateOk = false;
    for(i = 0; i < ctx->z_pcm_rate_constraint.count; ++i) {
        if(runtime->rate == ctx->z_pcm_rate_constraint.list[i]) {
            rateOk = true;
            break;
        }
    }

    if(!rateOk) {
        PRINT("Not supported sampling rate (%d)", runtime->rate);
        return -EINVAL;
    }

    return 0;
}




static int z_pcm_capture_open(struct snd_pcm_substream *substream)
{
    int                     res;
    struct z_device_ctx*    ctx     = substream->private_data;
    struct snd_pcm_runtime* runtime = substream->runtime;

    if(!ctx) {
        PRINT("ctx == NULL");
        return -1;
    }

    mutex_lock(&ctx->snd.lock);

    
    runtime->hw = z_snd_params;

    getConstraintList(ctx);

    
    res = snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_RATE, &ctx->z_pcm_rate_constraint);
    if(res) {
        PRINT("Error setting hardware constraints");
        return res;
    }

    
    #ifdef Z_KERNEL_HAVE_TIMER_SETUP
        timer_setup(&ctx->snd.timer, z_snd_pcm_timer_proc, 0);
    #else
        setup_timer(&ctx->snd.timer, z_snd_pcm_timer_proc, (unsigned long)ctx); 
    #endif

    
    substream->runtime->private_data    = ctx;
    ctx->snd.substream                  = substream;

    mutex_unlock(&ctx->snd.lock);

#if (Z_DEBUG_PCM == 1)
    PRINT("PCM Opened");
#endif

    return 0;
}


static int z_pcm_capture_close(struct snd_pcm_substream *substream)
{
    struct z_device_ctx* ctx = substream->private_data;

    if(!ctx) {
        PRINT("ctx == NULL");
        return -1;
    }

    
    Z_TIMER_SHUTDOWN_SYNC(&ctx->snd.timer);
    ctx->snd.timer.expires = 0;

    mutex_lock(&ctx->snd.lock);

    
    substream->private_data = NULL;

    mutex_unlock(&ctx->snd.lock);

#if (Z_DEBUG_PCM == 1)
    PRINT("PCM Closed");
#endif

    return 0;
}


static int z_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *hw_params)
{
    int res;

    
    res  = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
    if(res<0) {
#if (Z_DEBUG_PCM == 1)
        PRINT("snd_pcm_lib_malloc_pages() failed! (res=%d)", res);
#endif
        return res;
    }

    return res;
}


static int z_pcm_hw_free(struct snd_pcm_substream *substream)
{
    int res;

    
    res = snd_pcm_lib_free_pages(substream);

    return res;
}

static int z_pcm_prepare(struct snd_pcm_substream *substream) {
    int                     res;
    struct z_device_ctx*    ctx         = substream->private_data;
    struct snd_pcm_runtime* runtime     = substream->runtime;


    if(!ctx) {
        PRINT("ctx == NULL");
        return -1;
    }

#if (Z_DEBUG_PCM == 1)
    PRINT("PCM prepare:");

    if     (runtime->format == SNDRV_PCM_FORMAT_S16_LE)
        PRINT(" Format             : S16_LE");
    else if(runtime->format == SNDRV_PCM_FORMAT_S24_3LE)
        PRINT(" Format             : S24_3LE");
    else
        PRINT(" Format             : Unknown!");

    PRINT(" Channels           : %d",    runtime->channels);
    PRINT(" Sample rate        : %d Hz", runtime->rate);
#endif

    
    if(substream->stream != SNDRV_PCM_STREAM_CAPTURE) {
        PRINT("Not supported stream type");
        return -EINVAL;
    }

    
    res = z_pcm_test_runtime_params(ctx, runtime);
    if(res < 0) {
        return res;
    }

    mutex_lock(&ctx->snd.lock);

    
    if(runtime->format == SNDRV_PCM_FORMAT_S16_LE)
        ctx->snd.pcm_sample_bytes   = runtime->channels * 2;
    if(runtime->format == SNDRV_PCM_FORMAT_S24_3LE)
        ctx->snd.pcm_sample_bytes   = runtime->channels * 3;

    ctx->snd.pcm_buffer_bytes       = snd_pcm_lib_buffer_bytes(substream);
    ctx->snd.pcm_period_bytes       = snd_pcm_lib_period_bytes(substream);

#if (Z_DEBUG_PCM == 1)
    PRINT(" Sample size (frame): %d bytes",  ctx->snd.pcm_sample_bytes);
    PRINT(" Period frames      : %d",        ctx->snd.pcm_period_bytes / ctx->snd.pcm_sample_bytes);
    PRINT(" Period size        : %d bytes",  ctx->snd.pcm_period_bytes);
    PRINT(" Period count       : %d",        ctx->snd.pcm_buffer_bytes / ctx->snd.pcm_period_bytes);
    PRINT(" Buffer size        : %d",        ctx->snd.pcm_buffer_bytes);
#endif

    
    if(!ctx->snd.pcm_running) {
        ctx->snd.pcm_buffer_ptr     = 0;
        ctx->snd.pcm_period_ptr     = 0;
    }

    mutex_unlock(&ctx->snd.lock);

    
    res = z_CmdSetSampleRate(ctx->ctrl.engine, runtime->rate);
    if(res) {
        PRINT("Error setting sample rate!\n");
        return -1;
    }

    int channels = 0;

    
    switch (runtime->format) {
        case SNDRV_PCM_FORMAT_S16_LE: {
            res = z_CmdSetSampleFormat(ctx->ctrl.engine, 0);
            
            channels = (runtime->channels + 1) & ~0x1;
            break;
        }
        case SNDRV_PCM_FORMAT_S24_3LE: {
            res = z_CmdSetSampleFormat(ctx->ctrl.engine, 1);
            channels = (runtime->channels + 3) & ~0x3;
            break;
        }
    }

    if(res) {
        PRINT("Error setting sample format!\n");
        return -1;
    }

    
    res = z_CmdSetChannelCount(ctx->ctrl.engine, channels);
    if(res) {
        PRINT("Error setting channel count!\n");
        return -1;
    }

    
    
    
    ctx->proto.engine->channelCount     = 32;
    ctx->proto.engine->bitsPerSample    = 16;

    
    ctx->proto.skipFrames = runtime->rate / Z_Defines.Z_F6798FE13CF899722B23;

    return 0;
}


static int z_pcm_trigger_stop(struct snd_pcm_substream* substream) {
    struct z_device_ctx* const  ctx = substream->private_data;
    unsigned long flags;

    if(!ctx) {
        PRINT("ctx == NULL");
        return -1;
    }


#if (Z_DEBUG_PCM == 1)
        if(in_interrupt()) {
            PRINT("PCM trigger stop (ISR)");
        }
        else {
            PRINT("PCM trigger stop");
        }
#endif

        
        if (!ctx->usb.isDisconnected) {
            z_CmdEnableStreaming(ctx->ctrl.engine, 0);
        }

        
        if(!in_interrupt()) {
            Z_TIMER_SHUTDOWN_SYNC(&ctx->snd.timer);
            ctx->snd.timer.expires = 0;
        }

        
        spin_lock_irqsave(&ctx->snd.timer_lock, flags);

        ctx->snd.pcm_running    = 0;

        spin_unlock_irqrestore(&ctx->snd.timer_lock, flags);

        
        if (!ctx->usb.isDisconnected) {
            z_dev_update_led_color(ctx);
        }

    return 0;
}



static int z_pcm_trigger_start(struct snd_pcm_substream* substream) {
    struct z_device_ctx* const ctx = substream->private_data;
    unsigned long flags;

    if(!ctx) {
        PRINT("ctx == NULL");
        return -1;
    }
    
    const int res = z_pcm_test_runtime_params(ctx, substream->runtime);
    if(res < 0) return res;

#if (Z_DEBUG_PCM == 1)
    if(in_interrupt()) {
        PRINT("PCM trigger start (ISR)");
    }
    else {
        PRINT("PCM trigger start");
    }
#endif

    
    z_CmdEnableStreaming(ctx->ctrl.engine, 1);

    
    kfifo_reset_out(&ctx->snd.fifo);

    
    ctx->snd.pcm_buffer_ptr = 0;
    ctx->snd.pcm_period_ptr = 0;

    
    spin_lock_irqsave(&ctx->snd.timer_lock, flags);

    ctx->snd.pcm_running    = 1;
    mod_timer(&ctx->snd.timer, jiffies + HZ/100);

    spin_unlock_irqrestore(&ctx->snd.timer_lock, flags);

    
    z_dev_update_led_color(ctx);

    return 0;
}


static int z_pcm_trigger(struct snd_pcm_substream *substream, int cmd) {

    switch (cmd) {
        case SNDRV_PCM_TRIGGER_START:
            return z_pcm_trigger_start(substream);

        case SNDRV_PCM_TRIGGER_STOP:
            return z_pcm_trigger_stop(substream);

        default:
    #if (Z_DEBUG_PCM == 1)
            PRINT("Unsupported command %d", cmd);
    #endif
            return -EINVAL;
    }

    return -EINVAL;
}


static snd_pcm_uframes_t z_pcm_pointer(struct snd_pcm_substream *substream)
{
    struct z_device_ctx*    ctx = substream->private_data;
    unsigned long           flags;
    uint32_t                pos;

    if(!ctx) {
        PRINT("ctx == NULL");
        return 0;
    }

    spin_lock_irqsave(&ctx->snd.timer_lock, flags);
    pos = ctx->snd.pcm_buffer_ptr;
    spin_unlock_irqrestore(&ctx->snd.timer_lock, flags);

    return bytes_to_frames(substream->runtime, pos);
}




static struct snd_pcm_ops z_pcm_ops = {
    .open =        z_pcm_capture_open,
    .close =       z_pcm_capture_close,
    .ioctl =       snd_pcm_lib_ioctl,
    .hw_params =   z_pcm_hw_params,
    .hw_free =     z_pcm_hw_free,
    .prepare =     z_pcm_prepare,
    .trigger =     z_pcm_trigger,
    .pointer =     z_pcm_pointer,
};


static int z_snd_dev_free(struct snd_device *device)
{
	return 0;
}


static struct snd_device_ops z_snd_dev_ops =
{
	.dev_free = z_snd_dev_free,
};




static int z_snd_pcm_register(struct z_device_ctx* ctx)
{
    int             res;
    struct snd_pcm* pcm     = NULL;

    
    res = snd_pcm_new(ctx->snd.card, Z_PCM_NAME, 0, 0,1, &pcm);
    if(res) {
#if (Z_DEBUG_PCM == 1)
        PRINT("snd_pcm_new() failed! (res=%d)", res);
#endif
        return -1;
    }

    
    ctx->snd.pcm        = pcm;
    pcm->private_data   = ctx;

    
    snd_pcm_set_ops(ctx->snd.pcm, SNDRV_PCM_STREAM_CAPTURE, &z_pcm_ops);

    
    ctx->snd.pcm->info_flags = 0;
    strcpy(ctx->snd.pcm->name, Z_PCM_NAME);

    

    
    
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,1,0)
    snd_pcm_lib_preallocate_pages_for_all(ctx->snd.pcm, SNDRV_DMA_TYPE_CONTINUOUS,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
										snd_dma_continuous_data(GFP_KERNEL),
#else
										NULL,
#endif
                                        Z_PCM_MAX_BUFFER, Z_PCM_MAX_BUFFER);
#else
    res = snd_pcm_lib_preallocate_pages_for_all(ctx->snd.pcm, SNDRV_DMA_TYPE_CONTINUOUS,
                                                   snd_dma_continuous_data(GFP_KERNEL),
                                                   Z_PCM_MAX_BUFFER, Z_PCM_MAX_BUFFER);
    if(res) {
#if (Z_DEBUG_PCM == 1)
        PRINT("snd_pcm_lib_preallocate_pages_for_all() failed! (res=%d)", res);
#endif
        return -1;
    }
#endif

    return 0;
}




int z_snd_register(struct z_device_ctx* ctx)
{
    int                     res;

    struct snd_card*        card = NULL;

    char                    cardShortName[64];
    char                    cardLongName [64];

    
#if Z_CARD_NAME_ADD_SN
    sprintf(cardLongName, "%s %s %s", Z_CARD_NAME_PREFIX, ctx->info.cardFriendlyName, ctx->info.serialNumber);
    sprintf(cardShortName, "%s %s", ctx->info.cardFriendlyName, ctx->info.serialNumber);
#elif Z_CARD_NAME_ADD_SUFFIX

    if(ctx->index == 0) {
        sprintf(cardLongName, Z_CARD_NAME_PREFIX" %s", ctx->info.cardFriendlyName);
        strcpy (cardShortName, ctx->info.cardFriendlyName);
    }
    else {
        sprintf(cardLongName, Z_CARD_NAME_PREFIX" %s (%d)", ctx->info.cardFriendlyName, ctx->index + 1);
        sprintf(cardShortName, "%s (%d)", ctx->info.cardFriendlyName, ctx->index + 1);
    }

#endif

    
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 16, 0)
    res = snd_card_create(SNDRV_DEFAULT_IDX1, ctx->info.cardFriendlyName, THIS_MODULE, 0, &card);
#else
    res = snd_card_new(&ctx->usb.dev->dev, SNDRV_DEFAULT_IDX1, ctx->info.cardFriendlyName, THIS_MODULE, 0, &card);
#endif

    if(res != 0 || !card) {
        PRINT("snd_card_create() failed! (res=%d)", res);
        return -1;
    }

    
    card->private_data  = ctx;
    ctx->snd.card       = card;

    
    snd_card_set_dev(ctx->snd.card, &ctx->usb.dev->dev);

    
    strcpy (ctx->snd.card->driver,    Z_DRIVER_NAME);
    strcpy (ctx->snd.card->shortname, cardLongName);    
    strcpy (ctx->snd.card->longname,  cardShortName);   

    
    mutex_init(&ctx->snd.lock);

    
    spin_lock_init(&ctx->snd.timer_lock);

    

    
    res = snd_device_new(card, SNDRV_DEV_LOWLEVEL, ctx, &z_snd_dev_ops);
    if(res) {
        PRINT("snd_device_new() failed! (res=%d)", res);
        return -1;
    }

    

    
    res = z_snd_pcm_register(ctx);
    if(res) {
        PRINT("Error registering PCM interface!");
        return -1;
    }

    
    res = z_snd_mixer_initialize(ctx);
    if(res) {
        PRINT("Error registering ALSA mixer interface!");
        return -1;
    }

    

    
    res = snd_card_register(ctx->snd.card);
    if(res) {
        PRINT("snd_card_register() failed! (res=%d)", res);
        return -1;
    }

#if (Z_DEBUG_INIT == 1)
        PRINT("Sound card created");
#endif

    return 0;
}


int z_snd_unregister(struct z_device_ctx* ctx)
{
    if(!ctx) {
        PRINT("ctx == NULL");
        return -1;
    }

    
    if(ctx->snd.card) {

        snd_card_free(ctx->snd.card);

        ctx->snd.card = NULL;
        ctx->snd.pcm  = NULL;

#if (Z_DEBUG_INIT == 1)
        PRINT("Sound card removed");
#endif
    }

    return 0;
}
