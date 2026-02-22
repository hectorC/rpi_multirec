





#ifndef __Z_CHARDEV_H__
#define __Z_CHARDEV_H__


#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>

#include "defs.h"
#include "device.h"


int z_chr_register          (struct z_device_ctx* ctx);
int z_chr_unregister        (struct z_device_ctx* ctx);

int z_chr_register_class    (void);
int z_chr_unregister_class  (void);

#endif  
