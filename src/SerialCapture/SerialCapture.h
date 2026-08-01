#ifndef SerialCapture_h
#define SerialCapture_h

/*
    SerialCapture — captures everything that goes through myIOT2's PRNT/PRNTL
    macros into an in-RAM ring buffer, so it can be viewed from the web
    portal instead of requiring a physical USB/serial connection.

    Two layers:
      1. RAM ring buffer (_buf, sized at begin()) — holds recent output for
         live tailing during normal operation. Lost on reset, same as a
         physical serial monitor would be if you weren't watching it.
      2. A small RTC-memory snapshot (ESP8266 only) — the last ~250 bytes
         are mirrored into RTC memory, which survives a *reset* (not a full
         power loss). restoreFromRTC(), called first thing in setup(),
         checks for a valid snapshot from just before the most recent reset
         and prepends it to the fresh buffer — so a crash tail is still
         visible after the reboot that followed it, not just silently lost.

    Fully inert unless `enabled` is true — myIOT2's terminalEnabled flag
    controls this, off by default, zero cost when off (append() returns
    immediately without touching the buffer).
*/

#include <Arduino.h>
#include <stdint.h>

class SerialCapture
{
public:
    static bool enabled;

    // capacityBytes: size of the RAM ring buffer. ESP8266 has ~80KB total
    // heap shared with WiFi/MQTT/WebServer/ArduinoJson — 4096 is a
    // deliberately conservative default (roughly 60-100 lines of recent
    // output, not a long scrollback). Raise it only if you've confirmed
    // headroom; there's no runtime bounds-check protecting you from an
    // allocation that's too big for what's left.
    // Call begin() FIRST (allocates the RAM buffer restoreFromRTC() writes
    // into), then restoreFromRTC() — before any other PRNT/PRNTL call — so
    // a crash tail from just before the last reset gets prepended to the
    // fresh buffer instead of being silently lost.
    static void begin(uint16_t capacityBytes = 4096);
    static void restoreFromRTC();

    template <typename T>
    static void append(T val)
    {
        if (!enabled || !_buf)
            return;
        String s(val);
        _appendRaw(s.c_str(), s.length());
    }

    template <typename T>
    static void appendLine(T val)
    {
        append(val);
        if (!enabled || !_buf)
            return;
        _appendRaw("\n", 1);
        _snapshotToRTC(); // throttled internally — cheap to call every line
    }

    // Raw guarded append, used by SerialCaptureSink. Honours `enabled` and a
    // failed allocation exactly like append() does, strips CR (Print::println
    // emits CRLF, we store LF only), and triggers the throttled RTC snapshot
    // when a line completes.
    static void appendRaw(const char *data, size_t len);

    // Full current buffer content, oldest-first. Allocates a String sized
    // to the buffer — fine for occasional portal polling, not for a hot
    // path.
    static String getBuffer();

    // True only if begin() successfully allocated the RAM buffer. Distinct
    // from `enabled` — enabled can be true while isActive() is false if the
    // malloc() failed (e.g. heap pressure at boot time).
    static bool isActive() { return _buf != nullptr && _cap > 0; }

private:
    static char *_buf;
    static uint16_t _cap;
    static uint16_t _writePos;
    static bool _full;
    static unsigned long _lastRtcSnapshotMs;

    static void _appendRaw(const char *data, uint16_t len);
    static void _snapshotToRTC();
};

// ~~~ Print adapter ~~~
// Lets SerialCapture be attached to any library exposing a `Print *` log hook
// — myIOT2 does this via `iotLogSink`. Deriving from Print means every
// print()/println() overload (String, const char*, int, float, F()) is handled
// by the core, so there is nothing type-specific to maintain here.
//
// Wiring lives in the sketch, not in either library:
//     iotLogSink = &SerialCaptureStream;
// so myIOT2 and WebPortal stay mutually unaware.
class SerialCaptureSink : public Print
{
public:
    size_t write(uint8_t c) override
    {
        char ch = (char)c;
        SerialCapture::appendRaw(&ch, 1);
        return 1;
    }

    size_t write(const uint8_t *buffer, size_t size) override
    {
        SerialCapture::appendRaw((const char *)buffer, size);
        return size;
    }
};

extern SerialCaptureSink SerialCaptureStream;

#endif