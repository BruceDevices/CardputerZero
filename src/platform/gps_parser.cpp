/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * The term machine below is a port of TinyGPS++ 1.1.0 (TinyGPS++.cpp:
 * encode(), endOfTermHandler(), parseDecimal(), parseDegrees(),
 * distanceBetween(); Copyright (C) 2008-2024 Mikal Hart, LGPL-2.1-or-later),
 * used here under the "or later" option as part of this AGPL-3.0 program.
 */

#include "gps_parser.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace platform::gps {
namespace {

constexpr double kEarthMeanRadius = 6371009.0; // _GPS_EARTH_MEAN_RADIUS (1.1.0)
constexpr size_t kMaxTxtLine = 120;

double radians(double deg) {
    constexpr double kPi = 3.14159265358979323846;
    return deg * kPi / 180.0;
}

double sq(double v) {
    return v * v;
}

int from_hex(char a) {
    if (a >= 'A' && a <= 'F') {
        return a - 'A' + 10;
    }
    if (a >= 'a' && a <= 'f') {
        return a - 'a' + 10;
    }
    return a - '0';
}

// TinyGPS++: term[0] == 'G' && strchr("PNABL", term[1]); plus BD / GQ.
bool supported_talker(const char* term) {
    if (term[0] == 'G' && term[1] != '\0' && std::strchr("PNABL", term[1]) != nullptr) {
        return true;
    }
    return (term[0] == 'B' && term[1] == 'D') || (term[0] == 'G' && term[1] == 'Q');
}

void parse_degrees(const char* term, uint16_t& deg, uint32_t& billionths, bool& negative) {
    const uint32_t left_of_decimal = static_cast<uint32_t>(std::atol(term));
    const uint16_t minutes = static_cast<uint16_t>(left_of_decimal % 100);
    uint32_t multiplier = 10000000UL;
    uint32_t ten_millionths_of_minutes = minutes * multiplier;

    deg = static_cast<uint16_t>(left_of_decimal / 100);

    while (std::isdigit(static_cast<unsigned char>(*term))) {
        ++term;
    }
    if (*term == '.') {
        while (std::isdigit(static_cast<unsigned char>(*++term))) {
            multiplier /= 10;
            ten_millionths_of_minutes += static_cast<uint32_t>(*term - '0') * multiplier;
        }
    }
    billionths = (5 * ten_millionths_of_minutes + 1) / 3;
    negative = false;
}

} // namespace

const char* antenna_name(Antenna a) {
    switch (a) {
        case Antenna::Ok: return "OK";
        case Antenna::Open: return "OPEN";
        case Antenna::Short: return "SHORT";
        case Antenna::Unknown: break;
    }
    return "unknown";
}

// --- field classes (TinyGPS++ semantics) -------------------------------------

double Location::value(const Raw& r) {
    const double ret = r.deg + r.billionths / 1000000000.0;
    return r.negative ? -ret : ret;
}

void Location::commit() {
    lat_ = new_lat_;
    lng_ = new_lng_;
    valid_ = updated_ = true;
}

double Location::lat() {
    updated_ = false;
    return value(lat_);
}

double Location::lng() {
    updated_ = false;
    return value(lng_);
}

void Date::commit() {
    date_ = new_date_;
    valid_ = updated_ = true;
}

uint16_t Date::year() {
    updated_ = false;
    return static_cast<uint16_t>(date_ % 100 + 2000);
}

uint8_t Date::month() {
    updated_ = false;
    return static_cast<uint8_t>((date_ / 100) % 100);
}

uint8_t Date::day() {
    updated_ = false;
    return static_cast<uint8_t>(date_ / 10000);
}

void Time::commit() {
    time_ = new_time_;
    valid_ = updated_ = true;
}

uint8_t Time::hour() {
    updated_ = false;
    return static_cast<uint8_t>(time_ / 1000000);
}

uint8_t Time::minute() {
    updated_ = false;
    return static_cast<uint8_t>((time_ / 10000) % 100);
}

uint8_t Time::second() {
    updated_ = false;
    return static_cast<uint8_t>((time_ / 100) % 100);
}

uint8_t Time::centisecond() {
    updated_ = false;
    return static_cast<uint8_t>(time_ % 100);
}

void Integer::commit() {
    val_ = new_val_;
    valid_ = updated_ = true;
}

