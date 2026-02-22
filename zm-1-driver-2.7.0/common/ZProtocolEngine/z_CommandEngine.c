



#include "z_CommandEngine.h"

#if (MACOS_KERNEL_MODULE == 1) || (LINUX_KERNEL_MODULE == 1) || (WINDOWS_KERNEL_MODULE == 1)
#define PRId64 "lld"
#else
#include <inttypes.h>
#endif


static int z_CommandEngineLock      (zCommandEngine* a_Engine);
static int z_CommandEngineUnlock    (zCommandEngine* a_Engine);

static int z_CommandEngineParseVersionROM (zCommandEngine* a_Engine);





void z_CommandEngineLog(zCommandEngine* a_Engine, int a_Level, const char* a_Format, ...)
{
    va_list va;

    
    if((a_Engine == NULL) || (a_Engine->verboseLevel < a_Level)) {
        (void)va;
        return;
    }

#if (LINUX_KERNEL_MODULE == 1)

    va_start(va, a_Format);
    printk(KERN_DEBUG "zCommandEngine: ");
    vprintk(a_Format, va);
    va_end(va);

#elif (MACOS_KERNEL_MODULE == 1) || (MACOS_USER_MODULE == 1)
    
    {
        char msgBuffer[256] = "ZYLIA zCommandEngine: ";
        strncat(msgBuffer, a_Format, 256);

        va_start(va, a_Format);
        IOLogv(msgBuffer, va);
        va_end(va);
        
    }

#elif (WINDOWS_KERNEL_MODULE == 1)

    va_start(va, a_Format);
    vDbgPrintExWithPrefix("zCommandEngine: ", DPFLTR_IHVSTREAMING_ID, DPFLTR_MASK | DPFLTR_TRACE_LEVEL, a_Format, va);
    va_end(va);

#elif (WINDOWS_USER_MODULE == 1)
#ifdef _DEBUG
    {
        char msgFormat[64];
        char msgBuffer[256];

        va_start(va, a_Format);
        sprintf_s(msgFormat, sizeof(msgFormat), "zCommandEngine: %s", a_Format);
        vsprintf_s(msgBuffer, sizeof(msgBuffer), msgFormat, va);
        va_end(va);

        OutputDebugString(msgBuffer);
    }

#endif 

#else

    va_start(va, a_Format);
    fprintf(stderr, "zCommandEngine: ");
    vfprintf(stderr, a_Format, va);
    va_end(va);

#endif
}


zCommandEngine* z_CommandEngineCreate(void) {
    zCommandEngine* engine = NULL;

#if (LINUX_KERNEL_MODULE == 1)

    
    if(in_interrupt()) {
        engine = (zCommandEngine*)kzalloc(sizeof(zCommandEngine), GFP_ATOMIC);
    }
    else {
        engine = (zCommandEngine*)kzalloc(sizeof(zCommandEngine), GFP_KERNEL);
    }

    if(engine == NULL) {
        return NULL;
    }

    
    spin_lock_init(&engine->lock);

    
    engine->initialJiffies = jiffies;

#elif (MACOS_KERNEL_MODULE == 1)

    
    engine = (zCommandEngine*)IOMalloc(sizeof(zCommandEngine));
    if(engine == NULL) {
        return NULL;
    }

    memset(engine, 0, sizeof(zCommandEngine));

    
    engine->lock = IOSimpleLockAlloc();

    if(engine->lock == NULL) {
        IOFree(engine, sizeof(zCommandEngine));
        return NULL;
    }

#elif (WINDOWS_KERNEL_MODULE == 1)

    
    engine = (zCommandEngine*)ExAllocatePoolWithTag(NonPagedPool, sizeof(zCommandEngine), Z_CMD_ENGINE_POOL_TAG);
    if(engine == NULL) {
        return NULL;
    }

    RtlZeroMemory((void*)engine, sizeof(zCommandEngine));

    
    KeInitializeSpinLock(&engine->lock);

#elif (WINDOWS_USER_MODULE == 1)

    
    engine = (zCommandEngine*)calloc(1, sizeof(zCommandEngine));
    if(engine == NULL) {
        return NULL;
    }

    
    engine->mutex = CreateMutex(NULL, FALSE, NULL);
    
#elif (MACOS_USER_MODULE == 1)

    
    
    return NULL;

#else

    
    engine = (zCommandEngine*)calloc(1, sizeof(zCommandEngine));

    if(engine == NULL) {
        return NULL;
    }

#endif
    return engine;
}


