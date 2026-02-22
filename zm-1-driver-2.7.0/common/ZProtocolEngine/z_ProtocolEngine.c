



#include "z_ProtocolEngine.h"


static int             z_ProtoLock        (zProtoEngine* engine);
static int             z_ProtoUnlock      (zProtoEngine* engine);




static void z_Decode7to8(uint8_t* a_BytesSrc, uint8_t* a_BytesDst)
{
    a_BytesDst[0] = ((a_BytesSrc[1] & 0x01) << 7) | ((a_BytesSrc[0] & 0x7F)     );
    a_BytesDst[1] = ((a_BytesSrc[2] & 0x03) << 6) | ((a_BytesSrc[1] & 0x7E) >> 1);
    a_BytesDst[2] = ((a_BytesSrc[3] & 0x07) << 5) | ((a_BytesSrc[2] & 0x7C) >> 2);
    a_BytesDst[3] = ((a_BytesSrc[4] & 0x0F) << 4) | ((a_BytesSrc[3] & 0x78) >> 3);
    a_BytesDst[4] = ((a_BytesSrc[5] & 0x1F) << 3) | ((a_BytesSrc[4] & 0x70) >> 4);
    a_BytesDst[5] = ((a_BytesSrc[6] & 0x3F) << 2) | ((a_BytesSrc[5] & 0x60) >> 5);
    a_BytesDst[6] = ((a_BytesSrc[7] & 0x7F) << 1) | ((a_BytesSrc[6] & 0x40) >> 6);
}


static void z_Encode7to8(uint8_t* a_BytesSrc, uint8_t* a_BytesDst)
{
    a_BytesDst[0] = ((a_BytesSrc[0] & 0x7F)                                    );
    a_BytesDst[1] = ((a_BytesSrc[1] & 0x3F) << 1) | ((a_BytesSrc[0] & 0x80) >> 7);
    a_BytesDst[2] = ((a_BytesSrc[2] & 0x1F) << 2) | ((a_BytesSrc[1] & 0xC0) >> 6);
    a_BytesDst[3] = ((a_BytesSrc[3] & 0x0F) << 3) | ((a_BytesSrc[2] & 0xE0) >> 5);
    a_BytesDst[4] = ((a_BytesSrc[4] & 0x07) << 4) | ((a_BytesSrc[3] & 0xF0) >> 4);
    a_BytesDst[5] = ((a_BytesSrc[5] & 0x03) << 5) | ((a_BytesSrc[4] & 0xF8) >> 3);
    a_BytesDst[6] = ((a_BytesSrc[6] & 0x01) << 6) | ((a_BytesSrc[5] & 0xFC) >> 2);
    a_BytesDst[7] =                                 ((a_BytesSrc[6] & 0xFE) >> 1);
}


void z_ProtoLog(zProtoEngine* a_Engine, int a_Level, const char* a_Format, ...)
{
	va_list va;

    
    if((a_Engine == NULL) || (a_Engine->verboseLevel < a_Level)) {
		(void)va;
        return;
    }

#if (LINUX_KERNEL_MODULE == 1)

    va_start(va, a_Format);
    printk(KERN_DEBUG "zProtoEngine: ");
    vprintk(a_Format, va);
    va_end(va);

#elif (MACOS_KERNEL_MODULE == 1) || (MACOS_USER_MODULE == 1)
    {
        char msgBuffer[256] = "ZYLIA zProtoEngine: ";
        strncat(msgBuffer, a_Format, 256);

        va_start(va, a_Format);
        IOLogv(msgBuffer, va);
        va_end(va);
        
    }

#elif (WINDOWS_KERNEL_MODULE == 1)

    va_start(va, a_Format);
    vDbgPrintExWithPrefix("zProtoEngine: ", DPFLTR_IHVSTREAMING_ID, DPFLTR_MASK | DPFLTR_TRACE_LEVEL, a_Format, va);
    va_end(va);

#elif (WINDOWS_USER_MODULE == 1)
#ifdef _DEBUG
    {
        char msgFormat[64];
        char msgBuffer[256];

        va_start(va, a_Format);
        sprintf_s(msgFormat, sizeof(msgFormat), "zProtoEngine: %s", a_Format);
        vsprintf_s(msgBuffer, sizeof(msgBuffer), msgFormat, va);
        va_end(va);

        OutputDebugString(msgBuffer);
    }
#endif 
    
#else

    va_start(va, a_Format);
    fprintf(stderr, "zProtoEngine: ");
    vfprintf(stderr, a_Format, va);
    va_end(va);

#endif
}