uint32_t Integer::value() {
    updated_ = false;
    return val_;
}

void Decimal::commit() {
    val_ = new_val_;
    valid_ = updated_ = true;
}

int32_t Decimal::value() {
    updated_ = false;
    return val_;
}

// --- parser -------------------------------------------------------------------

int32_t Parser::parseDecimal(const char* term) {
    const bool negative = *term == '-';
    if (negative) {
        ++term;
    }
    int32_t ret = 100 * static_cast<int32_t>(std::atol(term));
    while (std::isdigit(static_cast<unsigned char>(*term))) {
        ++term;
    }
    if (*term == '.' && std::isdigit(static_cast<unsigned char>(term[1]))) {
        ret += 10 * (term[1] - '0');
        if (std::isdigit(static_cast<unsigned char>(term[2]))) {
            ret += term[2] - '0';
        }
    }
    return negative ? -ret : ret;
}

// TinyGPSPlus::encode()
bool Parser::encode(char c) {
    ++chars_;

    switch (c) {
        case ',': // term terminators
            parity_ ^= static_cast<uint8_t>(c);
            [[fallthrough]];
        case '\r':
        case '\n':
        case '*': {
            bool valid_sentence = false;
            if (term_offset_ < sizeof(term_)) {
                term_[term_offset_] = 0;
                valid_sentence = end_of_term(c);
            }
            ++term_number_;
            term_offset_ = 0;
            is_checksum_term_ = c == '*';
            return valid_sentence;
        }

        case '$': // sentence begin
            term_number_ = term_offset_ = 0;
            parity_ = 0;
            sentence_ = Sentence::Other;
            is_checksum_term_ = false;
            sentence_has_fix_ = false;
            new_antenna_ = Antenna::Unknown;
            discard_ = false;
            return false;

        default: // ordinary characters
            if (term_offset_ < sizeof(term_) - 1) {
                term_[term_offset_++] = c;
            }
            if (!is_checksum_term_) {
                parity_ ^= static_cast<uint8_t>(c);
            }
            return false;
    }
}

size_t Parser::encode(const char* data, size_t n) {
    size_t valid = 0;
    for (size_t i = 0; i < n; ++i) {
        if (encode(data[i])) {
            ++valid;
        }
    }
    return valid;
}