void z_CommandEngineInit(zCommandEngine* engine, void* a_UserContext, void* a_UserData) {

    
    engine->userContext     = a_UserContext;
    engine->userData        = a_UserData;

    
    
    engine->led.brightness  = 255;

    
    engine->commandQueue    = z_FifoCreate(128);    
    if(!engine->commandQueue) {
        z_CommandEngineLog(engine, 0, "FIFO _NOT_ created.\n");
        z_CommandEngineDelete(engine);
        return;
    }

    z_CommandEngineLog(engine, 0, "Command engine initialized.\n");

    return;
}


zCommandEngine* z_CommandEngineDelete(zCommandEngine* a_Engine) {
    if(a_Engine == NULL) {
        return NULL;
    }

    
    if(a_Engine->commandQueue != NULL) {
        z_FifoDelete(a_Engine->commandQueue);
    }

#if (LINUX_KERNEL_MODULE == 1)

    
    kfree(a_Engine);

#elif (MACOS_KERNEL_MODULE == 1)

    
    if (a_Engine->lock != NULL) {
        IOSimpleLockFree(a_Engine->lock);
    }

    
    IOFree(a_Engine, sizeof(zCommandEngine));

#elif (WINDOWS_KERNEL_MODULE == 1)

    ExFreePoolWithTag((void*)a_Engine, Z_CMD_ENGINE_POOL_TAG);
    
#elif (WINDOWS_USER_MODULE == 1)

    
    CloseHandle(a_Engine->mutex);

    
    free(a_Engine);
    
#elif (MACOS_USER_MODULE == 1)

    return NULL;
    

#else

    
    free(a_Engine);

#endif

    z_CommandEngineLog(NULL, 0, "Command engine freed.\n");

    return NULL;
}


int z_CommandEngineSetNotificationCallback(zCommandEngine* a_Engine, NotificationCallback a_Callback) {
    if(a_Engine == NULL) {
        return -1;
    }

    a_Engine->notificationCallback = a_Callback;

    return 0;
}




static int z_CommandEngineLock(zCommandEngine* a_Engine)
{
#if (LINUX_KERNEL_MODULE == 1)

    
    spin_lock_irqsave(&a_Engine->lock, a_Engine->lockFlags);

#elif (MACOS_KERNEL_MODULE == 1)

    
    IOSimpleLockLock(a_Engine->lock);

#elif (WINDOWS_KERNEL_MODULE == 1)

    
    KeAcquireSpinLock(&a_Engine->lock, &a_Engine->lockFlags);

#elif (WINDOWS_USER_MODULE == 1)

    
    WaitForSingleObject(a_Engine->mutex, INFINITE);

#else

    

#endif

    return 0;
}



static int z_CommandEngineUnlock(zCommandEngine* a_Engine)
{
#if (LINUX_KERNEL_MODULE == 1)

    
    spin_unlock_irqrestore(&a_Engine->lock, a_Engine->lockFlags);

#elif (MACOS_KERNEL_MODULE == 1)

    
    IOSimpleLockUnlock(a_Engine->lock);

#elif (WINDOWS_KERNEL_MODULE == 1)

    
    KeReleaseSpinLock(&a_Engine->lock, a_Engine->lockFlags);

#elif (WINDOWS_USER_MODULE == 1)

    
    ReleaseMutex(a_Engine->mutex);

#else

    

#endif

    return 0;
}





