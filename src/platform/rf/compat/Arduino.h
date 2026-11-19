/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Minimal Arduino compatibility shim for the Linux/CM0 port. Lets the Bruce
 * `rf/protocols/` sources (copied verbatim) compile unchanged. Only the surface
 * those files actually use is provided: an Arduino `String` backed by
 * std::string, and the handful of free functions/macros they reference. The
 * GPIO/timing helpers exist so the ESP32 hardware code paths (RMT/M5 capture)
 * compile; those paths are never taken on Linux (we bridge to RfRxCapture /
 * rf_bitbang).
 */
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <algorithm>

// ---------------------------------------------------------------------------
// Arduino String (subset) over std::string.
// ---------------------------------------------------------------------------
class String {
public:
    String() {}
    String(const char *s) : _s(s ? s : "") {}
    String(const char *s, size_t n) : _s(s ? std::string(s, n) : std::string()) {}
    String(const std::string &s) : _s(s) {}
    String(char c) : _s(1, c) {}
    String(int v) : _s(std::to_string(v)) {}
    String(unsigned int v) : _s(std::to_string(v)) {}
    String(long v) : _s(std::to_string(v)) {}
    String(unsigned long v) : _s(std::to_string(v)) {}
    String(long long v) : _s(std::to_string(v)) {}
    String(unsigned long long v) : _s(std::to_string(v)) {}
    String(float v) : _s(std::to_string(v)) {}
    String(double v) : _s(std::to_string(v)) {}

    const char *c_str() const { return _s.c_str(); }
    unsigned int length() const { return static_cast<unsigned int>(_s.size()); }
    bool isEmpty() const { return _s.empty(); }
    void reserve(unsigned int n) { _s.reserve(n); }
    const std::string &str() const { return _s; }

    char charAt(unsigned int i) const { return i < _s.size() ? _s[i] : '\0'; }
    char &operator[](unsigned int i) { return _s[i]; }
    const char &operator[](unsigned int i) const { return _s[i]; }

    // Output sink used by ArduinoJson's generic Writer (serializeJson -> String).
    size_t write(uint8_t c) { _s += static_cast<char>(c); return 1; }
    size_t write(const uint8_t *s, size_t n) {
        _s.append(reinterpret_cast<const char *>(s), n);
        return n;
    }

    int indexOf(char c, unsigned int from = 0) const {
        auto p = _s.find(c, from);
        return p == std::string::npos ? -1 : static_cast<int>(p);
    }
    int indexOf(const char *s, unsigned int from = 0) const {
        auto p = _s.find(s, from);
        return p == std::string::npos ? -1 : static_cast<int>(p);
    }
    int indexOf(const String &s, unsigned int from = 0) const { return indexOf(s.c_str(), from); }
    int lastIndexOf(char c) const {
        auto p = _s.rfind(c);
        return p == std::string::npos ? -1 : static_cast<int>(p);
    }

    String substring(unsigned int from) const {
        if (from >= _s.size()) return String();
        return String(_s.substr(from));
    }
    String substring(unsigned int from, unsigned int to) const {
        if (from >= _s.size() || to <= from) return String();
        return String(_s.substr(from, to - from));
    }

    bool startsWith(const char *p) const { return _s.rfind(p, 0) == 0; }
    bool startsWith(const String &p) const { return startsWith(p.c_str()); }
    bool endsWith(const char *p) const {
        size_t n = std::strlen(p);
        return _s.size() >= n && _s.compare(_s.size() - n, n, p) == 0;
    }
    bool equals(const String &o) const { return _s == o._s; }
    bool equalsIgnoreCase(const String &o) const {
        if (_s.size() != o._s.size()) return false;
        for (size_t i = 0; i < _s.size(); ++i)
            if (std::tolower((unsigned char)_s[i]) != std::tolower((unsigned char)o._s[i])) return false;
        return true;
    }

