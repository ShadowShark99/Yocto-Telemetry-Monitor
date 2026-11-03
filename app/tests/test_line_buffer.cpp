// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "telemetry/line_buffer.hpp"

using telemetry::LineBuffer;
using Lines = std::vector<std::string>;

TEST(LineBuffer, ReassemblesLinesSplitAcrossReads) {
    LineBuffer buffer;
    EXPECT_EQ(buffer.feed("$TLM,a"), Lines{});
    EXPECT_EQ(buffer.pending_bytes(), 6U);
    EXPECT_EQ(buffer.feed(",b*00\n$TL"), Lines{"$TLM,a,b*00"});
    EXPECT_EQ(buffer.feed("M\n"), Lines{"$TLM"});
    EXPECT_EQ(buffer.pending_bytes(), 0U);
}

TEST(LineBuffer, ReturnsSeveralLinesFromOneRead) {
    LineBuffer buffer;
    EXPECT_EQ(buffer.feed("one\ntwo\nthree\n"), (Lines{"one", "two", "three"}));
}

TEST(LineBuffer, StripsCarriageReturnsAndSkipsEmptyLines) {
    LineBuffer buffer;
    EXPECT_EQ(buffer.feed("one\r\n\r\n\ntwo\r\n"), (Lines{"one", "two"}));
}

TEST(LineBuffer, DiscardsOverlongLinesUntilTheNextNewline) {
    LineBuffer buffer(8);
    EXPECT_EQ(buffer.feed("0123456789abc"), Lines{});
    EXPECT_EQ(buffer.overflows(), 1U);
    EXPECT_EQ(buffer.pending_bytes(), 0U);
    // The rest of the overlong line is ignored; the next line is fine.
    EXPECT_EQ(buffer.feed("still-garbage\nok\n"), Lines{"ok"});
    EXPECT_EQ(buffer.overflows(), 1U);
}

TEST(LineBuffer, AcceptsLinesExactlyAtTheLimit) {
    LineBuffer buffer(4);
    EXPECT_EQ(buffer.feed("abcd\n"), Lines{"abcd"});
    EXPECT_EQ(buffer.overflows(), 0U);
}

TEST(LineBuffer, ClearDropsPartialData) {
    LineBuffer buffer;
    buffer.feed("partial");
    buffer.clear();
    EXPECT_EQ(buffer.feed("line\n"), Lines{"line"});
}