zProtoEngine* z_ProtoCreate(void) {
    zProtoEngine* engine = NULL;
    (void) engine;

#if (LINUX_KERNEL_MODULE == 1)

    
    if(in_interrupt()) {
        engine = (zProtoEngine*)kzalloc(sizeof(zProtoEngine), GFP_ATOMIC);
    }
    else {
        engine = (zProtoEngine*)kzalloc(sizeof(zProtoEngine), GFP_KERNEL);
    }

    if(engine == NULL) {
        return NULL;
    }

    
    spin_lock_init(&engine->lock);

#elif (MACOS_KERNEL_MODULE == 1)

    
    engine = (zProtoEngine*)IOMalloc(sizeof(zProtoEngine));
    if(engine == NULL) {
        return NULL;
    }

    memset(engine, 0, sizeof(zProtoEngine));

    
    engine->lock = IOSimpleLockAlloc();

    if(engine->lock == NULL) {
        IOFree(engine, sizeof(zProtoEngine));
        return NULL;
    }

#elif (WINDOWS_KERNEL_MODULE == 1)

    
    engine = (zProtoEngine*)ExAllocatePoolWithTag(NonPagedPool, sizeof(zProtoEngine), Z_PROTOCOL_ENGINE_POOL_TAG);
    if(engine == NULL) {
        return NULL;
    }

    RtlZeroMemory((void*)engine, sizeof(zProtoEngine));

    
    KeInitializeSpinLock(&engine->lock);

#elif (WINDOWS_USER_MODULE == 1)

    
    engine = (zProtoEngine*)calloc(1, sizeof(zProtoEngine));
    if(engine == NULL) {
        return NULL;
    }

    
    engine->mutex = CreateMutex(NULL, FALSE, NULL);

#elif (MACOS_USER_MODULE == 1)

    return NULL;
    
    
    
    
#else

    
    engine = (zProtoEngine*)calloc(1, sizeof(zProtoEngine));

#endif

    return engine;
    
}


void z_ProtoInit(zProtoEngine* engine, void* a_UserContext, void* a_UserData) {
    if(engine == NULL) {
        return;
    }

    
    engine->userContext     = a_UserContext;
    engine->userData        = a_UserData;

    
    engine->channelCount    = 32;
    engine->bitsPerSample   = 16;

    z_ProtoReset(engine);

    z_ProtoLog(engine, 0, "Protocol engine initialized.\n");
}


zProtoEngine* z_ProtoDelete(zProtoEngine* a_Engine) {
    if(a_Engine == NULL) {
        return NULL;
    }

#if (LINUX_KERNEL_MODULE == 1)

    
    kfree(a_Engine);

#elif (MACOS_KERNEL_MODULE == 1)

    
    if (a_Engine->lock != NULL) {
        IOSimpleLockFree(a_Engine->lock);
    }

    return NULL; 

#elif (WINDOWS_KERNEL_MODULE == 1)

    ExFreePoolWithTag((void*)a_Engine, Z_PROTOCOL_ENGINE_POOL_TAG);
    

#elif (WINDOWS_USER_MODULE == 1)

    
    CloseHandle(a_Engine->mutex);

    
    return NULL; 

#elif (MACOS_USER_MODULE == 1)

    return NULL; 
    
#else

    
    free(a_Engine);

#endif

    z_ProtoLog(NULL, 0, "Protocol engine freed.\n");

    return NULL;
}




static int z_ProtoLock(zProtoEngine* a_Engine)
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


static int z_ProtoUnlock(zProtoEngine* a_Engine)
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





int z_ProtoReset(zProtoEngine* a_Engine)
{
    if(a_Engine == NULL) {
        return -1;
    }

    a_Engine->rxState       = Z_ENGINE_IDLE;

    a_Engine->rxBlockPtr    = 0;

    a_Engine->rxWord        = 0;
    a_Engine->rxWordPtr     = 0;

    a_Engine->rxSamplePtr   = 0;

    return 0;
}


int z_ProtoResetStreamFormat(zProtoEngine* a_Engine) {
    if (a_Engine == NULL) { return -1; }
    a_Engine->channelCount = 32;
    a_Engine->bitsPerSample = 16;
    return 0;
}


