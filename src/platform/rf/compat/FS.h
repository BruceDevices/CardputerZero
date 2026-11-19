/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Arduino FS/File shim backed by the host filesystem. Enough for the KeeLoq
 * keystore reader and the legacy .sub migration helper to compile and, if a
 * real file exists at the given path, work. When the file is absent, open()
 * returns a falsy File and callers fall back to embedded data.
 */
#pragma once
#include <Arduino.h>
#include <cstdio>
#include <string>
#include <vector>

#ifndef FILE_READ
#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"
#endif

class File {
public:
    File() {}
    File(const std::string &path, const char *mode) {
        bool writing = mode && (mode[0] == 'w' || mode[0] == 'a');
        if (writing) {
            _fp = std::fopen(path.c_str(), mode);
            _ok = _fp != nullptr;
            return;
        }
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f) return;
        _ok = true;
        std::vector<char> buf(4096);
        size_t n;
        while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0)
            _content.append(buf.data(), n);
        std::fclose(f);
    }
    explicit operator bool() const { return _ok; }
    String readString() { return String(_content); }
    size_t size() const { return _content.size(); }
    int available() const { return static_cast<int>(_content.size() - _pos); }
    int read() { return _pos < _content.size() ? (unsigned char)_content[_pos++] : -1; }

    size_t print(const char *s) { return (_fp && s) ? std::fwrite(s, 1, std::strlen(s), _fp) : 0; }
    size_t print(const String &s) { return print(s.c_str()); }
    size_t println(const char *s) { size_t n = print(s); if (_fp) { std::fputc('\n', _fp); n++; } return n; }
    size_t println(const String &s) { return println(s.c_str()); }
    size_t println() { if (_fp) { std::fputc('\n', _fp); return 1; } return 0; }
    void close() { if (_fp) { std::fclose(_fp); _fp = nullptr; } _ok = false; }

private:
    bool _ok = false;
    FILE *_fp = nullptr;
    std::string _content;
    size_t _pos = 0;
};

class FS {
public:
    File open(const char *path, const char *mode = FILE_READ) { return File(std::string(path), mode); }
    File open(const String &path, const char *mode = FILE_READ) { return open(path.c_str(), mode); }
    bool exists(const char *path) {
        FILE *f = std::fopen(path, "rb");
        if (!f) return false;
        std::fclose(f);
        return true;
    }
    bool exists(const String &path) { return exists(path.c_str()); }
};