// TinyGPSPlus::endOfTermHandler()
bool Parser::end_of_term(char terminator) {
    if (discard_) {
        return false; // after a malformed TXT checksum, until the next '$'
    }
    if (is_checksum_term_) {
        if (sentence_ == Sentence::Txt) {
            // Stricter than TinyGPS++ (which reads only the first two
            // characters): the antenna status commits only for exactly two hex
            // digits directly followed by the end of the line.
            // Count received bytes (term_offset_), not the C string: a NUL
            // byte must not hide trailing garbage ("*25<NUL>extra").
            const bool strict = term_offset_ == 2 &&
                                std::isxdigit(static_cast<unsigned char>(term_[0])) &&
                                std::isxdigit(static_cast<unsigned char>(term_[1])) &&
                                (terminator == '\r' || terminator == '\n');
            if (!strict) {
                ++failed_;
                // Drop the rest of this sentence: a second '*' must not get
                // another chance ("*00*25" would otherwise commit).
                discard_ = true;
                return false;
            }
        }
        const uint8_t checksum = static_cast<uint8_t>(16 * from_hex(term_[0]) + from_hex(term_[1]));
        if (checksum == parity_) {
            ++passed_;
            switch (sentence_) {
                case Sentence::Rmc:
                    date.commit();
                    time.commit();
                    if (sentence_has_fix_) {
                        location.commit();
                        speed.commit();
                        course.commit();
                    }
                    break;
                case Sentence::Gga:
                    time.commit();
                    if (sentence_has_fix_) {
                        location.commit();
                        altitude.commit();
                    }
                    satellites.commit();
                    hdop.commit();
                    break;
                case Sentence::Txt:
                    if (new_antenna_ != Antenna::Unknown) {
                        antenna_ = new_antenna_;
                    }
                    break;
                case Sentence::Other:
                    break;
            }
            return true;
        }
        ++failed_;
        return false;
    }

    // The first term determines the sentence type.
    if (term_number_ == 0) {
        if (supported_talker(term_) && std::strcmp(term_ + 2, "RMC") == 0) {
            sentence_ = Sentence::Rmc;
        } else if (supported_talker(term_) && std::strcmp(term_ + 2, "GGA") == 0) {
            sentence_ = Sentence::Gga;
        } else if (std::strlen(term_) == 5 && std::strcmp(term_ + 2, "TXT") == 0) {
            sentence_ = Sentence::Txt; // any talker ($GPTXT, $GNTXT, $BDTXT, ...)
        } else {
            sentence_ = Sentence::Other;
        }
        return false;
    }

    if (sentence_ == Sentence::Other || term_[0] == '\0') {
        return false; // an empty term keeps the previous value
    }

    if (sentence_ == Sentence::Txt) {
        // $xxTXT,<total>,<num>,<type>,<text>: the text is term 4. Terms are
        // capped at 14 characters like TinyGPS++, which still keeps
        // "ANTENNA SHORT" / "ANTSTATUS=OPEN" intact enough to classify.
        if (term_number_ == 4 && std::strncmp(term_, "ANT", 3) == 0) {
            if (std::strstr(term_, "OPEN") != nullptr) {
                new_antenna_ = Antenna::Open;
            } else if (std::strstr(term_, "SHOR") != nullptr) {
                new_antenna_ = Antenna::Short;
            } else if (std::strstr(term_, "OK") != nullptr) {
                new_antenna_ = Antenna::Ok;
            }
        }
        return false;
    }

    if (sentence_ == Sentence::Rmc) {
        switch (term_number_) {
            case 1: time.new_time_ = static_cast<uint32_t>(parseDecimal(term_)); break;
            case 2: sentence_has_fix_ = term_[0] == 'A'; break;
            case 3: parse_degrees(term_, location.new_lat_.deg, location.new_lat_.billionths,
                                  location.new_lat_.negative); break;
            case 4: location.new_lat_.negative = term_[0] == 'S'; break;
            case 5: parse_degrees(term_, location.new_lng_.deg, location.new_lng_.billionths,
                                  location.new_lng_.negative); break;
            case 6: location.new_lng_.negative = term_[0] == 'W'; break;
            case 7: speed.new_val_ = parseDecimal(term_); break;
            case 8: course.new_val_ = parseDecimal(term_); break;
            case 9: date.new_date_ = static_cast<uint32_t>(std::atol(term_)); break;
            default: break;
        }
    } else { // GGA
        switch (term_number_) {
            case 1: time.new_time_ = static_cast<uint32_t>(parseDecimal(term_)); break;
            case 2: parse_degrees(term_, location.new_lat_.deg, location.new_lat_.billionths,
                                  location.new_lat_.negative); break;
            case 3: location.new_lat_.negative = term_[0] == 'S'; break;
            case 4: parse_degrees(term_, location.new_lng_.deg, location.new_lng_.billionths,
                                  location.new_lng_.negative); break;
            case 5: location.new_lng_.negative = term_[0] == 'W'; break;
            case 6: sentence_has_fix_ = term_[0] > '0'; break;
            case 7: satellites.new_val_ = static_cast<uint32_t>(std::atol(term_)); break;
            case 8: hdop.new_val_ = parseDecimal(term_); break;
            case 9: altitude.new_val_ = parseDecimal(term_); break;
            default: break;
        }
    }
    return false;
}

double Parser::distanceBetween(double lat1, double long1, double lat2, double long2) {
    // Verbatim port of TinyGPSPlus::distanceBetween (courtesy of Maarten Lamers).
    double delta = radians(long1 - long2);
    const double sdlong = std::sin(delta);
    const double cdlong = std::cos(delta);
    lat1 = radians(lat1);
    lat2 = radians(lat2);
    const double slat1 = std::sin(lat1);
    const double clat1 = std::cos(lat1);
    const double slat2 = std::sin(lat2);
    const double clat2 = std::cos(lat2);
    delta = (clat1 * slat2) - (slat1 * clat2 * cdlong);
    delta = sq(delta);
    delta += sq(clat2 * sdlong);
    delta = std::sqrt(delta);
    const double denom = (slat1 * slat2) + (clat1 * clat2 * cdlong);
    delta = std::atan2(delta, denom);
    return delta * kEarthMeanRadius;
}

} // namespace platform::gps
