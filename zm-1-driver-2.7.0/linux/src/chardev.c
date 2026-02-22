





#include <linux/cdev.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <asm/uaccess.h>

#include "version.h"
#include "mixer.h"
#include "snd.h"
#include "chardev.h"
#include "ftdi.h"
#include "ioctl.h"

#include "z_Utils.h"




static struct class*    z_chr_class;

struct z_chr_private_data {
    struct z_device_ctx* ctx;
};


static int z_chr_open(struct inode* a_Inode, struct file* a_File) {
    struct z_device_ctx* ctx = NULL;

    int i = 0;

    while((ctx = (struct z_device_ctx*)get_context(i)) != (void*)-1) {
        
        if(ctx && (ctx->chr.dev == a_Inode->i_cdev)) {
            break;
        } else {
            i++;
        }
    }

    
    if (ctx == NULL) {
        PRINT("Error finding device context! a_Inode=%p, a_File=%p", a_Inode, a_File);
        return -1;
    }

    
    if (ctx->chr.openCount != 0) {
#if (Z_DEBUG_CHARDEV == 1)
        PRINT("Already open!");
#endif
    }

    struct z_chr_private_data* pv = kmalloc(sizeof(struct z_chr_private_data), GFP_KERNEL);
    if (pv == NULL) {
        PRINT("Error allocating memory for private_data");
        return -1;
    }

    pv->ctx = ctx;

    
    a_File->private_data = pv;
    ctx->chr.openCount += 1;

#if (Z_DEBUG_CHARDEV == 1)
    PRINT("Opened");
#endif

    return 0;
}


static int z_chr_close(struct inode* a_Inode, struct file* a_File) {
    struct z_chr_private_data* pv = (struct z_chr_private_data*)a_File->private_data;
    if(!pv) { PRINT("pv == NULL"); return -1; }
    struct z_device_ctx* ctx = pv->ctx;
    if(!ctx) {
        PRINT("ctx == NULL");
        return -1;
    }

    
    if (ctx->chr.openCount == 0) {
#if (Z_DEBUG_CHARDEV == 1)
        PRINT("Not open!");
#endif
        return -1;
    }

    kfree(pv);

    
    a_File->private_data  = NULL;
    ctx->chr.openCount   -= 1;

#if (Z_DEBUG_CHARDEV == 1)
    PRINT("Closed");
#endif

    return 0;
}


static long z_ioctl_q_eeprom(struct z_device_ctx* a_Ctx, uint8_t a_Buffer[Z_IOCTL_BUFFER_SIZE], unsigned long a_IoctlParam) {
    copy_from_user(a_Buffer, (void*)a_IoctlParam, 2 * sizeof(uint8_t));
    const uint8_t addr = a_Buffer[0];
    const uint8_t size = min(a_Buffer[1], (uint8_t)(Z_IOCTL_BUFFER_SIZE/2)); 

    int res = z_read_eeprom(a_Ctx, a_Buffer+2, addr, size);
    copy_to_user((void*)a_IoctlParam, a_Buffer, sizeof(uint8_t) * (2+size));

    return res;
}


