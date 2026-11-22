/*
 * SPDX-FileCopyrightText: 2026 Bruce CardputerZero Community
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * NMEA fix model that behaves exactly like TinyGPS++ 1.1.0 (mikalhart/
 * TinyGPSPlus, LGPL-2.1-or-later), the parser Bruce firmware's GPS Tracker /
 * Wardriving are written against. Bruce's screen logic branches on TinyGPS++
 * details - `location.isUpdated()`, which reading lat()/lng() clears again, the
 * date/time commit on every RMC (even without a fix), satellites/HDOP from GGA,
 * values truncated (not rounded) to 2 decimals, an empty field keeping the
 * previous value - so the byte-level term machine of TinyGPSPlus::encode() /
 * endOfTermHandler() / parseDecimal() / parseDegrees() is reproduced here and
 * the firmware code can be ported line for line.
 *
 * Deliberate supersets (documented, they never change what a TinyGPS++ build
 * would show for the same input from a GP/GN receiver):
 *  - RMC/GGA are also accepted from the BD (BeiDou) and GQ (QZSS) talkers,
 *    besides TinyGPS++'s G[PNABL].
 *  - `$..TXT,..,..,..,ANTENNA OPEN|OK|SHORT` (ATGM336H) / `ANTSTATUS=...`
 *    (u-blox) is recognized by the same term machine and the antenna status is
 *    committed only when the sentence's checksum matches.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace platform::gps {

enum class Antenna { Unknown, Ok, Open, Short };

// TinyGPSLocation: committed by RMC (status A) / GGA (quality > 0).
class Location {
public:
    bool isValid() const { return valid_; }
    bool isUpdated() const { return updated_; }
    // Signed decimal degrees (deg + billionths). Reading clears isUpdated().
    double lat();
    double lng();

private:
    friend class Parser;
    struct Raw {
        uint16_t deg = 0;
        uint32_t billionths = 0;
        bool negative = false;
    };
    static double value(const Raw& r);
    void commit();
    bool valid_ = false, updated_ = false;
    Raw lat_, lng_, new_lat_, new_lng_;
};

// TinyGPSDate: ddmmyy as an integer, committed by every RMC.
class Date {
public:
    bool isValid() const { return valid_; }
    bool isUpdated() const { return updated_; }
    uint32_t value() const { return date_; }
    uint16_t year(); // date % 100 + 2000, clears isUpdated()
    uint8_t month();
    uint8_t day();

private:
    friend class Parser;
    void commit();
    bool valid_ = false, updated_ = false;
    uint32_t date_ = 0, new_date_ = 0;
};

// TinyGPSTime: hhmmsscc as an integer, committed by RMC and GGA.
class Time {
public:
    bool isValid() const { return valid_; }
    bool isUpdated() const { return updated_; }
    uint32_t value() const { return time_; }
    uint8_t hour(); // clears isUpdated()
    uint8_t minute();
    uint8_t second();
    uint8_t centisecond();

private:
    friend class Parser;
    void commit();
    bool valid_ = false, updated_ = false;
    uint32_t time_ = 0, new_time_ = 0;
};

// TinyGPSInteger (satellites): atol() of the term.
class Integer {
public:
    bool isValid() const { return valid_; }
    bool isUpdated() const { return updated_; }
    uint32_t value(); // clears isUpdated()

private:
    friend class Parser;
    void commit();
    bool valid_ = false, updated_ = false;
    uint32_t val_ = 0, new_val_ = 0;
};

// TinyGPSDecimal: value * 100, truncated to 2 decimals (parseDecimal()).
class Decimal {
public:
    bool isValid() const { return valid_; }
    bool isUpdated() const { return updated_; }
    int32_t value(); // clears isUpdated()

private:
    friend class Parser;
    void commit();
    bool valid_ = false, updated_ = false;
    int32_t val_ = 0, new_val_ = 0;
};

class Hdop : public Decimal {
public:
    double hdop() { return value() / 100.0; }
};

class Altitude : public Decimal {
public:
    double meters() { return value() / 100.0; }
};

class Speed : public Decimal {
public:
    double knots() { return value() / 100.0; }
    double kmph() { return 1.852 * value() / 100.0; } // _GPS_KMPH_PER_KNOT
};

class Course : public Decimal {
public:
    double deg() { return value() / 100.0; }
};

// TinyGPSPlus equivalent. Feed it raw serial bytes; a sentence commits when its
// checksum term matches, exactly like TinyGPSPlus::encode().
class Parser {
public:
    Location location;
    Date date;
    Time time;
    Speed speed;
    Course course;
    Integer satellites;
    Hdop hdop;
    Altitude altitude;

    // Returns true when the byte completed a sentence that passed its checksum.
    bool encode(char c);
    size_t encode(const char* data, size_t n); // returns sentences that passed

    Antenna antenna() const { return antenna_; }
    uint32_t charsProcessed() const { return chars_; }
    uint32_t passedChecksum() const { return passed_; }
    // Checksum mismatches only, like TinyGPS++: a sentence without a checksum
    // is ignored, not counted.
    uint32_t failedChecksum() const { return failed_; }

    // TinyGPSPlus::distanceBetween (great circle, R = 6371009 m), in meters.
    static double distanceBetween(double lat1, double long1, double lat2, double long2);

    // TinyGPSPlus::parseDecimal / parseDegrees, exposed for tests.
    static int32_t parseDecimal(const char* term);

private:
    enum class Sentence { Other, Rmc, Gga, Txt };
    static constexpr size_t kMaxField = 15; // _GPS_MAX_FIELD_SIZE

    bool end_of_term(char terminator);

    // TinyGPSPlus parsing state
    uint8_t parity_ = 0;
    bool is_checksum_term_ = false;
    char term_[kMaxField] = {};
    uint8_t term_number_ = 0;
    uint8_t term_offset_ = 0;
    bool sentence_has_fix_ = false;
    bool discard_ = false; // rest of a malformed $..TXT is ignored until '$'
    Sentence sentence_ = Sentence::Other;

    // $..TXT antenna status: parsed from term 4, committed on a valid checksum.
    Antenna new_antenna_ = Antenna::Unknown;
    Antenna antenna_ = Antenna::Unknown;

    uint32_t chars_ = 0, passed_ = 0, failed_ = 0;
};

const char* antenna_name(Antenna a);

} // namespace platform::gps
