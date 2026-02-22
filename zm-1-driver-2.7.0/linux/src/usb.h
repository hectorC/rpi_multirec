





#ifndef __Z_USB_H__
#define __Z_USB_H__


#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "defs.h"
#include "device.h"




typedef struct z_urb_ctx
{
    struct z_device_ctx*    ctx;    
    struct urb*             urb;    
    
    uint8_t*                buf;    
    int                     status; 
} z_urb_ctx;


int z_usb_driver_register    (void);
int z_usb_driver_unregister  (void);

#endif
