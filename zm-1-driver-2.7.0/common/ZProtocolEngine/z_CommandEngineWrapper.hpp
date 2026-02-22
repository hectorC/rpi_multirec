




#ifndef Z_COMMAND_ENGINE_HH
#define Z_COMMAND_ENGINE_HH

#include "z_CommandEngine.h"

namespace z {



class ICommandEngineCallback {
public:
    virtual int NotificationCallback(int32_t a_Code, void* a_Data) = 0;
    virtual ~ICommandEngineCallback() = default;
};



class CommandEngine {
public:

    CommandEngine(void* a_UserData=NULL, ICommandEngineCallback* a_Callback=NULL);
    ~CommandEngine();

    void SetUserData(void* a_UserData) {
        if (m_Engine == NULL) return;
        m_Engine->userData = a_UserData;
    }
    void SetVerboseLevel (int a_Level) {
        if (m_Engine == NULL) return;
        m_Engine->verboseLevel = a_Level;
    }

    
    int                     Process         (uint32_t* a_CtrlWord);
    int                     Receive         (uint32_t  a_CtrlWord);
    bool isPending() const;
    int getPendingCommandNum() const;

    
    void clearVersionRom();
    int                     readVersionRom      ();
    bool                    isVersionDataValid  () const;
    const char*             getVersionString    () const;
    uint8_t                 getDeviceCaps       () const;

    
    int                     ReadRegisters   ();

    
    int                     EnableStreaming (bool a_Enable);
    int                     SetSampleRate   (int a_SampleRate);
    int                     SetSampleFormat (int a_Format);
    int                     SetChannelCount (int a_Count);
    int                     SetAveragingMode(int a_Mode);
    int                     SetDigitalGain  (int a_Gain);
    int                     SetLedColor     (int a_Red, int a_Green, int a_Blue);
    int                     SetLedBrightness(int a_Brightness);

    int                     GetLedBrightness();

    void setTestMode(bool a_Enable);

    
    static void GetAvailableSampleRates(const char* a_Serial, unsigned int* a_SampleFrequencyTable, int* a_SampleFrequencyTableSize);
    
protected:

    
    zCommandEngine*         m_Engine;

    
    ICommandEngineCallback* m_Callback;

private:

    
    static int              _NotificationCallback   (zCommandEngine* a_Engine, int32_t a_Code, void* a_Data);
};

}
#endif