int z_ProtoSetDataCallback(zProtoEngine* a_Engine, zProtoCallback a_Callback)
{
    if(a_Engine == NULL) {
        return -1;
    }

    a_Engine->dataCallback  = a_Callback;

    return 0;
}


int z_ProtoSetCtrlCallback(zProtoEngine* a_Engine, zProtoCallback a_Callback)
{
    if(a_Engine == NULL) {
        return -1;
    }

    a_Engine->ctrlCallback  = a_Callback;

    return 0;
}




int z_ProtoDecode(zProtoEngine* a_Engine, uint8_t* a_Buffer, int a_Length)
{
    size_t      i, j;
    uint8_t     byte;
    uint32_t    word;
    uint8_t     bytesDec[7];

    int         index;

    int         isValid             = 0;
    int         newChannelCount     = 0;
    int         newBitsPerSample    = 0;

    if(a_Engine == NULL) {
        return -1;
    }

    
    for(i=0; i<(unsigned int)a_Length; ++i) {

        
        byte = a_Buffer[i];

        
        if(byte & 0x80) {

            byte &= 0x7F;

            
            a_Engine->rxBlockPtr    = 0;
            a_Engine->rxWord        = 0;
            a_Engine->rxWordPtr     = 0;
            a_Engine->rxSamplePtr   = 0;
            a_Engine->rxState       = Z_ENGINE_CTRL_WORD;

            z_ProtoLog(a_Engine, 3, "Got sync byte\n");
            z_ProtoLog(a_Engine, 3, "state <= Z_ENGINE_CTRL_WORD\n");
        }
        
        else {

            
            if(a_Engine->rxState == Z_ENGINE_IDLE)
                continue;
        }

        
        a_Engine->rxBlock[ a_Engine->rxBlockPtr++ ] = byte;

        
        if(a_Engine->rxBlockPtr >= 8) {

            
            z_Decode7to8(a_Engine->rxBlock, bytesDec);
            a_Engine->rxBlockPtr = 0;

            
            for(j=0; j<7; ++j) {

                
                byte = bytesDec[j];

                
                switch(a_Engine->rxState)
                {
                case Z_ENGINE_CTRL_WORD:    

                    
                    a_Engine->rxWord >>= 8;
                    a_Engine->rxWord  |= (byte << 24);

                    
                    if(++(a_Engine->rxWordPtr) >= 4) {

                        
                        if(a_Engine->rxWord & 0x80000000) {

                            z_ProtoLog(a_Engine, 2, "Rx CTRL: 0x%08X\n", a_Engine->rxWord);

                            if(a_Engine->ctrlCallback)
                                a_Engine->ctrlCallback(a_Engine, (void*)&a_Engine->rxWord, 1);
                        }

                        
                        else if((a_Engine->rxWord & 0xC0000000) == 0x40000000) {

                            z_ProtoLog(a_Engine, 3, "Stream header: 0x%08X\n", a_Engine->rxWord);

                            
                            isValid             = 1;
                            newChannelCount     = (a_Engine->rxWord & 0x000000FF) + 1;
                            newBitsPerSample    = (a_Engine->rxWord & 0x00000100) ? 24 : 16;

                            

                            
                            if((a_Engine->rxWord & 0x3FFFFE00) != 0)  
                                isValid = 0;
                            if(newChannelCount < 1 || newChannelCount > Z_ENGINE_MAX_CHANNELS)
                                isValid = 0;

                            
                            if(isValid) {
                                if(a_Engine->channelCount != newChannelCount || a_Engine->bitsPerSample != newBitsPerSample) {

                                    a_Engine->channelCount    = newChannelCount;
                                    a_Engine->bitsPerSample   = newBitsPerSample;

                                    z_ProtoLog(a_Engine, 1, "New format: %d channels, %d bps\n", a_Engine->channelCount, a_Engine->bitsPerSample);
                                }
                            }
                        }
                        else
                            z_ProtoLog(a_Engine, 4, "CTRL: 0x%08X\n", a_Engine->rxWord);

                        
                        if( a_Engine->channelCount  == 0 ||
                            a_Engine->bitsPerSample == 0)
                        {
                            a_Engine->rxState = Z_ENGINE_IDLE;
                            break;
                        }

                        
                        a_Engine->rxWord      = 0;
                        a_Engine->rxWordPtr   = 0;
                        a_Engine->rxSamplePtr = 0;
                        a_Engine->rxState     = Z_ENGINE_DATA_MSB;

                        z_ProtoLog(a_Engine, 3, "state <= Z_ENGINE_CTRL_MSB\n");
                    }

                    break;

                case Z_ENGINE_DATA_MSB:     

                    
                    a_Engine->rxWord >>= 8;
                    a_Engine->rxWord  |= (byte << 8);

                    
                    a_Engine->rxWordPtr++;
                    if(a_Engine->rxWordPtr >= 2) {

                        
                        if(a_Engine->rxWord & 0x00008000)
                            a_Engine->rxWord |= 0xFFFF0000;

                        z_ProtoLog(a_Engine, 4, "[%d/%d], %d\n", a_Engine->rxSamplePtr, a_Engine->channelCount, a_Engine->rxWord);

                        
                        
                        
                        
                        
                        index = a_Engine->rxSamplePtr;

                        
                        a_Engine->rxSamples[index] = a_Engine->rxWord;
                        a_Engine->rxSamplePtr++;

                        
                        a_Engine->rxWord      = 0;
                        a_Engine->rxWordPtr   = 0;

                        
                        if(a_Engine->rxSamplePtr >= a_Engine->channelCount) {

                            
                            if(a_Engine->bitsPerSample == 16) {

                                
                                if(a_Engine->dataCallback)
                                    a_Engine->dataCallback(a_Engine, (void*)a_Engine->rxSamples, a_Engine->channelCount);

                                
                                a_Engine->rxSamplePtr = 0;
                            }
                            
                            else if(a_Engine->bitsPerSample == 24) {

                                
                                a_Engine->rxSamplePtr = 0;
                                a_Engine->rxState     = Z_ENGINE_DATA_LSB;

                                z_ProtoLog(a_Engine, 3, "state <= Z_ENGINE_CTRL_LSB\n");
                            }
                        }
                    }

                    break;

                case Z_ENGINE_DATA_LSB:     

                    
                    

                    
                    
                    
                    
                    
                    index = a_Engine->rxSamplePtr ^ 2;

                    
                    word  = a_Engine->rxSamples[index];

                    
                    word <<= 8;
                    word  |= byte;

                    
                    a_Engine->rxSamples[index] = word;
                    a_Engine->rxSamplePtr++;

                    
                    if(a_Engine->rxSamplePtr >= a_Engine->channelCount) {

                        
                        if(a_Engine->dataCallback)
                            a_Engine->dataCallback(a_Engine, (void*)a_Engine->rxSamples, a_Engine->channelCount);

                        
                        a_Engine->rxSamplePtr = 0;
                        a_Engine->rxState     = Z_ENGINE_DATA_MSB;

                        z_ProtoLog(a_Engine, 3, "state <= Z_ENGINE_CTRL_MSB\n");
                    }

                    break;
                }
            }
        }
    }

    return 0;
}