int z_CommandEngineGetTimeMs(zCommandEngine* a_Engine, int64_t* a_Time)
{
    if(a_Engine == NULL) {
        return -1;
    }

#if (LINUX_KERNEL_MODULE == 1)

    
    int64_t tmpTime = ((jiffies - a_Engine->initialJiffies) * 1000);
    (*a_Time) = do_div(tmpTime, HZ);

#elif (MACOS_KERNEL_MODULE == 1)

    uint64_t time;

    
    clock_get_uptime(&time);

    
    absolutetime_to_nanoseconds(time, &time);

    
    (*a_Time) = (int64_t)time / (int64_t)1000000;

#elif (WINDOWS_KERNEL_MODULE == 1)

    LARGE_INTEGER time;

    
    KeQuerySystemTime(&time);

    
    (*a_Time) = (int64_t)time.QuadPart / (int64_t)10000;

#elif (WINDOWS_USER_MODULE == 1)

    LARGE_INTEGER   counterFreq;
    LARGE_INTEGER   counterValue;

    QueryPerformanceFrequency(&counterFreq);
    QueryPerformanceCounter(&counterValue);

    
    (*a_Time) = (counterValue.QuadPart * 1000) / counterFreq.QuadPart;
    
#elif (MACOS_USER_MODULE == 1)
    
    (*a_Time) = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / (int64_t)1000000;
    

#else

    
    (*a_Time) = 0;

#endif

    return 0;
}




int z_CommandEnginePush(zCommandEngine* a_Engine, zCommand* a_Command)
{
    int     res;

    if((a_Engine == NULL) || (a_Engine->commandQueue == NULL)) {
        return -1;
    }

    if(a_Command == NULL) {
        z_CommandEngineLog(a_Engine, 0, "Attempt to enqueue NULL command!\n");
        return -1;
    }

    
    res = z_FifoPush(a_Engine->commandQueue, (void*)a_Command);
    z_CommandEngineLog(a_Engine, 3, "Command enqueued. %d commands in queue.\n", a_Engine->commandQueue->occupancy);

    return res;
}


int z_CommandEngineProcess(zCommandEngine* a_Engine, uint32_t* a_CtrlWord) {
    uint32_t    word = 0L;
    int64_t     currTime;

    if((a_Engine == NULL) || (a_Engine->commandQueue == NULL) || (a_CtrlWord == NULL)) {
        return -1;
    }

    
    if(a_Engine->currCommand) {

        
        z_CommandEngineGetTimeMs(a_Engine, &currTime);

        
        if(currTime > a_Engine->timeoutTime)
        {
            z_CommandEngineLog(a_Engine, 1, "Pending command timeout! currTime %" PRId64 " timeoutTime %" PRId64, currTime, a_Engine->timeoutTime);

            
            a_Engine->currCommand = z_CmdDelete(a_Engine->currCommand);
            a_Engine->timeoutTime = 0;
        }
        
        else {
            return 1;
        }
    }

    
    if(a_Engine->commandQueue->occupancy == 0) {
        return 1;
    }

    
    z_FifoPop(a_Engine->commandQueue, (void**)&(a_Engine->currCommand));

    
    if(a_Engine->currCommand == NULL) {
        z_CommandEngineLog(a_Engine, 0, "NULL pointer in queue!\n");
        return -1;
    }

    
    z_CommandEngineGetTimeMs(a_Engine, &currTime);
    a_Engine->timeoutTime = currTime + 100;    

    
    switch(a_Engine->currCommand->type)
    {
    case Z_CMD_I2C:     

        

        
        if(a_Engine->currCommand->i2c.wr) {
            word = 0x40000000;
        } else {
            word = 0x00000000;
        }

        
        word |= 0x0A000000 + (a_Engine->currCommand->i2c.chip >> 1) * 0x01000000;

        
        if(a_Engine->currCommand->i2c.chip & 1) {
            word |= 0x10000000;
        }

        
        word |= a_Engine->currCommand->i2c.reg.address << 16;
        word |= a_Engine->currCommand->i2c.reg.value;

        
        word |= 0x80000000;

        break;

    case Z_CMD_VERSION_ROM: 

        z_CommandEngineLog(a_Engine, 2, "VER[0x%02X] => ...\n", a_Engine->currCommand->versionRom.address);

        
        word  = 0x00000000;

        
        word |= a_Engine->currCommand->versionRom.address << 16;

        
        word |= 0x80000000;

        break;

    case Z_CMD_REG:     

        if(a_Engine->currCommand->reg.wr)
            z_CommandEngineLog(a_Engine, 2, "REG[0x%1X] <= 0x%04X\n", a_Engine->currCommand->reg.address, a_Engine->currCommand->reg.value);
        else
            z_CommandEngineLog(a_Engine, 2, "REG[0x%1X] => ...\n", a_Engine->currCommand->reg.address);

        
        if(a_Engine->currCommand->reg.wr) {
            word = 0x4E000000;
        } else {
            word = 0x0E000000;
        }

        
        word |= a_Engine->currCommand->reg.address << 16;
        word |= a_Engine->currCommand->reg.value;

        
        word |= 0x80000000;

        break;

    case Z_CMD_COMMIT:  

        z_CommandEngineLog(a_Engine, 2, "Commit sample rate change\n");

        
        word  = 0x0F000000;

        
        word |= 0x80000000;

        break;

    case Z_CMD_LED:     

        z_CommandEngineLog(a_Engine, 2, "LED R:%02X G:%02X B:%02X\n",
            a_Engine->currCommand->led.red,
            a_Engine->currCommand->led.green,
            a_Engine->currCommand->led.blue);

        word  = 0x08000000;
        word |= a_Engine->currCommand->led.red;
        word |= a_Engine->currCommand->led.green << 8;
        word |= a_Engine->currCommand->led.blue  << 16;

        
        word |= 0x80000000;

        break;

    default:
        z_CommandEngineLog(a_Engine, 0, "Unknown command in queue (type=%d)\n", a_Engine->currCommand->type);

        
        a_Engine->currCommand = z_CmdDelete(a_Engine->currCommand);
        return -1;
    }

    z_CommandEngineLog(a_Engine, 4, "Tx CTRL: 0x%08X\n", word);

    
    (*a_CtrlWord) = word;

    return 0;
}


