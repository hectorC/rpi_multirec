#include <linux/version.h>
#include <linux/timer.h>
#include "device.h"
#include "usb.h"
#include "ftdi.h"
#include "snd.h"
#include "chardev.h"

#include "z_Utils.h"

#include "../../common/ZProtocolEngine/z_Correction.h"

#ifndef from_timer
#define from_timer(var, timer, timer_field) container_of(timer, typeof(*var), timer_field)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,6,0)
#define Z_TIMER_SHUTDOWN_SYNC timer_shutdown_sync
#else
#define Z_TIMER_SHUTDOWN_SYNC del_timer_sync
#endif

static int  z_usb_probe(struct usb_interface *a_interface, const struct usb_device_id *a_id);
static void z_usb_disconnect(struct usb_interface *a_interface);


static struct usb_device_id z_usb_table [] = {
    { USB_DEVICE(FTDI_VID, ZYLIA_PID) },
    { }
};


static struct usb_driver z_usb_driver = {
    .name       = Z_DRIVER_NAME,
    .id_table   = z_usb_table,
    .probe      = z_usb_probe,
    .disconnect = z_usb_disconnect,
};


MODULE_DEVICE_TABLE(usb, z_usb_table);




static void z_usb_in_urb_cb(struct urb *urb)   
{
    struct z_urb_ctx*       urb_ctx;
    struct z_device_ctx*    ctx;
    unsigned long           flags;

    size_t      dataLeft  = 0;
    uint8_t*    dataPtr   = NULL;
    size_t      chunkSize = 0;

    
    urb_ctx = (struct z_urb_ctx*)urb->context;
    if(!urb_ctx) {
#if (Z_DEBUG_URB == 1)
        PRINT("urb_ctx == NULL");
#endif
        return;
    }
    ctx = (struct z_device_ctx*)urb_ctx->ctx;
    if(!urb_ctx) {
        PRINT("ctx == NULL");
        return;
    }

    
    spin_lock_irqsave(&ctx->usb.lock, flags);

    
    urb_ctx->status = 0;

    
    if(urb->status != 0 && urb->status != -ESHUTDOWN) {
#if (Z_DEBUG_URB == 1)
        PRINT("URB %p status=%d", urb, urb->status);
#endif
        spin_unlock_irqrestore(&ctx->usb.lock, flags);
        return;
    }

#if (Z_DEBUG_THROUGHPUT == 1)
    ctx->usb.dbg_in_urb_count++;
    ctx->usb.dbg_raw_count += urb->actual_length;
#endif

    
    dataLeft = urb->actual_length;
    dataPtr  = (uint8_t*)urb_ctx->buf;

    while (dataLeft > 0) {

        
        chunkSize = dataLeft;
        if (chunkSize > 512) {  
            chunkSize = 512;
        }

        
        if (chunkSize > 2) {
            z_ProtoDecode(ctx->proto.engine, &dataPtr[2], chunkSize - 2);
        }

        
        dataLeft -= chunkSize;
        dataPtr  += chunkSize;
    }

    
    spin_unlock_irqrestore(&ctx->usb.lock, flags);
}


static void z_usb_out_urb_cb(struct urb *urb)   
{
    struct z_urb_ctx*       urb_ctx;
    struct z_device_ctx*    ctx;
    unsigned long           flags;

    
    urb_ctx = (struct z_urb_ctx*)urb->context;
    if(!urb_ctx) {
#if (Z_DEBUG_URB == 1)
        PRINT("urb_ctx == NULL");
#endif
        return;
    }
    ctx = (struct z_device_ctx*)urb_ctx->ctx;
    if(!urb_ctx) {
        PRINT("ctx == NULL");
        return;
    }

    
    spin_lock_irqsave(&ctx->usb.lock, flags);

    
    urb_ctx->status = 0;

    
    if(urb->status != 0 && urb->status != -ESHUTDOWN) {
#if (Z_DEBUG_URB == 1)
        PRINT("URB %p status=%d", urb, urb->status);
#endif
        spin_unlock_irqrestore(&ctx->usb.lock, flags);
        return;
    }

#if (Z_DEBUG_THROUGHPUT == 1)
    ctx->usb.dbg_out_urb_count++;
#endif

    
    spin_unlock_irqrestore(&ctx->usb.lock, flags);
}




