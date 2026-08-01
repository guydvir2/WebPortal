#include "WebPortal.h"
#include "SerialCapture/SerialCapture.h"
#include "config_page_gz.h"
#include <ArduinoJson.h>

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// setButton — call before begin() to register a button slot
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
void WebPortal::setButton(uint8_t index, const char *label, bool isToggle, ButtonCallback cb)
{
    if (index >= 4) return;
    strlcpy(_buttons[index].label, label, sizeof(_buttons[index].label));
    _buttons[index].isToggle = isToggle;
    _buttons[index].enabled  = (cb != nullptr);
    _buttonCb[index]         = cb;
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// begin / handle
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

void WebPortal::begin(ConfigGetter getCfg, ConfigSetter setCfg, StatusGetter getStatus,
                       LogGetter getLog, ResetRequester requestReset,
                       FileDeleter deleteConfig, FileDeleter deleteTopics,
                       APStarter startAP, uint16_t port)
{
    if (_running)
        return; // idempotent — calling begin() twice is a no-op, not a leak

    _getCfg = getCfg;
    _setCfg = setCfg;
    _getStatus = getStatus;
    _getLog = getLog;
    _requestReset = requestReset;
    _deleteConfig = deleteConfig;
    _deleteTopics = deleteTopics;
    _startAP = startAP;

    _server = new WebServerImpl(port);

    _server->on("/", HTTP_GET, [this]()
                { _handleRoot(); });
    _server->on("/config", HTTP_GET, [this]()
                { _handleConfigGet(); });
    _server->on("/config", HTTP_POST, [this]()
                { _handleConfigPost(); });
    _server->on("/status", HTTP_GET, [this]()
                { _handleStatusGet(); });
    _server->on("/log", HTTP_GET, [this]()
                { _handleLogGet(); });
    _server->on("/reset", HTTP_POST, [this]()
                { _handleResetPost(); });
    _server->on("/button", HTTP_POST, [this]()
                { _handleButtonPost(); });
    _server->on("/delete-config", HTTP_POST, [this]()
                { _handleDeleteConfig(); });
    _server->on("/delete-topics", HTTP_POST, [this]()
                { _handleDeleteTopics(); });
    _server->on("/start-ap", HTTP_POST, [this]()
                { _handleStartAP(); });

    _server->onNotFound([this]()
                         { _server->send(404, "text/plain", "not found"); });

    // Start SerialCapture if terminal is enabled in saved config
    WebPortalConfig cfg;
    if (_getCfg && _getCfg(cfg))
    {
        _terminalEnabled = cfg.terminalEnabled;
        if (_terminalEnabled)
        {
            SerialCapture::enabled = true;
            SerialCapture::begin();
            SerialCapture::restoreFromRTC();
        }
    }

    _server->begin();
    _running = true;
}

void WebPortal::handle()
{
    if (_running)
        _server->handleClient();
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GET /  — the page itself, served gzipped straight from flash
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

void WebPortal::_handleRoot()
{
    _server->sendHeader("Content-Encoding", "gzip");
    _server->sendHeader("Cache-Control", "no-cache"); // was max-age=86400 — too aggressive
                                                        // while the page itself is still changing
                                                        // frequently; browser re-validates every
                                                        // load instead of serving a stale copy
    _server->send_P(200, "text/html", (const char *)CONFIG_PAGE_GZ, CONFIG_PAGE_GZ_LEN);
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GET /config  — current config, secrets replaced with *Set booleans (FR-31)
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

void WebPortal::_handleConfigGet()
{
    if (!_getCfg)
    {
        _sendJsonError(500, "config getter not wired up");
        return;
    }

    WebPortalConfig cfg;
    if (!_getCfg(cfg))
    {
        _sendJsonError(500, "failed to read device config");
        return;
    }

    JsonDocument doc; // ArduinoJson v7 — auto-sized, no capacity guessing (fixes the FR-19 issue)
    doc["deviceName"] = cfg.deviceName;
    doc["timezone"] = cfg.timezone;
    doc["ssid"] = cfg.ssid;
    doc["wifiPwdSet"] = cfg.wifiPwdSet;
    doc["mqttHost"] = cfg.mqttHost;
    doc["mqttPort"] = cfg.mqttPort;
    doc["mqttUser"] = cfg.mqttUser;
    doc["mqttPwdSet"] = cfg.mqttPwdSet;
    doc["useSerial"] = cfg.useSerial;
    doc["otaEnabled"] = cfg.otaEnabled;
    doc["terminalEnabled"] = _terminalEnabled;
    doc["resetSafetyEnabled"] = cfg.resetSafetyEnabled;
    doc["resetSafetyThreshold"] = cfg.resetSafetyThreshold;
    doc["ignoreBootMsg"] = cfg.ignoreBootMsg;
    doc["useFlashP"] = cfg.useFlashP;
    doc["noNetworkResetMinutes"] = cfg.noNetworkResetMinutes;
    doc["topicPubAvail"] = cfg.topicPubAvail;
    doc["topicPubState"] = cfg.topicPubState;
    doc["topicSubCmd"] = cfg.topicSubCmd;
    doc["topicGenMessages"] = cfg.topicGenMessages;
    doc["topicGenLog"] = cfg.topicGenLog;
    doc["topicGenDebug"] = cfg.topicGenDebug;
    doc["topicsReady"] = cfg.topicsReady;
    JsonArray ePub = doc["extraPub"].to<JsonArray>();
    JsonArray eSub = doc["extraSub"].to<JsonArray>();
    for (uint8_t i = 0; i < 5; i++) { ePub.add(cfg.extraPub[i]); eSub.add(cfg.extraSub[i]); }

    String out;
    serializeJson(doc, out);
    _server->send(200, "application/json", out);
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GET /status  — live state for the LED rail, polled every few seconds
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

void WebPortal::_handleStatusGet()
{
    if (!_getStatus)
    {
        _sendJsonError(500, "status getter not wired up");
        return;
    }

    WebPortalStatus st;
    _getStatus(st);

    JsonDocument doc;
    doc["wifiConnected"] = st.wifiConnected;
    doc["mqttConnected"] = st.mqttConnected;
    doc["ntpSynced"] = st.ntpSynced;
    doc["otaActive"] = st.otaActive;
    doc["deviceId"] = st.deviceId;
    doc["primaryTopic"] = st.primaryTopic;
    doc["ignoreBootMsg"] = st.ignoreBootMsg;
    doc["useFlashP"] = st.useFlashP;
    doc["noNetworkResetMinutes"] = st.noNetworkResetMinutes;
    doc["espType"] = st.espType;
    doc["resetSafetyEnabled"] = st.resetSafetyEnabled;
    doc["resetSafetyCounter"] = st.resetSafetyCounter;
    doc["resetSafetyBootWasNormal"] = st.resetSafetyBootWasNormal;
    doc["portalVersion"] = version();

    JsonArray custom = doc["customStatus"].to<JsonArray>();
    for (uint8_t i = 0; i < WebPortalStatus::CUSTOM_STATUS_SLOTS; i++)
    {
        if (st.customLabel[i][0] == '\0')
            continue; // empty label — slot unused, skip it
        JsonObject row = custom.add<JsonObject>();
        row["label"] = st.customLabel[i];
        row["value"] = st.customValue[i];
    }

    // Buttons — send label, isToggle, toggleState, enabled for each slot
    JsonArray btns = doc["buttons"].to<JsonArray>();
    for (uint8_t i = 0; i < 4; i++) {
        JsonObject b = btns.add<JsonObject>();
        b["label"]       = _buttons[i].label;
        b["isToggle"]    = _buttons[i].isToggle;
        b["toggleState"] = _buttons[i].toggleState;
        b["enabled"]     = _buttons[i].enabled;
    }

    String out;
    serializeJson(doc, out);
    _server->send(200, "application/json", out);
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// GET /log  — captured serial output, plain text (not JSON — this is meant
// to read like an actual terminal, not be parsed as structured data).
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

void WebPortal::_handleLogGet()
{
    if (!_getLog)
    {
        _server->send(200, "text/plain", "(terminal not wired up on this device)");
        return;
    }
    String log = _getLog();
    if (log.length() == 0)
    {
        _server->send(200, "text/plain", "(terminal capture is off — enable it in Device Behavior, then reboot)");
        return;
    }
    _server->send(200, "text/plain", log);
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// POST /reset  — reboot the device. Response is sent BEFORE the reset
// callback runs, so the browser sees a clean 200 instead of a connection
// dropped mid-request.
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

void WebPortal::_handleResetPost()
{
    if (!_requestReset)
    {
        _sendJsonError(500, "reset not wired up on this device");
        return;
    }
    _server->send(200, "application/json", "{\"ok\":true,\"message\":\"rebooting\"}");
    delay(300); // give the TCP stack a moment to actually flush the response
                // before the device goes down
    _requestReset(); // expected not to return (e.g. wraps ESP.reset())
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// POST /config  — the only write path. Validates before ever touching flash.
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

void WebPortal::_handleConfigPost()
{
    if (!_setCfg)
    {
        _sendJsonError(500, "config setter not wired up");
        return;
    }

    String body = _server->arg("plain");

    // NFR-1 equivalent for this path: reject oversized/garbage input before
    // it ever reaches the JSON parser or a fixed-size buffer.
    if (body.length() == 0 || body.length() > MAX_BODY_LEN)
    {
        _sendJsonError(400, "request body missing or too large");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err)
    {
        _sendJsonError(400, "malformed JSON");
        return;
    }

    // Required fields present + roughly the right shape. This is intentionally
    // conservative — anything odd gets rejected rather than guessed at, per
    // the "config that fails safe" design principle.
    if (!doc["ssid"].is<const char *>() || !doc["mqttHost"].is<const char *>())
    {
        _sendJsonError(400, "missing required field: ssid or mqttHost");
        return;
    }
    if (!doc["topicPubAvail"].is<const char *>() || !doc["topicSubCmd"].is<const char *>())
    {
        _sendJsonError(400, "missing required field: topicPubAvail or topicSubCmd");
        return;
    }

    WebPortalConfigUpdate upd;

    const char *deviceName = doc["deviceName"] | ""; // optional, no validation beyond length
    const char *timezone = doc["timezone"] | "";     // optional, opaque POSIX TZ string
    const char *ssid = doc["ssid"] | "";
    const char *mqttHost = doc["mqttHost"] | "";
    const char *mqttUser = doc["mqttUser"] | "";
    const char *wifiPwd = doc["wifiPwd"] | "";   // absent/omitted => "" => unchanged
    const char *mqttPwd = doc["mqttPwd"] | "";   // same

    const char *topicPubAvail = doc["topicPubAvail"] | "";
    const char *topicPubState = doc["topicPubState"] | "";
    const char *topicSubCmd = doc["topicSubCmd"] | "";
    const char *topicGenMessages = doc["topicGenMessages"] | "";
    const char *topicGenLog = doc["topicGenLog"] | "";
    const char *topicGenDebug = doc["topicGenDebug"] | "";

    if (strlen(ssid) == 0 || strlen(ssid) >= sizeof(upd.ssid))
    {
        _sendJsonError(400, "ssid missing or too long");
        return;
    }
    if (strlen(mqttHost) == 0 || strlen(mqttHost) >= sizeof(upd.mqttHost))
    {
        _sendJsonError(400, "mqttHost missing or too long");
        return;
    }
    if (strlen(wifiPwd) >= sizeof(upd.wifiPwd) || strlen(mqttPwd) >= sizeof(upd.mqttPwd))
    {
        _sendJsonError(400, "password too long");
        return;
    }
    if (strlen(mqttUser) >= sizeof(upd.mqttUser))
    {
        _sendJsonError(400, "mqttUser too long");
        return;
    }
    if (strlen(deviceName) >= sizeof(upd.deviceName))
    {
        _sendJsonError(400, "deviceName too long");
        return;
    }
    if (strlen(timezone) >= sizeof(upd.timezone))
    {
        _sendJsonError(400, "timezone too long");
        return;
    }
    // sizeof(upd.topicXxx) == 32 — >= catches a 31-char string that would
    // fill the buffer with no room for the null terminator.
    if (strlen(topicPubAvail) == 0 || strlen(topicPubAvail) >= sizeof(upd.topicPubAvail))
    {
        _sendJsonError(400, "topicPubAvail missing or too long (max 31 chars)");
        return;
    }
    if (strlen(topicSubCmd) == 0 || strlen(topicSubCmd) >= sizeof(upd.topicSubCmd))
    {
        _sendJsonError(400, "topicSubCmd missing or too long (max 31 chars)");
        return;
    }
    if (strlen(topicPubState) >= sizeof(upd.topicPubState) ||
        strlen(topicGenMessages) >= sizeof(upd.topicGenMessages) ||
        strlen(topicGenLog) >= sizeof(upd.topicGenLog) ||
        strlen(topicGenDebug) >= sizeof(upd.topicGenDebug))
    {
        _sendJsonError(400, "one or more optional topic fields too long (max 31 chars)");
        return;
    }

    long port = doc["mqttPort"] | 1883;
    if (port < 1 || port > 65535)
    {
        _sendJsonError(400, "mqttPort out of range");
        return;
    }

    long resetSafetyThreshold = doc["resetSafetyThreshold"] | 3;
    if (resetSafetyThreshold < 1 || resetSafetyThreshold > 20)
    {
        _sendJsonError(400, "resetSafetyThreshold out of range (1-20)");
        return;
    }
    long noNetworkResetMinutes = doc["noNetworkResetMinutes"] | 4;
    if (noNetworkResetMinutes < 0 || noNetworkResetMinutes > 255)
    {
        _sendJsonError(400, "noNetworkResetMinutes out of range");
        return;
    }

    // All bounds-checked — strlcpy is now provably safe, not just "probably fine"
    strlcpy(upd.deviceName, deviceName, sizeof(upd.deviceName));
    strlcpy(upd.timezone, timezone, sizeof(upd.timezone));
    strlcpy(upd.ssid, ssid, sizeof(upd.ssid));
    strlcpy(upd.wifiPwd, wifiPwd, sizeof(upd.wifiPwd));
    strlcpy(upd.mqttHost, mqttHost, sizeof(upd.mqttHost));
    strlcpy(upd.mqttUser, mqttUser, sizeof(upd.mqttUser));
    strlcpy(upd.mqttPwd, mqttPwd, sizeof(upd.mqttPwd));
    upd.mqttPort = (uint16_t)port;
    upd.useSerial = doc["useSerial"] | true;
    upd.otaEnabled = doc["otaEnabled"] | false;
    upd.terminalEnabled = doc["terminalEnabled"] | false;
    upd.resetSafetyEnabled = doc["resetSafetyEnabled"] | false;
    upd.resetSafetyThreshold = (uint8_t)resetSafetyThreshold;
    upd.ignoreBootMsg = doc["ignoreBootMsg"] | false;
    upd.useFlashP = doc["useFlashP"] | false;
    upd.noNetworkResetMinutes = (uint8_t)noNetworkResetMinutes;
    strlcpy(upd.topicPubAvail, topicPubAvail, sizeof(upd.topicPubAvail));
    strlcpy(upd.topicPubState, topicPubState, sizeof(upd.topicPubState));
    strlcpy(upd.topicSubCmd, topicSubCmd, sizeof(upd.topicSubCmd));
    strlcpy(upd.topicGenMessages, topicGenMessages, sizeof(upd.topicGenMessages));
    strlcpy(upd.topicGenLog, topicGenLog, sizeof(upd.topicGenLog));
    strlcpy(upd.topicGenDebug, topicGenDebug, sizeof(upd.topicGenDebug));

    // extra topics — optional, validate length only
    for (uint8_t i = 0; i < 5; i++) {
        char key[12];
        snprintf(key, sizeof(key), "extraPub%d", i);
        const char *v = doc[key] | "";
        if (strlen(v) >= sizeof(upd.extraPub[i])) { _sendJsonError(400, "extraPub too long"); return; }
        strlcpy(upd.extraPub[i], v, sizeof(upd.extraPub[i]));

        snprintf(key, sizeof(key), "extraSub%d", i);
        v = doc[key] | "";
        if (strlen(v) >= sizeof(upd.extraSub[i])) { _sendJsonError(400, "extraSub too long"); return; }
        strlcpy(upd.extraSub[i], v, sizeof(upd.extraSub[i]));
    }

    char errMsg[80] = "";
    if (!_setCfg(upd, errMsg, sizeof(errMsg)))
    {
        _sendJsonError(422, errMsg[0] ? errMsg : "config rejected by device");
        return;
    }

    _terminalEnabled = upd.terminalEnabled;
    _server->send(200, "application/json", "{\"ok\":true}");
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~


void WebPortal::_handleDeleteConfig()
{
    if (!_deleteConfig) { _sendJsonError(500, "not wired up"); return; }
    _server->send(200, "application/json", "{\"ok\":true,\"message\":\"credentials deleted, rebooting\"}");
    delay(300);
    _deleteConfig();
}

void WebPortal::_handleDeleteTopics()
{
    if (!_deleteTopics) { _sendJsonError(500, "not wired up"); return; }
    bool ok = _deleteTopics();
    if (ok)
        _server->send(200, "application/json", "{\"ok\":true,\"message\":\"topics deleted, reboot to apply\"}");
    else
        _sendJsonError(500, "failed to delete topics file");
}

void WebPortal::_handleStartAP()
{
    if (!_startAP) { _sendJsonError(500, "not wired up"); return; }
    _server->send(200, "application/json", "{\"ok\":true,\"message\":\"switching to AP mode\"}");
    delay(300);
    _startAP();
}

void WebPortal::_sendJsonError(int code, const char *msg)
{
    JsonDocument doc;
    doc["error"] = msg;
    String out;
    serializeJson(doc, out);
    _server->send(code, "application/json", out);
}

// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// POST /button  — browser sends { "index": 0..3, "state": true/false }
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
void WebPortal::_handleButtonPost()
{
    String body = _server->arg("plain");
    if (body.length() == 0 || body.length() > 64) {
        _sendJsonError(400, "bad request");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        _sendJsonError(400, "malformed JSON");
        return;
    }
    uint8_t idx = doc["index"] | 255;
    if (idx >= 4) {
        _sendJsonError(400, "index out of range");
        return;
    }
    if (!_buttons[idx].enabled || !_buttonCb[idx]) {
        _sendJsonError(400, "button not enabled");
        return;
    }
    bool state = doc["state"] | true;
    if (_buttons[idx].isToggle) {
        _buttons[idx].toggleState = state;
    }
    _buttonCb[idx](idx, state);
    _server->send(200, "application/json", "{\"ok\":true}");
}