int z_CommandEngineReceive(zCommandEngine* a_Engine, uint32_t a_CtrlWord) {
    int         i;
    int         address;
    uint16_t    data;

    z_CommandEngineLog(a_Engine, 4, "Rx CTRL: 0x%08X\n", a_CtrlWord);

    
    if(a_Engine->currCommand == NULL) {
        z_CommandEngineLog(a_Engine, 1, "Received response with no pending request!\n");
        return -1;
    }

    

    
    if((a_CtrlWord & 0x4F000000) == 0x00000000) {
        z_CommandEngineLog(a_Engine, 2, "VER[0x%02X] => 0x%04X\n", (a_CtrlWord & 0x00FF0000) >> 16, (a_CtrlWord & 0x0000FFFF));

        
        address = (a_CtrlWord & 0x00FF0000) >> 16;
        data    = (a_CtrlWord & 0x0000FFFF);

        
        if (address >= (Z_VERSION_ROM_SIZE/2)) {
            z_CommandEngineLog(a_Engine, 1, "Invalid version ROM chunk address! (0x%02X)\n", address);
        }
        else {

            a_Engine->versionRom.data[(address<<1)+0] = data >> 8;
            a_Engine->versionRom.data[(address<<1)+1] = data & 0xFF;

            a_Engine->versionRom.isValidRes[address] = 1;

            z_CommandEngineLog(a_Engine, 4, "Received  %c%c for address (0x%02X)\n", data >> 8, data & 0xFF, address);

            

            uint8_t allValid = 1;
            for (i = 0; i < Z_VERSION_ROM_SIZE/2; ++i) {
                if (!a_Engine->versionRom.isValidRes[i]) allValid = 0;
            }

            if (allValid) {

                
                for (i=0; i<Z_VERSION_ROM_SIZE; ++i) {
                    a_Engine->versionRom.string[i] = a_Engine->versionRom.data[i];
                }

                
                a_Engine->versionRom.isValid                    = 1;
                a_Engine->versionRom.string[Z_VERSION_ROM_SIZE] = 0;

                z_CommandEngineLog(a_Engine, 2, "Version ROM: '%s'\n", a_Engine->versionRom.string);

                
                z_CommandEngineParseVersionROM(a_Engine);

                
                if (a_Engine->notificationCallback != NULL) {
                    a_Engine->notificationCallback(a_Engine,
                                                   Z_NOTIFY_VERSION_ROM,
                                                   (void*)a_Engine->versionRom.string);
                }
            }
        }
    }

    
    if((a_CtrlWord & 0x4F000000) == 0x0E000000) {
        
        
        
        if (a_Engine->currCommand->type == Z_CMD_REG) {
            if (a_Engine->currCommand->reg.wr == 0) {

                z_CommandEngineLog(a_Engine, 2, "REG[0x%1X] => 0x%04X\n", (a_CtrlWord & 0x00FF0000) >> 16, (a_CtrlWord & 0x0000FFFF));

                
                address = (a_CtrlWord & 0x00FF0000) >> 16;
                data    = (a_CtrlWord & 0x0000FFFF);

                
                if (address >= 4) {
                    z_CommandEngineLog(a_Engine, 1, "Invalid register address! (0x%02X)\n", address);
                }
                
                else {
                    a_Engine->deviceRegisters[address] = data;

                    
                    if (a_Engine->notificationCallback != NULL) {

                        
                        if (address == 0x3) {

                            
                            if(!(a_Engine->caps & Z_CAP_BRIGHTNESS)) {
                                a_Engine->led.brightness = 255;
                            
                            } else {
                                a_Engine->led.brightness = (uint8_t)z_CmdGetLedBrightness(a_Engine);
                            }

                            
                            int32_t param = a_Engine->led.brightness;

                            a_Engine->notificationCallback(a_Engine,
                                                           Z_NOTIFY_LED_BRIGHTNESS,
                                                           (void*)&param);
                        }
                    }

                }
            }
        }
    }

    
    a_Engine->currCommand = z_CmdDelete(a_Engine->currCommand);

    return 0;
}


