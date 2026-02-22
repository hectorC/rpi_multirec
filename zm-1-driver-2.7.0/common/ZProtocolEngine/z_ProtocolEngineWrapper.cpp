




#include "z_ProtocolEngineWrapper.hpp"


namespace z {

ProtocolEngine::ProtocolEngine(void* a_UserData, IProtocolEngineCallback* a_Callback)
    : m_Callback(a_Callback)
{
    m_Engine = new zProtoEngine;
    z_ProtoInit(m_Engine, this, a_UserData);
    
    
    z_ProtoSetDataCallback(m_Engine, ProtocolEngine::_DataCallback);
    z_ProtoSetCtrlCallback(m_Engine, ProtocolEngine::_CtrlCallback);
}

ProtocolEngine::~ProtocolEngine() {
    z_ProtoDelete(m_Engine);
}

int ProtocolEngine::_DataCallback(zProtoEngine* a_Engine, void* a_Data, int a_Count) {
    
    ProtocolEngine* classPtr = (ProtocolEngine*)a_Engine->userContext;

    
    if(classPtr == NULL) {
        return -1;
    }

    
    IProtocolEngineCallback* ifacePtr = classPtr->m_Callback;

    
    if(ifacePtr == NULL) {
        return -1;
    }

    
    return ifacePtr->DataCallback(a_Data, a_Count);
}

int ProtocolEngine::_CtrlCallback(zProtoEngine* a_Engine, void* a_Data, int a_Count) {
    
    ProtocolEngine* classPtr = (ProtocolEngine*)a_Engine->userContext;

    
    if(classPtr == NULL) {
        return -1;
    }

    
    IProtocolEngineCallback* ifacePtr = classPtr->m_Callback;

    
    if(ifacePtr == NULL) {
        return -1;
    }

    
    return ifacePtr->CtrlCallback(a_Data, a_Count);
}

int ProtocolEngine::Reset() {
    return z_ProtoReset(m_Engine);
}

int ProtocolEngine::Decode(uint8_t* a_Buffer, int a_Length) {
    return z_ProtoDecode(m_Engine, a_Buffer, a_Length);
}

int ProtocolEngine::EncodeCtrl(uint32_t a_CtrlWord, uint8_t* a_Buffer, int* a_Length) {
    return z_ProtoEncodeCtrl(m_Engine, a_CtrlWord, a_Buffer, a_Length);
}


}

