





#ifndef __Z_MIXER_H__
#define __Z_MIXER_H__


#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/init.h>

#include <sound/core.h>
#include <sound/initval.h>
#include <sound/control.h>
#include <sound/tlv.h>

#include "defs.h"
#include "device.h"


int z_snd_mixer_initialize  (struct z_device_ctx* ctx);

#endif