int z_CommandEngineIsPending(zCommandEngine* a_Engine)
{
    
    if (a_Engine->currCommand != NULL) {
        return 1;
    }

    
    if (a_Engine->commandQueue->occupancy != 0) {
        return 1;
    }

    
    return 0;
}




static int z_CommandEngineParseVersionROM(zCommandEngine* a_Engine) {
    const char* versionStr = a_Engine->versionRom.string;

    
    if (strlen(versionStr) < 5) {
        z_CommandEngineLog(a_Engine, 0, "Version string too short!\n");
        return -1;
    }

    int32_t fwVersion = 0;

    if (versionStr[0] == 'B' && strlen(versionStr) >= 9) {
        fwVersion  = 100 * (versionStr[4] - '0');
        fwVersion += 10  * (versionStr[6] - '0');
        fwVersion +=       (versionStr[8] - '0');
    }
    else {
        fwVersion  = 100 * (versionStr[0] - '0');
        fwVersion += 10  * (versionStr[2] - '0');
        fwVersion +=       (versionStr[4] - '0');
    }

    z_CommandEngineLog(a_Engine, 3, "fwVersion = %03d\n", fwVersion);

    uint8_t newCaps = 0x00;

    
    if (fwVersion >= 130) {
        newCaps |= Z_CAP_BRIGHTNESS;
    }

    
    a_Engine->caps |= newCaps;

    z_CommandEngineLog(a_Engine, 2, "New device caps: 0x%02X\n", a_Engine->caps);

    return 0;
}




int z_CmdReadRegisters(zCommandEngine* a_Engine) {
    int res = 0;

    z_CommandEngineLock(a_Engine);

    
    for (int i=0; i<4; ++i) {
        res = z_CommandEnginePush(a_Engine, z_CmdNewRegRd((uint8_t)i));
        if (res) break;
    }

    z_CommandEngineUnlock(a_Engine);
    return res;
}


int z_CmdReadVersionROM(zCommandEngine* a_Engine) {

    z_CommandEngineLock(a_Engine);

    int res = 0;

    
    for (int i = 0; i < Z_VERSION_ROM_SIZE / 2; ++i) {
        res = z_CommandEnginePush(a_Engine, z_CmdNewVersionRd((uint8_t)i));
        if (res) break;
    }

    z_CommandEngineUnlock(a_Engine);
    return res;
}


void z_CmdClearVersionROM(zCommandEngine* a_Engine) {
    z_CommandEngineLock(a_Engine);

    a_Engine->versionRom.isValid    = 0;
    for (int i = 0; i < Z_VERSION_ROM_SIZE/2; ++i) a_Engine->versionRom.isValidRes[i] = 0;
    for (int i = 0; i < Z_VERSION_ROM_SIZE; ++i) a_Engine->versionRom.data[i] = 0;

    z_CommandEngineUnlock(a_Engine);
}