static long z_ioctl_s_eeprom(struct z_device_ctx* a_Ctx, uint8_t a_Buffer[Z_IOCTL_BUFFER_SIZE], unsigned long a_IoctlParam) {
    
    unsigned short eeprom[FTDI_EEPROM_SIZE/2];
    int res = 0;
    for (uint8_t i = 0; i<FTDI_EEPROM_SIZE/2 ; i++) {
        res = ftdi_get_eeprom(a_Ctx, i, eeprom + i);
        if (res < 0) goto end;
    }

    copy_from_user(a_Buffer, (void*)a_IoctlParam, Z_IOCTL_BUFFER_SIZE * sizeof(uint8_t));
    const uint8_t addr = a_Buffer[0];
    const uint8_t size = min(a_Buffer[1], (uint8_t)(Z_IOCTL_BUFFER_SIZE/2)); 
    const uint8_t* bufferData = a_Buffer + 2;

    uint16_t buffer16[Z_IOCTL_BUFFER_SIZE/2];
    bytesToShorts(bufferData, size, buffer16);

    for (uint8_t i = 0; i<size/2; ++i) {
        const unsigned short val = buffer16[i];
#if (Z_DEBUG_CHARDEV == 1)
        PRINT("set addr (0x%02X) to (0x%04X)", addr + i/2, val);
#endif
        res = ftdi_set_eeprom(a_Ctx, addr + i, val); 
        eeprom[addr + i] = val; 
        if (res < 0) goto end;
    }
    const unsigned short checksum = calcFTDIChecksum(eeprom, FTDI_EEPROM_SIZE/2-1);
    res = ftdi_set_eeprom(a_Ctx, FTDI_EEPROM_SIZE/2-1, checksum); 

    end:
    return res < 0 ? -1 : 0;
}