    long toInt() const { return std::strtol(_s.c_str(), nullptr, 0); }
    float toFloat() const { return std::strtof(_s.c_str(), nullptr); }
    double toDouble() const { return std::strtod(_s.c_str(), nullptr); }

    void trim() {
        size_t a = _s.find_first_not_of(" \t\r\n");
        size_t b = _s.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) _s.clear();
        else _s = _s.substr(a, b - a + 1);
    }
    void toUpperCase() { std::transform(_s.begin(), _s.end(), _s.begin(), ::toupper); }
    void toLowerCase() { std::transform(_s.begin(), _s.end(), _s.begin(), ::tolower); }
    void remove(unsigned int idx, unsigned int count = ~0u) {
        if (idx < _s.size()) _s.erase(idx, count == ~0u ? std::string::npos : count);
    }
    void replace(const String &from, const String &to) {
        if (from._s.empty()) return;
        size_t p = 0;
        while ((p = _s.find(from._s, p)) != std::string::npos) {
            _s.replace(p, from._s.size(), to._s);
            p += to._s.size();
        }
    }
    void concat(const String &o) { _s += o._s; }
    void concat(const char *o) { if (o) _s += o; }

    String &operator+=(const String &o) { _s += o._s; return *this; }
    String &operator+=(const char *o) { if (o) _s += o; return *this; }
    String &operator+=(char c) { _s += c; return *this; }

    friend String operator+(String a, const String &b) { a._s += b._s; return a; }
    friend String operator+(String a, const char *b) { if (b) a._s += b; return a; }
    friend String operator+(const char *a, const String &b) { String r(a); r._s += b._s; return r; }

    bool operator==(const String &o) const { return _s == o._s; }
    bool operator==(const char *o) const { return _s == (o ? o : ""); }
    bool operator!=(const String &o) const { return _s != o._s; }
    bool operator!=(const char *o) const { return _s != (o ? o : ""); }
    bool operator<(const String &o) const { return _s < o._s; }

    explicit operator bool() const { return !_s.empty(); }

private:
    std::string _s;
};

// ---------------------------------------------------------------------------
// Free functions / macros.
// ---------------------------------------------------------------------------
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#ifndef HIGH
#define HIGH 1
#define LOW 0
#endif
#ifndef INPUT
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#endif
#ifndef CHANGE
#define CHANGE 3
#define RISING 1
#define FALLING 2
#endif

inline unsigned long millis() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<unsigned long>(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}
inline unsigned long micros() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<unsigned long>(ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL);
}
inline void delay(unsigned long) {}
inline void delayMicroseconds(unsigned int) {}
inline void yield() {}

// GPIO/ISR stubs — only reached by the ESP32 hardware paths, never on Linux.
inline int digitalRead(int) { return 0; }
inline void digitalWrite(int, int) {}
inline void pinMode(int, int) {}
inline void attachInterrupt(int, void (*)(), int) {}
inline void detachInterrupt(int) {}
inline long random(long max) { return max > 0 ? (std::rand() % max) : 0; }
inline long random(long lo, long hi) { return hi > lo ? lo + (std::rand() % (hi - lo)) : lo; }

#ifndef digitalPinToInterrupt
#define digitalPinToInterrupt(p) (p)
#endif

// Arduino character helpers used by the RF sources (do NOT define min/max/
// constrain as macros — they collide with std:: used by ArduinoJson).
inline bool isDigit(int c) { return c >= '0' && c <= '9'; }
inline bool isAlpha(int c) { return std::isalpha(c) != 0; }
inline bool isSpace(int c) { return std::isspace(c) != 0; }
inline bool isHexadecimalDigit(int c) { return std::isxdigit(c) != 0; }
template <typename T> T constrain(T x, T lo, T hi) { return x < lo ? lo : (x > hi ? hi : x); }

inline long map(long x, long inMin, long inMax, long outMin, long outMax) {
    if (inMax == inMin) return outMin;
    return (x - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}
