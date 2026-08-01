#include "SerialCapture.h"

#if defined(ESP32)
// Survives a reset (not a full power cycle) on ESP32 — the ESP8266 path
// below uses the RTC user-memory API instead, since that's what's actually
// available there.
RTC_NOINIT_ATTR uint32_t _rtcMagic;
RTC_NOINIT_ATTR uint16_t _rtcLength;
RTC_NOINIT_ATTR char _rtcSnippet[248];
#endif

namespace
{
    const uint32_t RTC_MAGIC = 0xC4A5B6D7; // arbitrary marker distinguishing
                                            // "valid snapshot" from cold-boot garbage
    const uint16_t RTC_SNIPPET_LEN = 248;
    const unsigned long RTC_SNAPSHOT_INTERVAL_MS = 2000; // throttle — RTC writes
                                                           // are cheap but there's
                                                           // no reason to do one
                                                           // on every single line
}

bool SerialCapture::enabled = false;
char *SerialCapture::_buf = nullptr;
uint16_t SerialCapture::_cap = 0;
uint16_t SerialCapture::_writePos = 0;
bool SerialCapture::_full = false;
unsigned long SerialCapture::_lastRtcSnapshotMs = 0;

void SerialCapture::begin(uint16_t capacityBytes)
{
    if (_buf)
        return; // already initialized — begin() is idempotent, not a leak

    _buf = (char *)malloc(capacityBytes);
    if (!_buf)
    {
        // Allocation failed (likely heap pressure from WiFi/MQTT/WebServer
        // already in use). Fail safe: capture stays fully inert rather than
        // writing through a null pointer.
        _cap = 0;
        return;
    }
    _cap = capacityBytes;
    _writePos = 0;
    _full = false;
}

void SerialCapture::_appendRaw(const char *data, uint16_t len)
{
    if (!_buf || _cap == 0)
        return;

    for (uint16_t i = 0; i < len; i++)
    {
        _buf[_writePos] = data[i];
        _writePos++;
        if (_writePos >= _cap)
        {
            _writePos = 0;
            _full = true;
        }
    }
}

SerialCaptureSink SerialCaptureStream;

void SerialCapture::appendRaw(const char *data, size_t len)
{
    if (!enabled || !_buf)
        return;

    for (size_t i = 0; i < len; i++)
    {
        if (data[i] == '\r')
            continue; // Print::println() emits CRLF; keep the buffer LF-only

        _appendRaw(&data[i], 1);

        if (data[i] == '\n')
            _snapshotToRTC(); // throttled internally — cheap per line
    }
}

String SerialCapture::getBuffer(){
    if (!_buf || _cap == 0)
        return String("");

    String out;
    if (!_full)
    {
        out.reserve(_writePos);
        for (uint16_t i = 0; i < _writePos; i++)
            out += _buf[i];
    }
    else
    {
        // Ring has wrapped — oldest byte is right at the current write
        // position, newest is just before it. Linearize oldest-first.
        out.reserve(_cap);
        for (uint16_t i = 0; i < _cap; i++)
        {
            uint16_t idx = (_writePos + i) % _cap;
            out += _buf[idx];
        }
    }
    return out;
}

void SerialCapture::_snapshotToRTC()
{
    unsigned long now = millis();
    if (now - _lastRtcSnapshotMs < RTC_SNAPSHOT_INTERVAL_MS)
        return;
    _lastRtcSnapshotMs = now;

    // Take the tail end of the current buffer — the most recent
    // RTC_SNIPPET_LEN bytes — since that's what matters if this turns out
    // to be the snapshot right before a crash.
    String full = getBuffer();
    uint16_t len = full.length();
    uint16_t start = (len > RTC_SNIPPET_LEN) ? (len - RTC_SNIPPET_LEN) : 0;
    uint16_t copyLen = len - start;

#if defined(ESP8266)
    struct
    {
        uint32_t magic;
        uint16_t length;
        uint16_t reserved;
        char snippet[RTC_SNIPPET_LEN];
    } rtcData;

    rtcData.magic = RTC_MAGIC;
    rtcData.length = copyLen;
    rtcData.reserved = 0;
    memset(rtcData.snippet, 0, RTC_SNIPPET_LEN);
    memcpy(rtcData.snippet, full.c_str() + start, copyLen);

    ESP.rtcUserMemoryWrite(0, (uint32_t *)&rtcData, sizeof(rtcData));
#elif defined(ESP32)
    _rtcMagic = RTC_MAGIC;
    _rtcLength = copyLen;
    memset(_rtcSnippet, 0, RTC_SNIPPET_LEN);
    memcpy(_rtcSnippet, full.c_str() + start, copyLen);
#endif
}

void SerialCapture::restoreFromRTC()
{
    if (!enabled)
        return; // stays fully inert when the terminal feature is off,
                // same as append()/appendLine()

#if defined(ESP8266)
    struct
    {
        uint32_t magic;
        uint16_t length;
        uint16_t reserved;
        char snippet[RTC_SNIPPET_LEN];
    } rtcData;

    if (!ESP.rtcUserMemoryRead(0, (uint32_t *)&rtcData, sizeof(rtcData)))
        return; // read failed — nothing to restore, not an error
    if (rtcData.magic != RTC_MAGIC)
        return; // cold boot / power-on reset — RTC memory is uninitialized
                // garbage, not a real snapshot. Correctly ignored.
    if (rtcData.length == 0 || rtcData.length > RTC_SNIPPET_LEN)
        return; // corrupt length — don't trust it

    _appendRaw("--- resumed after reset ---\n", 29);
    _appendRaw(rtcData.snippet, rtcData.length);
    _appendRaw("\n--- end of pre-reset log ---\n", 30);

    // Clear the magic so a *second* reset without any new logging in
    // between doesn't replay the same stale snippet again.
    rtcData.magic = 0;
    ESP.rtcUserMemoryWrite(0, (uint32_t *)&rtcData, sizeof(rtcData));
#elif defined(ESP32)
    if (_rtcMagic != RTC_MAGIC)
        return;
    if (_rtcLength == 0 || _rtcLength > RTC_SNIPPET_LEN)
        return;

    _appendRaw("--- resumed after reset ---\n", 29);
    _appendRaw(_rtcSnippet, _rtcLength);
    _appendRaw("\n--- end of pre-reset log ---\n", 30);

    _rtcMagic = 0;
#endif
}