int z_CmdEnableStreaming(zCommandEngine* a_Engine, int a_Enable)
{
    int         res;
    uint16_t    reg;

    z_CommandEngineLock(a_Engine);

    reg  = a_Engine->deviceRegisters[0x1];

    
    if(a_Enable) {
        reg |=  0x8000;
    }
    else {
        reg &= ~0x8000;
    }

    a_Engine->deviceRegisters[0x1] = reg;

    
    res = z_CommandEnginePush(a_Engine, z_CmdNewRegWr(0x1, reg));

    z_CommandEngineUnlock(a_Engine);
    return res;
}


int z_CmdSetSampleRate(zCommandEngine* a_Engine, int a_SampleRate)
{
    int         res;
    uint16_t    reg;

    z_CommandEngineLock(a_Engine);

    reg  = a_Engine->deviceRegisters[0x1];

    
    reg &= ~0x000F;

    
    switch(a_SampleRate)
    {
    case 16000: reg |= 0x000A; break;
    case 22050: reg |= 0x0006; break;
    case 32000: reg |= 0x0008; break;
    case 44100: reg |= 0x0004; break;
    case 48000: reg |= 0x0000; break;
    case 64000: reg |= 0x0009; break;
    case 88200: reg |= 0x0005; break;
    case 96000: reg |= 0x0001; break;

    default: 
        z_CommandEngineUnlock(a_Engine);
        return -1;
    }

    a_Engine->deviceRegisters[0x1] = reg;

    
    res = z_CommandEnginePush(a_Engine, z_CmdNewRegWr(0x1, reg));

    z_CommandEngineUnlock(a_Engine);
    return res;
}


int z_CmdSetSampleFormat(zCommandEngine* a_Engine, int a_Format)
{
    int         res;
    uint16_t    reg;

    z_CommandEngineLock(a_Engine);

    reg  = a_Engine->deviceRegisters[0x2];

    
    reg &= ~0x0F00;

    
    if(a_Format) {
        reg |= 0x0100;
    }

    a_Engine->deviceRegisters[0x2] = reg;

    
    res = z_CommandEnginePush(a_Engine, z_CmdNewRegWr(0x2, reg));

    z_CommandEngineUnlock(a_Engine);
    return res;
}


int z_CmdSetChannelCount(zCommandEngine* a_Engine, int a_Channels)
{
    int         res;
    uint16_t    reg;

    z_CommandEngineLock(a_Engine);

    reg  = a_Engine->deviceRegisters[0x2];

    
    reg &= ~0x00FF;

    
    reg |= (a_Channels - 1);

    a_Engine->deviceRegisters[0x2] = reg;

    
    res = z_CommandEnginePush(a_Engine, z_CmdNewRegWr(0x2, reg));

    z_CommandEngineUnlock(a_Engine);
    return res;
}


int z_CmdSetAveragingMode(zCommandEngine* a_Engine, int a_Mode)
{
    int         res;
    uint16_t    reg;

    z_CommandEngineLock(a_Engine);

    reg  = a_Engine->deviceRegisters[0x2];

    
    reg &= ~0x3000;

    
    reg |= a_Mode << 12;

    a_Engine->deviceRegisters[0x2] = reg;

    
    res = z_CommandEnginePush(a_Engine, z_CmdNewRegWr(0x2, reg));

    z_CommandEngineUnlock(a_Engine);
    return res;
}


int z_CmdSetDigitalGain(zCommandEngine* a_Engine, int a_Gain)
{
    int         res;
    uint16_t    reg;

    z_CommandEngineLock(a_Engine);

    reg  = a_Engine->deviceRegisters[0x1];

    
    reg &= ~0x00F0;

    
    reg |= (a_Gain & 0xF) << 4;

    a_Engine->deviceRegisters[0x1] = reg;

    
    res = z_CommandEnginePush(a_Engine, z_CmdNewRegWr(0x1, reg));

    z_CommandEngineUnlock(a_Engine);
    return res;

}



int z_CmdSetLedColor(zCommandEngine* a_Engine, int a_Red, int a_Green, int a_Blue)
{
    int newRed;
    int newGreen;
    int newBlue;

    int brightness;

    
    a_Engine->led.r = (uint8_t)a_Red;
    a_Engine->led.g = (uint8_t)a_Green;
    a_Engine->led.b = (uint8_t)a_Blue;

    
    
    if (!(a_Engine->caps & Z_CAP_BRIGHTNESS)) {

        
        brightness = a_Engine->led.brightness;

        
        newRed   = (a_Red   * brightness) / 255;
        newGreen = (a_Green * brightness) / 255;
        newBlue  = (a_Blue  * brightness) / 255;
    }
    
    else {

        newRed   = a_Red;
        newGreen = a_Green;
        newBlue  = a_Blue;
    }

    
    return z_CommandEnginePush(a_Engine, z_CmdNewLedColor((uint8_t)newRed, (uint8_t)newGreen, (uint8_t)newBlue));
}


