// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <set>
#include <string>
#include <variant>

#include "telemetry/packet.hpp"

using telemetry::checksum;
using telemetry::format_packet;
using telemetry::parse_packet;
using telemetry::ParseError;
using telemetry::Reading;

namespace {

// Reference checksums were computed independently (Python reduce over XOR).
constexpr const char* kValidLine = "$TLM,env-01,42,23.50,41.2,3.29*0B";

ParseError error_of(const telemetry::ParseResult& result) {
    const auto* error = std::get_if<ParseError>(&result);
    EXPECT_NE(error, nullptr) << "expected a parse error";
    return error != nullptr ? *error : ParseError::Empty;
}

// Returns by value: the ParseResult passed in is usually a temporary.
Reading reading_of(const telemetry::ParseResult& result) {
    const auto* reading = std::get_if<Reading>(&result);
    EXPECT_NE(reading, nullptr) << "expected a reading, got "
                                << (reading ? "" : telemetry::to_string(std::get<ParseError>(result)));
    return reading != nullptr ? *reading : Reading{};
}

}  // namespace

TEST(Checksum, MatchesReferenceValues) {
    EXPECT_EQ(checksum("TLM,env-01,42,23.50,41.2,3.29"), 0x0B);
    EXPECT_EQ(checksum("TLM,env-02,7,-5.25,99.9,12.00"), 0x17);
    EXPECT_EQ(checksum(""), 0x00);
}

TEST(ParsePacket, ParsesValidLine) {
    const Reading r = reading_of(parse_packet(kValidLine));
    EXPECT_EQ(r.sensor_id, "env-01");
    EXPECT_EQ(r.sequence, 42U);
    EXPECT_DOUBLE_EQ(r.temperature_c, 23.50);
    EXPECT_DOUBLE_EQ(r.humidity_pct, 41.2);
    EXPECT_DOUBLE_EQ(r.voltage_v, 3.29);
}

TEST(ParsePacket, AcceptsNegativeValuesAndLowercaseChecksum) {
    const Reading r = reading_of(parse_packet("$TLM,env-02,7,-5.25,99.9,12.00*17"));
    EXPECT_DOUBLE_EQ(r.temperature_c, -5.25);
    EXPECT_EQ(error_of(parse_packet("$TLM,env-01,42,23.50,41.2,3.29*0c")), ParseError::ChecksumMismatch);
    EXPECT_TRUE(std::holds_alternative<Reading>(parse_packet("$TLM,node_A,0,0.00,0.0,0.00*79")));
}

TEST(ParsePacket, IgnoresSurroundingWhitespaceAndCrLf) {
    EXPECT_TRUE(std::holds_alternative<Reading>(parse_packet("  $TLM,env-01,42,23.50,41.2,3.29*0B\r\n")));
}

TEST(ParsePacket, RejectsFramingErrors) {
    EXPECT_EQ(error_of(parse_packet("")), ParseError::Empty);
    EXPECT_EQ(error_of(parse_packet("   \r\n")), ParseError::Empty);
    EXPECT_EQ(error_of(parse_packet("TLM,env-01,42,23.50,41.2,3.29*0B")), ParseError::MissingStart);
    EXPECT_EQ(error_of(parse_packet("$TLM,env-01,42,23.50,41.2,3.29")), ParseError::MissingChecksum);
    EXPECT_EQ(error_of(parse_packet("$TLM,env-01,42,23.50,41.2,3.29*0")), ParseError::BadChecksumFormat);
    EXPECT_EQ(error_of(parse_packet("$TLM,env-01,42,23.50,41.2,3.29*0BB")), ParseError::BadChecksumFormat);
    EXPECT_EQ(error_of(parse_packet("$TLM,env-01,42,23.50,41.2,3.29*G0")), ParseError::BadChecksumFormat);
    EXPECT_EQ(error_of(parse_packet("$TLM,env-01,42,23.50,41.2,3.29*0C")), ParseError::ChecksumMismatch);
}

TEST(ParsePacket, RejectsWrongTypeAndFieldCount) {
    EXPECT_EQ(error_of(parse_packet("$XYZ,env-01,42,23.50,41.2,3.29*05")), ParseError::UnknownType);
    EXPECT_EQ(error_of(parse_packet("$TLM,env-01,1,23.50,41.2,3.29,9*29")), ParseError::WrongFieldCount);
}

TEST(ParsePacket, RejectsBadFieldContents) {
    EXPECT_EQ(error_of(parse_packet("$TLM,env 01,42,23.50,41.2,3.29*06")), ParseError::BadSensorId);
    EXPECT_EQ(error_of(parse_packet("$TLM,env-01,4x,23.50,41.2,3.29*41")), ParseError::BadNumber);
    EXPECT_EQ(error_of(parse_packet("$TLM,env-01,-1,23.50,41.2,3.29*11")), ParseError::BadNumber);
    EXPECT_EQ(error_of(parse_packet("$TLM,env-01,42,nan,41.2,3.29*40")), ParseError::BadNumber);
    EXPECT_EQ(error_of(parse_packet("$TLM,env-01,42,23.50,141.2,3.29*3A")), ParseError::OutOfRange);
}

TEST(SensorId, EnforcesCharsetAndLength) {
    EXPECT_TRUE(telemetry::is_valid_sensor_id("env-01"));
    EXPECT_TRUE(telemetry::is_valid_sensor_id("A_b-9"));
    EXPECT_TRUE(telemetry::is_valid_sensor_id(std::string(32, 'x')));
    EXPECT_FALSE(telemetry::is_valid_sensor_id(std::string(33, 'x')));
    EXPECT_FALSE(telemetry::is_valid_sensor_id(""));
    EXPECT_FALSE(telemetry::is_valid_sensor_id("env/01"));
    EXPECT_FALSE(telemetry::is_valid_sensor_id("caf\xc3\xa9"));
}

TEST(FormatPacket, ProducesTheDocumentedWireFormat) {
    Reading r;
    r.sensor_id = "env-01";
    r.sequence = 42;
    r.temperature_c = 23.5;
    r.humidity_pct = 41.2;
    r.voltage_v = 3.29;
    EXPECT_EQ(format_packet(r), kValidLine);
}

TEST(FormatPacket, RoundTripsThroughTheParser) {
    Reading r;
    r.sensor_id = "node_7";
    r.sequence = 0xFFFFFFFFU;
    r.temperature_c = -12.34;
    r.humidity_pct = 100.0;
    r.voltage_v = 48.0;
    const Reading back = reading_of(parse_packet(format_packet(r)));
    EXPECT_EQ(back.sensor_id, r.sensor_id);
    EXPECT_EQ(back.sequence, r.sequence);
    EXPECT_DOUBLE_EQ(back.temperature_c, r.temperature_c);
    EXPECT_DOUBLE_EQ(back.humidity_pct, r.humidity_pct);
    EXPECT_DOUBLE_EQ(back.voltage_v, r.voltage_v);
}

TEST(ParseErrorNames, AreUniqueAndCoverEveryValue) {
    std::set<std::string> names;
    for (std::size_t i = 0; i < telemetry::kParseErrorCount; ++i) {
        names.insert(telemetry::to_string(static_cast<ParseError>(i)));
    }
    EXPECT_EQ(names.size(), telemetry::kParseErrorCount);
    EXPECT_EQ(names.count("unknown"), 0U);
}
