





#include "mixer.h"

#include "z_CommandEngine.h"
#include "snd.h"






static const uint64_t dbtoLinGain[] = {
    65536, 
    73532, 
    82504, 
    92572, 
    103867, 
    116541, 
    130761, 
    146716, 
    164618, 
    184705, 
    207243, 
    232530, 
    260903, 
    292738, 
    328458, 
    368536, 
    413504, 
    463959, 
    520570, 
    584090, 
    655360, 
    735326, 
    825049, 
    925720, 
    1038675, 
    1165413, 
    1307615, 
    1467168, 
    1646189, 
    1847055, 
    2072430, 
    2325305, 
    2609035, 
    2927385, 
    3284580, 
    3685360, 
    4135042, 
    4639593, 
    5205709, 
    5840902, 
    6553600, 
    7353260, 
    8250493, 
    9257206, 
    10386756, 
    11654131, 
    13076151, 
    14671682, 
    16461898, 
    18470554, 
    20724302, 
    23253050, 
    26090351, 
    29273855, 
    32845806, 
    36853601, 
    41350420, 
    46395934, 
    52057095, 
    58409021, 
    65536000, 
    73532601, 
    82504935, 
    92572060, 
    103867560, 
    116541319, 
    130761511, 
    146716828, 
    164618989, 
    184705543, 
    207243028, 
};





static DECLARE_TLV_DB_MINMAX(z_mixer_digital_gain_scale, (int)(100.0*Z_DIGITAL_GAIN_MIN), (int)(100.0*Z_DIGITAL_GAIN_MAX) );


static int z_mixer_digital_gain_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo) {
    uinfo->type                 = SNDRV_CTL_ELEM_TYPE_INTEGER;
    uinfo->count                = 1;
    uinfo->value.integer.min    = (int)(Z_DIGITAL_GAIN_MIN / Z_DIGITAL_GAIN_STEP);
    uinfo->value.integer.max    = (int)(Z_DIGITAL_GAIN_MAX / Z_DIGITAL_GAIN_STEP);
    uinfo->value.integer.step   = 1;
    
    return 0;
}


static int z_mixer_digital_gain_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol) {
    struct z_device_ctx* ctx = (struct z_device_ctx*)snd_kcontrol_chip(kcontrol);
    
    if(!ctx) {
        PRINT("ctx == NULL");
        return -1;
    }

    const int maxGain = ARRAY_SIZE(dbtoLinGain);

    const uint64_t gainLin = ctx->gain.masterGain;
    int gainDb = 0;
    for (int i = 0; i < maxGain; ++i) {
        if (gainLin >= dbtoLinGain[i]) {
            gainDb = i;
        }
    }
    
    
    ucontrol->value.integer.value[0] = gainDb;

#if (Z_DEBUG_MIXER == 1)
    PRINT("Mixer - Get gain %d", ctx->gain.masterGain);
#endif

    return 0;
}


static int z_mixer_digital_gain_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol) {
    struct z_device_ctx* ctx = (struct z_device_ctx*)snd_kcontrol_chip(kcontrol);
        
    if(!ctx) {
        PRINT("ctx == NULL");
        return -1;
    }

    int maxGain = ARRAY_SIZE(dbtoLinGain) - 1;

    int gainDb = min((int)ucontrol->value.integer.value[0], maxGain);
    
    
    ctx->gain.masterGain = dbtoLinGain[gainDb];

    z_dev_update_gains(ctx);
    
#if (Z_DEBUG_MIXER == 1)
    PRINT("Mixer - Set gain %d", ctx->gain.masterGain);
#endif

    return 0;
}




static int z_mixer_testmode_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
    uinfo->type                 = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
    uinfo->count                = 1;
    uinfo->value.integer.min    = 0;
    uinfo->value.integer.max    = 1;

    return 0;
}


static int z_mixer_testmode_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
    struct z_device_ctx* ctx = (struct z_device_ctx*)snd_kcontrol_chip(kcontrol);
    
    if(!ctx) {
        PRINT("ctx == NULL");
        return -1;
    }
    
    
    ucontrol->value.integer.value[0] = ctx->snd.testMode;

    return 0;
}