int z_ProtoEncodeCtrl(zProtoEngine* a_Engine, uint32_t a_CtrlWord, uint8_t* a_Buffer, int* a_Length)
{
    uint8_t     bytesEnc[7];
    uint8_t     bytesRaw[8];

    
    bytesEnc[0] = 0;
    bytesEnc[1] = 0;
    bytesEnc[2] = 0;
    bytesEnc[3] = 0;
    bytesEnc[4] = 0;
    bytesEnc[5] = 0;
    bytesEnc[6] = 0;

    z_Encode7to8(bytesEnc, bytesRaw);
    memcpy(&a_Buffer[0], bytesRaw, 8);

    
    bytesEnc[0] = (uint8_t)((a_CtrlWord & 0x000000FF)      );
    bytesEnc[1] = (uint8_t)((a_CtrlWord & 0x0000FF00) >>  8);
    bytesEnc[2] = (uint8_t)((a_CtrlWord & 0x00FF0000) >> 16);
    bytesEnc[3] = (uint8_t)((a_CtrlWord & 0xFF000000) >> 24);

    
    bytesEnc[4] = 0;
    bytesEnc[5] = 0;
    bytesEnc[6] = 0;

    z_Encode7to8(bytesEnc, bytesRaw);
    bytesRaw[0] |= 0x80;
    memcpy(&a_Buffer[8], bytesRaw, 8);

    (*a_Length) = 16;

    z_ProtoLog(a_Engine, 2, "Tx CTRL: 0x%08X\n", a_CtrlWord);

    return 0;
}

