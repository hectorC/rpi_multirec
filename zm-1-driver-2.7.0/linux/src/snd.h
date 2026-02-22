





#ifndef __Z_SND_H__
#define __Z_SND_H__


#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/init.h>

#include <linux/usb.h>

#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#include "defs.h"
#include "device.h"


int z_snd_register      (struct z_device_ctx* ctx);
int z_snd_unregister    (struct z_device_ctx* ctx);
int z_backlight		(struct z_device_ctx* ctx);

#endif