int z_CmdSetLedBrightness(zCommandEngine* a_Engine, int a_Brightness)
{
    int         res;
    uint16_t    reg;
    int         brightness;

    
    a_Engine->led.brightness = (uint8_t)a_Brightness;

    
    
    if (!(a_Engine->caps & Z_CAP_BRIGHTNESS)) {

        return z_CmdSetLedColor(a_Engine, a_Engine->led.r, a_Engine->led.g, a_Engine->led.b);
    }

    
    else {

        
        brightness = a_Brightness;

        z_CommandEngineLock(a_Engine);

        reg  = a_Engine->deviceRegisters[0x3];

        
        reg &= ~0x00FF;

        
        reg |= (brightness & 0xFF);

        a_Engine->deviceRegisters[0x3] = reg;

        
        res = z_CommandEnginePush(a_Engine, z_CmdNewRegWr(0x3, reg));

        z_CommandEngineUnlock(a_Engine);
        return res;
    }
}


int z_CmdSetTestMode(zCommandEngine* a_Engine, int a_TestMode)
{
    int         res;
    uint16_t    reg;

    z_CommandEngineLock(a_Engine);

    reg  = a_Engine->deviceRegisters[0x2];

    
    reg &= ~0x4000;

    
    if(a_TestMode) {
        reg |= 0x4000;
    }

    a_Engine->deviceRegisters[0x2] = reg;

    
    res = z_CommandEnginePush(a_Engine, z_CmdNewRegWr(0x2, reg));

    z_CommandEngineUnlock(a_Engine);
    return res;
}




int z_CmdGetLedBrightness(zCommandEngine* a_Engine)
{
    
    
    if (!(a_Engine->caps & Z_CAP_BRIGHTNESS)) {
        return (int)a_Engine->led.brightness;

    
    } else {

        return (int)(a_Engine->deviceRegisters[0x3] & 0x00FF);
    }
}


void z_GetAvailableSampleRates(const char* a_Serial, unsigned int* a_SampleFrequencyTable, int* a_SampleFrequencyTableSize) {

    unsigned int sampleRatesInfineon[Z_NUM_OF_FREQ_INFINEON] = {48000};
    unsigned int sampleRatesAnalog[Z_NUM_OF_FREQ_ANALOG] = {16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000};
    unsigned int sampleRatesMEMS[Z_NUM_OF_FREQ_DIGITAL] = {16000, 22050, 24000, 32000, 44100, 48000};
    unsigned int sampleRatesDefault[Z_NUM_OF_FREQ_INFINEON] = {0};

    if((strlen(a_Serial) < 5) || (strlen(a_Serial) == 0) ) {
        memcpy(a_SampleFrequencyTable, sampleRatesDefault, sizeof(unsigned int) * Z_NUM_OF_FREQ_INFINEON);
        (*a_SampleFrequencyTableSize) = 0;
        return;
    }

    if(a_Serial[5] == 'R') {
        memcpy(a_SampleFrequencyTable, sampleRatesInfineon, sizeof(unsigned int) * Z_NUM_OF_FREQ_INFINEON);
        (*a_SampleFrequencyTableSize) = Z_NUM_OF_FREQ_INFINEON;
        return;
    }

    if(a_Serial[4] == 'D') {
        memcpy(a_SampleFrequencyTable, sampleRatesMEMS, sizeof(unsigned int) * Z_NUM_OF_FREQ_DIGITAL);
        (*a_SampleFrequencyTableSize) = Z_NUM_OF_FREQ_DIGITAL;
        return;
    }
    else {
        memcpy(a_SampleFrequencyTable, sampleRatesAnalog, sizeof(unsigned int) * Z_NUM_OF_FREQ_ANALOG);
        (*a_SampleFrequencyTableSize) = Z_NUM_OF_FREQ_ANALOG;
        return;
    }
}