#ifdef Z_KERNEL_HAVE_TIMER_SETUP
static void z_usb_timer_proc(struct timer_list *t) {   
    struct z_device_ctx* ctx = from_timer(ctx, t, usb.timer);
#else
static void z_usb_timer_proc(unsigned long data) {
    struct z_device_ctx* ctx = (struct z_device_ctx*) data;
#endif

    int             res;

    uint32_t        len;
    unsigned long   flags;

    uint32_t        ctrlWord;



    if(!ctx) {
        PRINT("ctx == NULL");
        return;
    }

    
    spin_lock_irqsave(&ctx->usb.lock, flags);

    
    mod_timer(&ctx->usb.timer, jiffies + HZ / Z_Defines.Z_067F7118AAD81EF6BD0C);

    
    for(int i=0; i<Z_MAX_URB_IN_LIST; ++i) {

        
        if(ctx->usb.urbs_in[i].status != 0) {
            continue;
        }

        
        usb_fill_bulk_urb(
            ctx->usb.urbs_in[i].urb,
            ctx->usb.dev,
            usb_rcvbulkpipe(ctx->usb.dev, ctx->usb.ep_in),
            ctx->usb.urbs_in[i].buf,
            Z_Defines.Z_025538939104A5F99E72,
            z_usb_in_urb_cb,
            (void*)&ctx->usb.urbs_in[i]);

        ctx->usb.urbs_in[i].urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

        
        res = usb_submit_urb(ctx->usb.urbs_in[i].urb, GFP_ATOMIC);
        if(res) {
#if (Z_DEBUG_URB == 1)
            PRINT("usb_submit_urb() failed (res=%d)", res);
#endif
            continue;
        }

        
        ctx->usb.urbs_in[i].status = 1;
    }

    for(int i=0; i<Z_MAX_URB_OUT_LIST; ++i) {

        
        if(ctx->usb.urbs_out[i].status != 0) {
            continue;
        }

        
        res = z_CommandEngineProcess(ctx->ctrl.engine, &ctrlWord);
        if(res) continue;

        
        res = z_ProtoEncodeCtrl(ctx->proto.engine, ctrlWord, ctx->usb.urbs_out[i].buf, &len);
        if(res) continue;

        
        usb_fill_bulk_urb(
            ctx->usb.urbs_out[i].urb,
            ctx->usb.dev,
            usb_sndbulkpipe(ctx->usb.dev, ctx->usb.ep_out),
            ctx->usb.urbs_out[i].buf,
            len,
            z_usb_out_urb_cb,
            (void*)&ctx->usb.urbs_out[i]);

        ctx->usb.urbs_out[i].urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

        
        res = usb_submit_urb(ctx->usb.urbs_out[i].urb, GFP_ATOMIC);
        if(res) {
#if (Z_DEBUG_URB == 1)
            PRINT("usb_submit_urb() failed (res=%d)", res);
#endif
            continue;
        }

        
        ctx->usb.urbs_out[i].status = 1;
    }

    
    spin_unlock_irqrestore(&ctx->usb.lock, flags);
}



#if (Z_DEBUG_THROUGHPUT == 1)


#ifdef Z_KERNEL_HAVE_TIMER_SETUP
static void z_dbg_timer_proc(struct timer_list *t) {
    struct z_device_ctx* ctx = from_timer(ctx, t, usb.dbg_timer);
#else
static void z_dbg_timer_proc(unsigned long data) {  
    struct z_device_ctx* ctx = (struct z_device_ctx*)data;
#endif
    unsigned long   flags;

    uint32_t    time_diff;
    uint32_t    raw_count;
    uint32_t    dat_count;
    uint32_t    in_urb_count;
    uint32_t    out_urb_count;
    uint32_t    frame_count;

    uint32_t    raw_rate;
    uint32_t    dat_rate;
    uint32_t    in_urb_rate;
    uint32_t    out_urb_rate;
    uint32_t    frame_rate;



    if(!ctx) {
        PRINT("ctx == NULL");
        return;
    }

    
    spin_lock_irqsave(&ctx->usb.lock, flags);

    
    time_diff     = jiffies - ctx->usb.dbg_jiffies;
    raw_count     = ctx->usb.dbg_raw_count;
    dat_count     = ctx->usb.dbg_dat_count;
    in_urb_count  = ctx->usb.dbg_in_urb_count;
    out_urb_count = ctx->usb.dbg_out_urb_count;
    frame_count   = ctx->usb.dbg_frame_count;

    
    raw_rate      = raw_count     / Z_1473D97CC6C2D067367F;
    dat_rate      = dat_count     / Z_1473D97CC6C2D067367F;
    in_urb_rate   = in_urb_count  / Z_1473D97CC6C2D067367F;
    out_urb_rate  = out_urb_count / Z_1473D97CC6C2D067367F;
    frame_rate    = frame_count   / Z_1473D97CC6C2D067367F;

    PRINT("OUT URB: %d (%d Hz). IN URB: %d (%d Hz)", out_urb_count, out_urb_rate, in_urb_count, in_urb_rate);
    PRINT("RAW: %d kB/s, DAT: %d kB/s", raw_rate/1024, dat_rate/1024);
    PRINT("Frame: %d fr/s", frame_rate);

    
    ctx->usb.dbg_jiffies        = jiffies;
    ctx->usb.dbg_raw_count      = 0;
    ctx->usb.dbg_dat_count      = 0;
    ctx->usb.dbg_in_urb_count   = 0;
    ctx->usb.dbg_out_urb_count  = 0;
    ctx->usb.dbg_frame_count    = 0;
    mod_timer(&ctx->usb.dbg_timer, jiffies + HZ * Z_1473D97CC6C2D067367F);

    
    spin_unlock_irqrestore(&ctx->usb.lock, flags);
}

#endif




static int z_usb_alloc_urbs(struct z_device_ctx* ctx)
{
    int         i,res = 0;
    uint8_t*    buf;
    struct urb* urb;

    
    for(i=0; i<Z_MAX_URB_IN_LIST; ++i) {

        
        urb = usb_alloc_urb(0, GFP_KERNEL);
        if(!urb) {
#if (Z_DEBUG_URB == 1)
            PRINT("URB allocation failed!");
#endif
            continue;
        }

        
        buf = usb_alloc_coherent(ctx->usb.dev, Z_Defines.Z_025538939104A5F99E72, GFP_KERNEL, &urb->transfer_dma);
        if(!buf) {
#if (Z_DEBUG_URB == 1)
            PRINT("URB buffer allocation failed!");
#endif
            usb_free_urb(urb);
            urb = NULL;
            res = -1;
        }

        
        ctx->usb.urbs_in[i].ctx    = ctx;
        ctx->usb.urbs_in[i].urb    = urb;
        ctx->usb.urbs_in[i].buf    = buf;
        ctx->usb.urbs_in[i].status = 0;
    }

    
    for(i=0; i<Z_MAX_URB_OUT_LIST; ++i) {

        
        urb = usb_alloc_urb(0, GFP_KERNEL);
        if(!urb) {
#if (Z_DEBUG_URB == 1)
            PRINT("URB allocation failed!");
#endif
            continue;
        }

        
        buf = usb_alloc_coherent(ctx->usb.dev, ctx->usb.ep_out_size, GFP_KERNEL, &urb->transfer_dma);
        if(!buf) {
#if (Z_DEBUG_URB == 1)
            PRINT("URB buffer allocation failed!");
#endif
            usb_free_urb(urb);
            urb = NULL;
            res = -1;
        }

        
        ctx->usb.urbs_out[i].ctx    = ctx;
        ctx->usb.urbs_out[i].urb    = urb;
        ctx->usb.urbs_out[i].buf    = buf;
        ctx->usb.urbs_out[i].status = 0;
    }

    return res;
}


static int z_usb_free_urbs(struct z_device_ctx* ctx)
{
    int         i;

    
    for(i=0; i<Z_MAX_URB_IN_LIST; ++i) {

        if(ctx->usb.urbs_in[i].urb)
        {
            
            usb_kill_urb(ctx->usb.urbs_in[i].urb);

            
            usb_free_coherent(
                ctx->usb.urbs_in[i].urb->dev,
                ctx->usb.urbs_in[i].urb->transfer_buffer_length,
                ctx->usb.urbs_in[i].urb->transfer_buffer,
                ctx->usb.urbs_in[i].urb->transfer_dma);

            
            usb_free_urb(ctx->usb.urbs_in[i].urb);
        }

        ctx->usb.urbs_in[i].ctx    = NULL;
        ctx->usb.urbs_in[i].urb    = NULL;
        ctx->usb.urbs_in[i].buf    = NULL;
        ctx->usb.urbs_in[i].status = 0;
    }

    
    for(i=0; i<Z_MAX_URB_OUT_LIST; ++i) {

        if(ctx->usb.urbs_out[i].urb)
        {
            
            usb_kill_urb(ctx->usb.urbs_out[i].urb);

            
            usb_free_coherent(
                ctx->usb.urbs_out[i].urb->dev,
                ctx->usb.urbs_out[i].urb->transfer_buffer_length,
                ctx->usb.urbs_out[i].urb->transfer_buffer,
                ctx->usb.urbs_out[i].urb->transfer_dma);

            
            usb_free_urb(ctx->usb.urbs_out[i].urb);
        }

        ctx->usb.urbs_out[i].ctx    = NULL;
        ctx->usb.urbs_out[i].urb    = NULL;
        ctx->usb.urbs_out[i].buf    = NULL;
        ctx->usb.urbs_out[i].status = 0;
    }

    return 0;
}




static int z_parse_eeprom_gains_v1 (const uint8_t* a_EepromData) {

    const int v2DataSize = Z_EEPROM_GAINS_NUM + 5; 

    uint16_t* checksumData = kcalloc(v2DataSize - 2, sizeof(uint16_t), 0);

    uint8_t versionRead = a_EepromData[v2DataSize - 1]; 

#if (Z_DEBUG_CORRECTIONS > 1)
    PRINT("Corr: Checking for data v1...");
#endif

    for(unsigned int i = 0; i < Z_EEPROM_GAINS_NUM + 1; i++) {
        checksumData[i] = (uint16_t)a_EepromData[i+4];
    }

    uint16_t checksum = calcFTDIChecksum(checksumData, Z_EEPROM_GAINS_NUM + 1);
    uint16_t checksumRead = ((0x00FF & a_EepromData[2]) << 8) | a_EepromData[3];

    kfree(checksumData);

#if (Z_DEBUG_CORRECTIONS > 1)
    PRINT("Corr: CRC calc = 0x%04X", checksum);
    PRINT("Corr: CRC read = 0x%04X", checksumRead);
#endif

    
    if (checksum == checksumRead && versionRead == 1) {
        return 1;  
    }

    return 0;
}


static int z_parse_eeprom_gains_v2 (const uint8_t* a_EepromData) {

    const int v2DataSize = Z_EEPROM_GAINS_NUM + 5;

    uint16_t* checksumData = kcalloc(v2DataSize - 2, sizeof(uint16_t), 0);

    uint8_t  versionRead = a_EepromData[v2DataSize - 1];  

#if (Z_DEBUG_CORRECTIONS > 1)
    PRINT("Corr: Checking for data v2...");
#endif

    for(unsigned int i = 0; i < (Z_EEPROM_GAINS_NUM + 3); i++) {
        checksumData[i] = (uint16_t)a_EepromData[i+2];
    }

    uint16_t checksum = calcFTDIChecksum(checksumData, Z_EEPROM_GAINS_NUM + 3);
    uint16_t checksumRead = ((0x00FF & a_EepromData[0]) << 8) | a_EepromData[1];

    kfree(checksumData);

#if (Z_DEBUG_CORRECTIONS > 1)
    PRINT("Corr: CRC calc = 0x%04X", checksum);
    PRINT("Corr: CRC read = 0x%04X", checksumRead);
#endif

    
    if (checksum == checksumRead && versionRead == 2) {
        return versionRead;
    }

    return 0;
}


static void z_read_eeprom_gains(struct z_device_ctx* ctx) {

    
    uint8_t eepromData[Z_EEPROM_GAINS_NUM + 5];
    z_read_eeprom(ctx, eepromData, Z_EEPROM_GAINS_ADDR, Z_EEPROM_GAINS_NUM + 5);

    
    int corrVersion = z_parse_eeprom_gains_v2(eepromData);
    if (corrVersion == 0) {
        
        corrVersion = z_parse_eeprom_gains_v1(eepromData);
    }

#if (Z_DEBUG_CORRECTIONS > 0)
    PRINT("Corr: Version = %d", corrVersion);
#endif

    
    if (corrVersion) {

        
        for(size_t i = 0; i < Z_EEPROM_GAINS_NUM; i++){
            int index = g_CorrectionsZeroIndex + (signed char)(eepromData[i + 4]);
            index = max(0, min(g_CorrectionsFloatSize-1, index));
            ctx->gain.correctionGains[i] = g_CorrectionsInt[index];

#if (Z_DEBUG_CORRECTIONS > 0)
            PRINT("Corr: [%02lu] = %d(%d), %d", i+1,
                index,
                index - g_CorrectionsZeroIndex,
                ctx->gain.correctionGains[i]
                );
#endif
        }

        
        if (corrVersion == 2) {
            int index = g_CorrectionsZeroIndex + (signed char)(eepromData[2]);
            index = max(0, min(g_CorrectionsFloatSize-1, index));
            ctx->gain.correctionGlobalGain = g_CorrectionsInt[index];

#if (Z_DEBUG_CORRECTIONS > 0)
            PRINT("Corr: Master = %d(%d), %d",
                index,
                index - g_CorrectionsZeroIndex,
                ctx->gain.correctionGlobalGain
                );
#endif
        }
    }
}




static int z_identify_device(struct usb_device* dev, const struct usb_device_id *a_id, z_device_info* info) {
    char deviceName[100];
    char deviceSerialNumber[100];
    char* deviceIdStr = NULL;

    
    int res = usb_string(dev, dev->descriptor.iProduct, deviceName, sizeof(deviceName)-1);
    if(res < 0) {
        PRINT("Error obtaining device name");
        return -1;
    }

    
    res = usb_string(dev, dev->descriptor.iSerialNumber, deviceSerialNumber, sizeof(deviceSerialNumber)-1);
    if(res < 0) {
        PRINT("Error obtaining device name");
        return -1;
    }

    
    strcpy(info->deviceFriendlyName, deviceName);
    strcpy(info->cardFriendlyName, deviceName);
    strcpy(info->serialNumber, deviceSerialNumber);

    
    for(int i = strlen(deviceName)-1; i>=0; --i) {
        if(deviceName[i] == ' ') break;
        deviceIdStr = &deviceName[i];
    }

    
    strcpy(info->cardFriendlyName, deviceIdStr);

    PRINT("deviceId = '%s'", deviceIdStr);

    
    if(a_id->idProduct == ZYLIA_PID) {
        PRINT("Found device '%s'", deviceName);
        return 0;
    }

    PRINT("Device '%s' is not the mic array, skipping", deviceName);
    return -1;
}




static int z_usb_probe(struct usb_interface *a_interface, const struct usb_device_id *a_id) {
    int                     i, res;

    struct usb_device*      dev;

    struct z_device_info    info;
    struct z_device_ctx*    ctx;

    
    if (a_id->idVendor  != FTDI_VID) {
        PRINT("Wrong USB device idVendor (0x%04X)", a_id->idVendor);
        return -1;
    }
    if (a_id->idProduct != ZYLIA_PID) {
        PRINT("Wrong USB device idProduct (0x%04X)", a_id->idProduct);
        return -1;
    }

    
    dev = interface_to_usbdev(a_interface);


    

    
    res = z_identify_device(dev, a_id, &info);

    if(res != 0) {
        return -1;
    }

    
    ctx = kzalloc(sizeof(struct z_device_ctx), GFP_KERNEL);
    if(!ctx) {
        PRINT("Out of memory");
        return -1;
    }

    
    ctx->info           = info;
    ctx->usb.dev        = dev;
    ctx->usb.interface  = a_interface;


    
    if(a_interface->cur_altsetting->desc.bInterfaceNumber != 0) {

        res = push_context(ctx, 1);
        kfree(ctx);
        if(res == -1) {
            return -1;
        }

        PRINT("Registering dummy device for FTDI interface %d", a_interface->cur_altsetting->desc.bInterfaceNumber);
        usb_set_intfdata(a_interface, NULL);
        return 0;
    }

    res = push_context(ctx, 0);

    if(res == -1) {
        kfree(ctx);
        return -1;
    }

#if (Z_DEBUG_INIT == 1)
        PRINT("New device index %d", res);
#endif

    spin_lock_init(&ctx->usb.lock);

    

    
    for(i=0; i<ctx->usb.interface->cur_altsetting->desc.bNumEndpoints; ++i) {

        
        if( usb_endpoint_xfer_bulk(&ctx->usb.interface->cur_altsetting->endpoint[i].desc) &&
            usb_endpoint_dir_in   (&ctx->usb.interface->cur_altsetting->endpoint[i].desc))
        {
            if(ctx->usb.ep_in  == 0) {
                ctx->usb.ep_in       = ctx->usb.interface->cur_altsetting->endpoint[i].desc.bEndpointAddress;
                ctx->usb.ep_in_size  = ctx->usb.interface->cur_altsetting->endpoint[i].desc.wMaxPacketSize;
                PRINT("Found Bulk-In endpoint  (0x%02X %d)", ctx->usb.ep_in, ctx->usb.ep_in_size);
            }
        }

        
        if( usb_endpoint_xfer_bulk(&ctx->usb.interface->cur_altsetting->endpoint[i].desc) &&
            usb_endpoint_dir_out  (&ctx->usb.interface->cur_altsetting->endpoint[i].desc))
        {
            if(ctx->usb.ep_out == 0) {
                ctx->usb.ep_out      = ctx->usb.interface->cur_altsetting->endpoint[i].desc.bEndpointAddress;
                ctx->usb.ep_out_size = ctx->usb.interface->cur_altsetting->endpoint[i].desc.wMaxPacketSize;
                PRINT("Found Bulk-Out endpoint (0x%02X %d)", ctx->usb.ep_out, ctx->usb.ep_out_size);
            }
        }
    }

    
    if(!ctx->usb.ep_in || !ctx->usb.ep_out) {
        PRINT("Could not find USB endpoints!");
        kfree(ctx);
        return -1;
    }

    

    
    res = z_usb_alloc_urbs(ctx);
    if(res) {
#if (Z_DEBUG_URB == 1)
        PRINT("URB allocation failed!");
#endif
        z_usb_free_urbs(ctx);
        kfree(ctx);
        return -1;
    }

    
    usb_set_intfdata(a_interface, ctx);

#if (Z_DEBUG_INIT == 1)
        PRINT("USB device registered (ctx=%p)", ctx);
#endif

    

    
    ctx->led.controlMode        = Z_LED_CONTROL_AUTOMATIC;
    ctx->led.color[0]           = 0x00;
    ctx->led.color[1]           = 0x00;
    ctx->led.color[2]           = 0xFF;

    ctx->snd.testMode = 0;

    ctx->usb.isDisconnected = false;

    

    ctx->gain.correctionEnabled = true;

    
    for(unsigned int i = 0; i < Z_EEPROM_GAINS_NUM; i++){
        int32_t gain = g_CorrectionsInt[g_CorrectionsZeroIndex + 0];
        ctx->gain.correctionGains[i]  = gain;
        ctx->gain.correctionValues[i] = gain << 16;
    }
    ctx->gain.correctionGlobalGain = g_CorrectionsInt[g_CorrectionsZeroIndex + 0];
    ctx->gain.masterGain = g_CorrectionsInt[g_CorrectionsZeroIndex + 0];

    
    z_read_eeprom_gains(ctx);

    z_dev_update_gains(ctx);

    
    ctx->proto.engine = z_ProtoCreate();
    if(!ctx->proto.engine) {
        PRINT("Error creating protocol engine!");
        return -1;
    }
    z_ProtoInit(ctx->proto.engine, NULL, (void*)ctx);

    ctx->proto.engine->verboseLevel  = Z_DEBUG_PROTO_VERBOSE;

    
    z_ProtoSetDataCallback(ctx->proto.engine, z_dev_data_cb);
    z_ProtoSetCtrlCallback(ctx->proto.engine, z_dev_ctrl_cb);

    

    
    ctx->ctrl.engine = z_CommandEngineCreate();
    if(!ctx->ctrl.engine) {
        PRINT("Error creating command engine!");
        return -1;
    }
    z_CommandEngineInit(ctx->ctrl.engine, NULL, (void*)ctx);

    ctx->ctrl.engine->verboseLevel  = Z_DEBUG_COMMAND_VERBOSE;

    
    z_CommandEngineSetNotificationCallback(ctx->ctrl.engine, z_dev_notify_cb);

    
    z_CommandEnginePush(ctx->ctrl.engine, z_CmdNewRegRd(0x00));

    
    z_CmdReadVersionROM(ctx->ctrl.engine);

    
    z_CmdReadRegisters(ctx->ctrl.engine);

    
    z_dev_update_led_color(ctx);

    
    z_CmdSetDigitalGain(ctx->ctrl.engine, 0);

    
    kfifo_alloc(&ctx->snd.fifo, Z_Defines.Z_63E875E7FC1AC5FDBFD4, GFP_KERNEL);

    
    res = z_snd_register(ctx);
    if(res) {
        PRINT("Error registering sound card!");
        return -1;
    }

    

    
    res = z_chr_register(ctx);
    if(res) {
        PRINT("Error registering character device!");
        return -1;
    }

    

    
    res = ftdi_set_bit_mode(ctx, FTDI_BITMODE_SYNC_FF, 0xFF);
    if(res) {
        PRINT("Failed to set FTDI bit mode!");
    }

    
    res = ftdi_set_latency_timer(ctx, 2);
    if(res) {
        PRINT("Failed to set FTDI latency timer!");
    }

    
#ifdef Z_KERNEL_HAVE_TIMER_SETUP
    timer_setup(&ctx->usb.timer, z_usb_timer_proc, 0);
#else
    setup_timer(&ctx->usb.timer, z_usb_timer_proc, (unsigned long)ctx);
#endif
    mod_timer(&ctx->usb.timer, jiffies + 1);

#if (Z_DEBUG_THROUGHPUT == 1)
    ctx->usb.dbg_jiffies        = jiffies;
    ctx->usb.dbg_raw_count      = 0;
    ctx->usb.dbg_dat_count      = 0;
    ctx->usb.dbg_in_urb_count   = 0;
    ctx->usb.dbg_out_urb_count  = 0;
#ifdef Z_KERNEL_HAVE_TIMER_SETUP
    timer_setup(&ctx->usb.dbg_timer, z_dbg_timer_proc, 0);
#else
    setup_timer(&ctx->usb.dbg_timer, z_dbg_timer_proc, (unsigned long)ctx);
#endif
    mod_timer(&ctx->usb.dbg_timer, jiffies + 1);
#endif

    return 0;
}


static void z_usb_disconnect(struct usb_interface *a_interface)
{
    struct z_device_ctx*    ctx;

    
    ctx = usb_get_intfdata(a_interface);
    if(ctx) {

        ctx->usb.isDisconnected = true;

        
        Z_TIMER_SHUTDOWN_SYNC(&ctx->usb.timer);
        ctx->usb.timer.expires = 0;

#if (Z_DEBUG_THROUGHPUT == 1)
        Z_TIMER_SHUTDOWN_SYNC(&ctx->usb.dbg_timer);
        ctx->usb.dbg_timer.expires = 0;
#endif

        
        z_usb_free_urbs(ctx);

        
        if(ctx->proto.engine)
            z_ProtoDelete(ctx->proto.engine);

        
        if(ctx->ctrl.engine)
            z_CommandEngineDelete(ctx->ctrl.engine);

        
        z_chr_unregister(ctx);

        
        z_snd_unregister(ctx);

        
        kfifo_free(&ctx->snd.fifo);

        
        remove_context_ptr(ctx);

        
        kfree(ctx);
    }

    
    usb_set_intfdata(a_interface, NULL);

#if (Z_DEBUG_INIT == 1)
    PRINT("USB Device deregistered (ctx=%p)", ctx);
#endif
}




int z_usb_driver_register(void)
{
    int res;

    
    res = usb_register(&z_usb_driver);
    if(res) {
        PRINT("Error registering USB driver!");
        return res;
    }

    return 0;
}


int z_usb_driver_unregister(void)
{
    
    usb_deregister(&z_usb_driver);

    return 0;
}