static long z_chr_compat_ioctl(struct file* a_File, unsigned int a_IoctlCode, unsigned long a_IoctlParam) {
    uint8_t              buffer[Z_IOCTL_BUFFER_SIZE];
    int                  res = 0;
    int return_code = -1;

    struct z_chr_private_data* pv = (struct z_chr_private_data*)a_File->private_data;
    if(!pv) { PRINT("pv == NULL"); return -1; }

    struct z_device_ctx* ctx = pv->ctx;
    if(!ctx) {
        PRINT("ctx == NULL");
        return -1;
    }

    
    if (ctx->chr.openCount == 0) {
#if (Z_DEBUG_CHARDEV == 1)
        PRINT("Not open!");
#endif
        return -1;
    }

#if (Z_DEBUG_CHARDEV == 1)
    PRINT("code: %08Xh, param: %p", a_IoctlCode, (void*)a_IoctlParam);
#endif

    switch (a_IoctlCode) {

    case Z_IOCTL_Q_DRIVER_VERSION:  

        
        strcpy(buffer, Z_DRIVER_VERSION_STRING);

        copy_to_user((void*)a_IoctlParam, buffer, strlen(buffer)+1);
        return_code = 0;
        break;

    case Z_IOCTL_Q_DEVICE_MODEL:    

        
        res = usb_string(ctx->usb.dev, ctx->usb.dev->descriptor.iProduct, buffer, sizeof(buffer)-1);
        if (res < 0) {
            return_code = -1;
            break;
        }

        copy_to_user((void*)a_IoctlParam, buffer, strlen(buffer)+1);
        return_code = 0;
        break;

    case Z_IOCTL_Q_DEVICE_SERIAL:   

        
        res = usb_string(ctx->usb.dev, ctx->usb.dev->descriptor.iSerialNumber, buffer, sizeof(buffer)-1 );
        if (res < 0) {
            return_code = -1;
            break;
        }

        copy_to_user((void*)a_IoctlParam, buffer, strlen(buffer)+1);
        return_code = 0;
        break;


    case Z_IOCTL_Q_FPGA_FW_VERSION:   

        
        if (!ctx->ctrl.engine->versionRom.isValid) {
            return_code = 1;
            break;
        }

        copy_to_user((void*)a_IoctlParam, ctx->ctrl.engine->versionRom.data, Z_VERSION_ROM_SIZE);
        return_code = 0;
        break;


    case Z_IOCTL_Q_LED_BRIGHTNESS:      

        buffer[0] = z_CmdGetLedBrightness(ctx->ctrl.engine);
        copy_to_user((void*)a_IoctlParam, buffer, sizeof(uint8_t));
        return_code = 0;
        break;

    case Z_IOCTL_S_LED_BRIGHTNESS:      

        copy_from_user(buffer, (void*)a_IoctlParam, sizeof(uint8_t));
        z_CmdSetLedBrightness(ctx->ctrl.engine, buffer[0]);
        return_code = 0;
        break;

    case Z_IOCTL_Q_LED_CONTROL_MODE:    

        buffer[0] = ctx->led.controlMode;
        copy_to_user((void*)a_IoctlParam, buffer, sizeof(uint8_t));
        return_code = 0;
        break;

    case Z_IOCTL_S_LED_CONTROL_MODE:    

        copy_from_user(buffer, (void*)a_IoctlParam, sizeof(uint8_t));
        z_dev_set_led_mode(ctx, buffer[0]);
        return_code = 0;
        break;

    case Z_IOCTL_S_LED_COLOR:           

        copy_from_user(buffer, (void*)a_IoctlParam, 3 * sizeof(uint8_t));

        memcpy(ctx->led.color, buffer, 3 * sizeof(uint8_t));
        z_dev_update_led_color(ctx);
        return_code = 0;
        break;

    case Z_IOCTL_S_FTDI_EEPROM:
        return_code = z_ioctl_s_eeprom(ctx, buffer, a_IoctlParam);
        break;

    case Z_IOCTL_Q_FTDI_EEPROM:
        return_code = z_ioctl_q_eeprom(ctx, buffer, a_IoctlParam);
        break;

    case Z_IOCTL_S_SKIP_CORRECTIONS:
        copy_from_user(buffer, (void*)a_IoctlParam, sizeof(uint8_t));
        z_dev_set_correction_enabled(ctx, buffer[0]==0);
        return_code = 0;
        break;

    case Z_IOCTL_Q_SKIP_CORRECTIONS:
        buffer[0] = !ctx->gain.correctionEnabled;
        copy_to_user((void*)a_IoctlParam, buffer, sizeof(uint8_t));
        return_code = 0;
        break;

    case Z_IOCTL_S_TESTMODE:
        copy_from_user(buffer, (void*)a_IoctlParam, sizeof(uint8_t));
        ctx->snd.testMode = buffer[0];
        z_CmdSetTestMode(ctx->ctrl.engine, ctx->snd.testMode);
        z_dev_update_led_color(ctx);
        return_code = 0;
        break;

    case Z_IOCTL_Q_TESTMODE:
        buffer[0] = ctx->snd.testMode;
        copy_to_user((void*)a_IoctlParam, buffer, sizeof(uint8_t));
        return_code = 0;
        break;

    case Z_IOCTL_PRINT:
        copy_from_user(buffer, (void*)a_IoctlParam, Z_IOCTL_BUFFER_SIZE * sizeof(uint8_t));
        buffer[Z_IOCTL_BUFFER_SIZE-1] = '\0'; 
        printk(KERN_DEBUG "%s", buffer);
        printk(KERN_DEBUG "");
        return_code = 0;
        break;

    case Z_IOCTL_Q_GAIN: {
        *((uint32_t*)buffer) = ctx->gain.masterGain;
        copy_to_user((void*)a_IoctlParam, buffer, sizeof(uint32_t));
        return 0;
    }
    case Z_IOCTL_S_GAIN: {
        copy_from_user(buffer, (void*)a_IoctlParam, sizeof(uint32_t));
        ctx->gain.masterGain = *((uint32_t*)buffer);
#if (Z_DEBUG_MIXER == 1)
        PRINT("Set master gain to: %d", ctx->gain.masterGain);
#endif
        z_dev_update_gains(ctx);

        snd_ctl_notify(ctx->snd.card, SNDRV_CTL_EVENT_MASK_VALUE, &(ctx->gain.masterGainControl->id));
        return 0;
    }
    }

    return return_code;
}


static long z_chr_unlocked_ioctl(struct file* a_File, unsigned int a_IoctlCode, unsigned long a_IoctlParam)
{
    return z_chr_compat_ioctl(a_File, a_IoctlCode, a_IoctlParam);
}



