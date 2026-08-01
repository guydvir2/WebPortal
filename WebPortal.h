#ifndef WebPortal_h
#define WebPortal_h

// WebPortal v0.2 — self-contained web config portal for myIOT2 devices.
// Serves a single gzipped page from flash. Decoupled from myIOT2 via callbacks.
// No internet dependency — works in AP mode or local network.

#if defined(ESP8266)
#include <ESP8266WebServer.h>
typedef ESP8266WebServer WebServerImpl;
#elif defined(ESP32)
#include <WebServer.h>
typedef WebServer WebServerImpl;
#endif

#include <Arduino.h>
#include <stdint.h>
#include <myJflash.h>
#include "SerialCapture/SerialCapture.h"

// Override PRNT/PRNTL to also feed SerialCapture
#ifdef PRNT
#undef PRNT
#endif
#ifdef PRNTL
#undef PRNTL
#endif
#define PRNT(a)  do { if (useSerial) Serial.print(a);   SerialCapture::append(a);     } while (0)
#define PRNTL(a) do { if (useSerial) Serial.println(a); SerialCapture::appendLine(a); } while (0)

// ~~~ Config read shape — secrets replaced with *Set booleans, never echoed back ~~~
struct WebPortalConfig
{
    char deviceName[32]{};
    char timezone[48]{};
    char ssid[32]{};
    char mqttHost[40]{};
    uint16_t mqttPort = 1883;
    char mqttUser[32]{};
    bool wifiPwdSet = false;
    bool mqttPwdSet = false;
    bool useSerial = true;
    bool otaEnabled = false;
    bool terminalEnabled = false;
    bool resetSafetyEnabled = false;
    uint8_t resetSafetyThreshold = 3;
    bool ignoreBootMsg = false;
    bool useFlashP = false;
    uint8_t noNetworkResetMinutes = 4;
    char topicPubAvail[32]{};
    char topicPubState[32]{};
    char topicSubCmd[32]{};
    char topicGenMessages[32]{};
    char topicGenLog[32]{};
    char topicGenDebug[32]{};
    bool topicsReady = false;
    char extraPub[5][32]{};
    char extraSub[5][32]{};
};

// ~~~ Config write shape — password fields empty = leave unchanged ~~~
struct WebPortalConfigUpdate
{
    char deviceName[32]{};
    char timezone[48]{};
    char ssid[32]{};
    char wifiPwd[64]{};
    char mqttHost[40]{};
    uint16_t mqttPort = 1883;
    char mqttUser[32]{};
    char mqttPwd[64]{};
    bool useSerial = true;
    bool otaEnabled = false;
    bool terminalEnabled = false;
    bool resetSafetyEnabled = false;
    uint8_t resetSafetyThreshold = 3;
    bool ignoreBootMsg = false;
    bool useFlashP = false;
    uint8_t noNetworkResetMinutes = 4;
    char topicPubAvail[32]{};
    char topicPubState[32]{};
    char topicSubCmd[32]{};
    char topicGenMessages[32]{};
    char topicGenLog[32]{};
    char topicGenDebug[32]{};
    char extraPub[5][32]{};
    char extraSub[5][32]{};
};

// ~~~ Button definition — set via portal.setButton() before begin() ~~~
struct WebPortalButton
{
    char label[20]{};
    bool isToggle = false;     // false = momentary, true = on/off toggle
    bool toggleState = false;
    bool enabled = false;      // false = greyed out
};

typedef void (*ButtonCallback)(uint8_t index, bool state);

// ~~~ Live status — polled every few seconds by the browser ~~~
struct WebPortalStatus
{
    bool wifiConnected = false;
    bool mqttConnected = false;
    bool ntpSynced = false;
    bool otaActive = false;
    char deviceId[24]{};
    char primaryTopic[64]{};
    bool ignoreBootMsg = false;
    bool useFlashP = false;
    uint8_t noNetworkResetMinutes = 0;
    char espType[10]{};
    bool resetSafetyEnabled = false;
    uint8_t resetSafetyCounter = 0;
    bool resetSafetyBootWasNormal = true;

    // Custom read-only rows shown in the Device Data section.
    // Leave label empty to skip that slot.
    static const uint8_t CUSTOM_STATUS_SLOTS = 4;
    char customLabel[CUSTOM_STATUS_SLOTS][20]{};
    char customValue[CUSTOM_STATUS_SLOTS][32]{};

    // Buttons shown in the Controls section.
    static const uint8_t BUTTON_COUNT = 4;
    WebPortalButton buttons[BUTTON_COUNT]{};
};

class WebPortal
{
public:
    typedef bool   (*ConfigGetter)(WebPortalConfig &out);
    typedef bool   (*ConfigSetter)(const WebPortalConfigUpdate &in, char *errMsg, size_t errLen);
    typedef void   (*StatusGetter)(WebPortalStatus &out);
    typedef String (*LogGetter)(void);
    typedef void   (*ResetRequester)(void);
    typedef bool   (*FileDeleter)(void);    // returns false if delete failed
    typedef void   (*APStarter)(void);

    static const char* version()  { return "0.3 (2026-08-01)"; }

    // Register a button (index 0..3) before calling begin().
    void setButton(uint8_t index, const char *label, bool isToggle, ButtonCallback cb);

    void begin(ConfigGetter getCfg, ConfigSetter setCfg, StatusGetter getStatus,
               LogGetter getLog = nullptr, ResetRequester requestReset = nullptr,
               FileDeleter deleteConfig = nullptr, FileDeleter deleteTopics = nullptr,
               APStarter startAP = nullptr, uint16_t port = 80);
    void handle();  // call every loop() iteration

    inline bool isRunning() const { return _running; }

private:
    WebServerImpl *_server = nullptr;
    ConfigGetter  _getCfg = nullptr;
    ConfigSetter  _setCfg = nullptr;
    StatusGetter  _getStatus = nullptr;
    LogGetter     _getLog = nullptr;
    ResetRequester _requestReset = nullptr;
    FileDeleter _deleteConfig = nullptr;
    FileDeleter _deleteTopics = nullptr;
    APStarter   _startAP = nullptr;
    ButtonCallback _buttonCb[4]{nullptr, nullptr, nullptr, nullptr};
    WebPortalButton _buttons[4]{};
    bool _running = false;
    bool _terminalEnabled = false;

    static const size_t MAX_BODY_LEN = 1536;

    void _handleRoot();
    void _handleConfigGet();
    void _handleConfigPost();
    void _handleStatusGet();
    void _handleLogGet();
    void _handleResetPost();
    void _handleButtonPost();
    void _handleDeleteConfig();
    void _handleDeleteTopics();
    void _handleStartAP();
    void _sendJsonError(int code, const char *msg);
    bool _loadTerminalEnabled();
    void _saveTerminalEnabled(bool value);
};

#endif