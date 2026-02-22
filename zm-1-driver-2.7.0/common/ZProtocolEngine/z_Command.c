



#include "z_Command.h"




zCommand* z_CmdNew(void) {
    zCommand* cmd = NULL;

#if (LINUX_KERNEL_MODULE == 1)

    
    if(in_interrupt()) {
        cmd = (zCommand*)kzalloc(sizeof(zCommand), GFP_ATOMIC);
    }
    else {
        cmd = (zCommand*)kzalloc(sizeof(zCommand), GFP_KERNEL);
    }

#elif (MACOS_KERNEL_MODULE == 1)

    cmd = (zCommand*)IOMalloc(sizeof(zCommand));
    if(cmd == NULL) {
        return NULL;
    }

    memset(cmd, 0, sizeof(zCommand));

#elif (WINDOWS_KERNEL_MODULE == 1)

    cmd = (zCommand*)ExAllocatePoolWithTag(NonPagedPool, sizeof(zCommand), Z_CMD_POOL_TAG);
    if(cmd == NULL) {
        return NULL;
    }

    RtlZeroMemory((void*)cmd, sizeof(zCommand));

#elif (WINDOWS_USER_MODULE == 1)

    cmd = (zCommand*)calloc(1, sizeof(zCommand));
    
#elif (MACOS_USER_MODULE == 1)
    
    cmd = IONewZero(zCommand, 1);

#else

    
    cmd = (zCommand*)calloc(1, sizeof(zCommand));

#endif

    return cmd;
}



zCommand* z_CmdDelete(zCommand* a_Cmd) {
    if(a_Cmd == NULL) {
        return NULL;
    }

#if (LINUX_KERNEL_MODULE == 1)

    
    kfree(a_Cmd);

#elif (MACOS_KERNEL_MODULE == 1)

    IOFree((void*)a_Cmd, sizeof(zCommand));

#elif (WINDOWS_KERNEL_MODULE == 1)

    ExFreePoolWithTag((void*)a_Cmd, Z_CMD_POOL_TAG);

#elif (WINDOWS_USER_MODULE == 1)

    free(a_Cmd);
    
#elif (MACOS_USER_MODULE == 1)
    IODelete(a_Cmd, zCommand, 1);
#else

    
    free(a_Cmd);

#endif

    return NULL;
}




zCommand* z_CmdNewRegRd(uint8_t a_Address)
{
    zCommand*   cmd = NULL;

    
    cmd = z_CmdNew();
    if(cmd == NULL) {
        return NULL;
    }

    
    cmd->type           = Z_CMD_REG;
    cmd->reg.wr         = 0;
    cmd->reg.address    = a_Address;

    return cmd;
}


zCommand* z_CmdNewRegWr(uint8_t a_Address, uint16_t a_Value)
{
    zCommand*   cmd = NULL;

    
    cmd = z_CmdNew();
    if(cmd == NULL) {
        return NULL;
    }

    
    cmd->type           = Z_CMD_REG;
    cmd->reg.wr         = 1;
    cmd->reg.address    = a_Address;
    cmd->reg.value      = a_Value;

    return cmd;
}


zCommand* z_CmdNewVersionRd(uint8_t a_Address)
{
    zCommand*   cmd = NULL;

    
    cmd = z_CmdNew();
    if(cmd == NULL) {
        return NULL;
    }

    
    cmd->type               = Z_CMD_VERSION_ROM;
    cmd->versionRom.address = a_Address;

    return cmd;
}


zCommand* z_CmdNewI2cRd(int a_Chip, uint8_t a_Address, int a_WaitMs)
{
    zCommand*   cmd = NULL;

    
    cmd = z_CmdNew();
    if(cmd == NULL) {
        return NULL;
    }

    
    cmd->type             = Z_CMD_I2C;
    cmd->i2c.wr           = 0;
    cmd->i2c.chip         = (uint8_t)a_Chip;
    cmd->i2c.reg.address  = a_Address;
    cmd->i2c.reg.waitMs   = a_WaitMs;

    return cmd;
}


zCommand* z_CmdNewI2cWr(int a_Chip, uint8_t a_Address, uint16_t a_Value, int a_WaitMs)
{
    zCommand*   cmd = NULL;

    
    cmd = z_CmdNew();
    if(cmd == NULL) {
        return NULL;
    }

    
    cmd->type             = Z_CMD_I2C;
    cmd->i2c.wr           = 1;
    cmd->i2c.chip         = (uint8_t)a_Chip;
    cmd->i2c.reg.address  = a_Address;
    cmd->i2c.reg.value    = a_Value;
    cmd->i2c.reg.waitMs   = a_WaitMs;

    return cmd;
}


zCommand* z_CmdNewCommit(void)
{
    zCommand*   cmd = NULL;

    
    cmd = z_CmdNew();
    if(cmd == NULL) {
        return NULL;
    }

    
    cmd->type           = Z_CMD_COMMIT;

    return cmd;
}


zCommand* z_CmdNewLedColor(uint8_t a_Red, uint8_t a_Green, uint8_t a_Blue)
{
    zCommand*   cmd = NULL;

    
    cmd = z_CmdNew();
    if(cmd == NULL) {
        return NULL;
    }

    
    cmd->type             = Z_CMD_LED;
    cmd->led.red          = a_Red;
    cmd->led.green        = a_Green;
    cmd->led.blue         = a_Blue;

    return cmd;
}