static struct file_operations z_chardev_fops = {
.owner          = THIS_MODULE,
.open           = z_chr_open,
.release        = z_chr_close,
.compat_ioctl   = z_chr_compat_ioctl,
.unlocked_ioctl = z_chr_unlocked_ioctl
};




int z_chr_register(struct z_device_ctx* ctx)
{
    int     res;

    if(!ctx) {
        PRINT("ctx == NULL");
        return -1;
    }

    ctx->chr.dev = NULL;

    
    ctx->chr.dev = cdev_alloc();
    if(!ctx->chr.dev) {
        PRINT("cdev_alloc() failed!");
        goto err_cleanup;
    }

    ctx->chr.dev->owner    = THIS_MODULE;
    ctx->chr.dev->ops      = &z_chardev_fops;

    sprintf(ctx->chr.name, Z_CHARDEV_NAME, ctx->index);

    
    res = alloc_chrdev_region(&ctx->chr.id, ctx->index, 1, ctx->chr.name);
    if(res < 0) {
        PRINT("alloc_chrdev_region() failed! (res=%d)", res);
        goto err_cleanup;
    }

    
    res = cdev_add(ctx->chr.dev, ctx->chr.id, 1);
    if(res < 0) {
        PRINT("cdev_add() failed! (res=%d)", res);
        goto err_cleanup;
    }

    
    if(!device_create(z_chr_class, NULL, ctx->chr.id, ctx, ctx->chr.name)) {
        PRINT("device_create() failed!");
        goto err_cleanup;
    }

#if (Z_DEBUG_CHARDEV == 1)
    PRINT("Device '/dev/%s' registered (major=%d, minor=%d)", ctx->chr.name, MAJOR(ctx->chr.id), MINOR(ctx->chr.id));
#endif

    return 0;

err_cleanup:

    
    if(ctx->chr.dev) {
        device_destroy(z_chr_class, ctx->chr.id);
        cdev_del(ctx->chr.dev);
        ctx->chr.dev = NULL;
    }

    
    unregister_chrdev_region(ctx->chr.id, 1);

    return -1;
}


int z_chr_unregister(struct z_device_ctx* ctx)
{
    if(!ctx) {
        PRINT("ctx == NULL");
        return -1;
    }

    
    if(ctx->chr.dev) {
        device_destroy(z_chr_class, ctx->chr.id);
        cdev_del(ctx->chr.dev);
        ctx->chr.dev = NULL;
    }

    
    unregister_chrdev_region(ctx->chr.id, 1);

#if (Z_DEBUG_CHARDEV == 1)
    PRINT("Device '/dev/%s' unregistered", ctx->chr.name);
#endif

    return 0;
}





#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
static int z_chr_uevent(struct device* a_Dev, struct kobj_uevent_env* a_Env)
#else
static int z_chr_uevent(const struct device* a_Dev, struct kobj_uevent_env* a_Env)
#endif
{
    add_uevent_var(a_Env, "DEVMODE=%#o", 0666);
    return 0;
}




int z_chr_register_class(void)
{
    
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
    z_chr_class = class_create(THIS_MODULE, Z_DRIVER_NAME);
#else
    z_chr_class = class_create(Z_DRIVER_NAME);
#endif

    if (!z_chr_class) {
        PRINT("Error registering character device class '"Z_DRIVER_NAME"'");
        return -1;
    }

    
    z_chr_class->dev_uevent = z_chr_uevent;

#if (Z_DEBUG_CHARDEV == 1)
    PRINT("Character device class '"Z_DRIVER_NAME"' registered.");
#endif

    return 0;
}


int z_chr_unregister_class(void)
{
    
    if (z_chr_class) {
        class_destroy(z_chr_class);
        z_chr_class = NULL;
    }

#if (Z_DEBUG_CHARDEV == 1)
    PRINT("Character device class '"Z_DRIVER_NAME"' unregistered.");
#endif

    return 0;
}

