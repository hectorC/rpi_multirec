



#include "z_CommandEngineWrapper.hpp"

namespace z {

CommandEngine::CommandEngine(void* a_UserData, ICommandEngineCallback* a_Callback) {
    
    m_Engine = new zCommandEngine;
    z_CommandEngineInit(m_Engine, (void*)this, a_UserData);

    
    z_CommandEngineSetNotificationCallback(m_Engine, CommandEngine::_NotificationCallback);

    
    m_Callback = a_Callback;

}

CommandEngine::~CommandEngine() {
    delete m_Engine;
}

int CommandEngine::_NotificationCallback(zCommandEngine* a_Engine, int32_t a_Code, void* a_Data)
{
    
    CommandEngine* classPtr = (CommandEngine*)a_Engine->userContext;

    
    if(classPtr == NULL) {
        return -1;
    }

    
    ICommandEngineCallback* ifacePtr = classPtr->m_Callback;

    
    if(ifacePtr == NULL) {
        return -1;
    }

    
    return ifacePtr->NotificationCallback(a_Code, a_Data);
}

int CommandEngine::Process(uint32_t* a_CtrlWord) {
    return z_CommandEngineProcess(m_Engine, a_CtrlWord);
}

int CommandEngine::Receive(uint32_t a_CtrlWord) {
    return z_CommandEngineReceive(m_Engine, a_CtrlWord);
}

void CommandEngine::clearVersionRom() {
    z_CmdClearVersionROM(m_Engine);
}

int CommandEngine::readVersionRom() {
    return z_CmdReadVersionROM(m_Engine);
}

bool CommandEngine::isVersionDataValid() const {
    return (m_Engine->versionRom.isValid != 0);
}

const char* CommandEngine::getVersionString() const {
    return m_Engine->versionRom.string;
}

uint8_t CommandEngine::getDeviceCaps() const {
    return m_Engine->caps;
}

int CommandEngine::ReadRegisters() {
    return z_CmdReadRegisters(m_Engine);
}

bool CommandEngine::isPending() const {
    return z_CommandEngineIsPending(m_Engine);
}

int CommandEngine::getPendingCommandNum() const {
    return m_Engine->commandQueue->occupancy;
}

int CommandEngine::EnableStreaming(bool a_Enable) {
    return z_CmdEnableStreaming(m_Engine, (int)a_Enable);
}

int CommandEngine::SetSampleRate(int a_SampleRate)
{
    return z_CmdSetSampleRate(m_Engine, a_SampleRate);
}

int CommandEngine::SetSampleFormat(int a_Format)
{
    return z_CmdSetSampleFormat(m_Engine, a_Format);
}

int CommandEngine::SetChannelCount(int a_Count)
{
    return z_CmdSetChannelCount(m_Engine, a_Count);
}

int CommandEngine::SetAveragingMode(int a_Mode)
{
    return z_CmdSetAveragingMode(m_Engine, a_Mode);
}

int CommandEngine::SetDigitalGain(int a_Gain)
{
    return z_CmdSetDigitalGain(m_Engine, a_Gain);
}

int CommandEngine::SetLedColor(int a_Red, int a_Green, int a_Blue)
{
    return z_CmdSetLedColor(m_Engine, a_Red, a_Green, a_Blue);
}

int CommandEngine::SetLedBrightness(int a_Brightness)
{
    return z_CmdSetLedBrightness(m_Engine, a_Brightness);
}


int CommandEngine::GetLedBrightness()
{
    return z_CmdGetLedBrightness(m_Engine);
}

void CommandEngine::setTestMode(bool a_Enable) {
    z_CmdSetTestMode(m_Engine, a_Enable);
}

void CommandEngine::GetAvailableSampleRates(const char* a_Serial, unsigned int* a_SampleFrequencyTable, int* a_SampleFrequencyTableSize) {
    z_GetAvailableSampleRates(a_Serial, a_SampleFrequencyTable, a_SampleFrequencyTableSize);
}

} 

