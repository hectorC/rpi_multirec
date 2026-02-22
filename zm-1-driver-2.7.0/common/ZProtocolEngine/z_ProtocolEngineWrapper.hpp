




#ifndef Z_PROTOCOL_ENGINE_HH
#define Z_PROTOCOL_ENGINE_HH

#include "z_ProtocolEngine.h"

#ifndef NULL
#define NULL nullptr
#endif

namespace z {



class IProtocolEngineCallback {
public:

    
    virtual int DataCallback(void* a_Data, int a_Count) = 0;
    virtual int CtrlCallback(void* a_Data, int a_Count) = 0;

    virtual ~IProtocolEngineCallback() = default;
};



class ProtocolEngine final {

public:

    ProtocolEngine(void* a_UserData=NULL, IProtocolEngineCallback* a_Callback=NULL);
    ~ProtocolEngine();

    
    inline void             SetUserData         (void* a_UserData);
    inline void             SetVerboseLevel     (int a_Level);

    inline void             ResetStreamFormat   ();
    inline int              GetChannelCount     ();
    inline int              GetBitsPerSample    ();

    int                     Reset               ();

    int                     Decode              (uint8_t* a_Buffer, int a_Length);
    int                     EncodeCtrl          (uint32_t a_CtrlWord, uint8_t* a_Buffer, int* a_Length);

private:

    static int _DataCallback(zProtoEngine* a_Engine, void* a_Data, int a_Count);
    static int _CtrlCallback(zProtoEngine* a_Engine, void* a_Data, int a_Count);

    
    zProtoEngine* m_Engine;

    
    IProtocolEngineCallback* const m_Callback;

};

void ProtocolEngine::SetUserData(void* a_UserData) {
    
    if(m_Engine != NULL) {
        m_Engine->userData = a_UserData;
    }
}

void ProtocolEngine::SetVerboseLevel(int a_Level) {
    
    if(m_Engine != NULL) {
        m_Engine->verboseLevel = a_Level;
    }
}

void ProtocolEngine::ResetStreamFormat() {
    z_ProtoResetStreamFormat(m_Engine);
}

int ProtocolEngine::GetChannelCount() {
    return m_Engine->channelCount;
}

int ProtocolEngine::GetBitsPerSample() {
    return m_Engine->bitsPerSample;
}

}
#endif