static int z_mixer_testmode_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
    struct z_device_ctx* ctx = (struct z_device_ctx*)snd_kcontrol_chip(kcontrol);
        
    if(!ctx) {
        PRINT("ctx == NULL");
        return -1;
    }
    
    
    ctx->snd.testMode = ucontrol->value.integer.value[0];

    
    z_CmdSetTestMode(ctx->ctrl.engine, ctx->snd.testMode);
        
    
    if(ctx->snd.pcm_running) {
    
        if(ctx->snd.testMode) {
            z_CmdSetLedColor(ctx->ctrl.engine, Z_COLOR_MIN, Z_COLOR_MAX, Z_COLOR_MIN);
        } 
        else {
            z_CmdSetLedColor(ctx->ctrl.engine, Z_COLOR_MAX, Z_COLOR_MIN, Z_COLOR_MIN);
        }
    }
        
#if (Z_DEBUG_MIXER == 1)
    PRINT("%d", ctx->snd.mixer.testMode);
#endif
        
    return 0;
}


static int z_mixer_dimmer_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{

    uinfo->type                 = SNDRV_CTL_ELEM_TYPE_INTEGER;
    uinfo->count                = 1;
    uinfo->value.integer.min    = Z_COLOR_MIN;
    uinfo->value.integer.max    = Z_COLOR_MAX;
    uinfo->value.integer.step   = Z_COLOR_STEP;

    return 0;
}

static int z_mixer_dimmer_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol) {
    struct z_device_ctx* ctx = (struct z_device_ctx*)snd_kcontrol_chip(kcontrol);
    
    if(!ctx) {
        PRINT("ctx == NULL");
        return -1;
    }   
    
    
    ucontrol->value.integer.value[0] = z_CmdGetLedBrightness(ctx->ctrl.engine);

#if (Z_DEBUG_MIXER == 1)
    PRINT("%d", (int)ucontrol->value.integer.value[0]);
#endif

    return 0;
}

static int z_mixer_dimmer_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol) {
    struct z_device_ctx* ctx = (struct z_device_ctx*)snd_kcontrol_chip(kcontrol);
        
    if(!ctx) {
        PRINT("ctx == NULL");
        return -1;
    }

	
    z_CmdSetLedBrightness(ctx->ctrl.engine, ucontrol->value.integer.value[0]);

#if (Z_DEBUG_MIXER == 1)
    PRINT("%d", (int)ucontrol->value.integer.value[0]);
#endif

    return 0;
}




static struct snd_kcontrol_new  z_mixer_digital_controls[] = {
{
    .iface      = SNDRV_CTL_ELEM_IFACE_MIXER,
    .name       = "Master Gain",
    .index      = 0,
    .access     = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
    .info       = z_mixer_digital_gain_info,
    .get        = z_mixer_digital_gain_get,
    .put        = z_mixer_digital_gain_put,
    .tlv.p      = z_mixer_digital_gain_scale,
},
{
    .iface      = SNDRV_CTL_ELEM_IFACE_MIXER,
    .name       = "Capture TestMode Switch",
    .index      = 0,
    .access     = SNDRV_CTL_ELEM_ACCESS_READWRITE,
    .info       = z_mixer_testmode_info,
    .get        = z_mixer_testmode_get,
    .put        = z_mixer_testmode_put,
},
{
    .iface      = SNDRV_CTL_ELEM_IFACE_MIXER,
    .name       = "LED intensity",
    .index      = 0,
    .access     = SNDRV_CTL_ELEM_ACCESS_READWRITE,
    .info       = z_mixer_dimmer_info,
    .get        = z_mixer_dimmer_get,
    .put        = z_mixer_dimmer_put,
},
};




int z_snd_mixer_initialize(struct z_device_ctx* ctx) {
    
    strcpy(ctx->snd.card->mixername, Z_MIXER_NAME);
    
    
    for (int i = 0; i < ARRAY_SIZE(z_mixer_digital_controls); ++i) {

        
        struct snd_kcontrol* kctl = snd_ctl_new1(&z_mixer_digital_controls[i], ctx);
        if (!kctl) {
            PRINT("Out of memory!");
            continue;
        }

        if (0 == strcmp(z_mixer_digital_controls[i].name, "Master Gain")) {
            ctx->gain.masterGainControl = kctl;
        }

        
        kctl->id.device     = 0;
        kctl->id.subdevice  = 0;

        
        int res = snd_ctl_add(ctx->snd.card, kctl);
        if (res) {
            PRINT("snd_ctl_add() failed! (res=%d)", res);
        }
    }
    return 0;
}

