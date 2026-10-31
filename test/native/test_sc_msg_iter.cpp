/*
 * test_sc_msg_iter.cpp - scsynth's argument reader stays inside the message.
 *
 * Every command handler reads its arguments through sc_msg_iter, and the
 * message is a client's bytes. Its type tags say how many arguments follow and
 * what each is; the reader stops at whichever ends first, the tags or the
 * bytes. Each message here is held in a buffer of exactly its own size, so a
 * read past its end is a heap overflow under AddressSanitizer as well as a
 * wrong answer.
 */
#include <catch2/catch_test_macros.hpp>

#include "SC_Types.h"
#include "sc_msg_iter.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

int32_t bitsOf(float f) {
    int32_t i;
    std::memcpy(&i, &f, sizeof i);
    return i;
}

// A message's argument part (type tags, then the data), written byte by byte.
class Args {
public:
    explicit Args(const char* tags) { str(tags); }
    Args& i(int32_t v) { for (int k = 3; k >= 0; --k) mBytes.push_back(char(uint32_t(v) >> (8 * k))); return *this; }
    Args& raw(const char* s, size_t n) { mBytes.insert(mBytes.end(), s, s + n); return *this; }
    Args& str(const char* s) {
        raw(s, std::strlen(s));
        do mBytes.push_back(0); while (mBytes.size() % 4 != 0);
        return *this;
    }
    // A copy on the heap of exactly the message's size.
    std::unique_ptr<char[]> exact() const {
        auto p = std::make_unique<char[]>(mBytes.size());
        std::memcpy(p.get(), mBytes.data(), mBytes.size());
        return p;
    }
    int size() const { return static_cast<int>(mBytes.size()); }
private:
    std::vector<char> mBytes;
};

} // namespace

TEST_CASE("sc_msg_iter: nothing remains once every tagged argument is read", "[sc_msg_iter]") {
    // One int tagged, two ints of data.
    const Args a = Args(",i").i(7).i(8);
    auto bytes = a.exact();
    sc_msg_iter msg(a.size(), bytes.get());
    CHECK(msg.geti() == 7);
    CHECK(msg.remain() == 0);
    CHECK(msg.nextTag() == 0);
}

TEST_CASE("sc_msg_iter: nothing remains once the data runs out", "[sc_msg_iter]") {
    // Three ints tagged, one int of data.
    const Args a = Args(",iii").i(7);
    auto bytes = a.exact();
    sc_msg_iter msg(a.size(), bytes.get());
    CHECK(msg.geti() == 7);
    CHECK(msg.remain() == 0);
    CHECK(msg.geti(-1) == -1);
}

TEST_CASE("sc_msg_iter: a value with too few bytes left is not read", "[sc_msg_iter]") {
    // An int tagged, two bytes of data.
    const Args a = Args(",i").raw("\x01\x02", 2);
    auto bytes = a.exact();
    sc_msg_iter msg(a.size(), bytes.get());
    CHECK(msg.geti(-1) == -1);
    CHECK(msg.remain() == 0);
}

// A caller that saw an 's' tag uses the string without checking it, so a
// string the data does not hold reads as empty rather than as nothing.
TEST_CASE("sc_msg_iter: a string with no end inside the message reads as empty", "[sc_msg_iter]") {
    // A string tagged, four bytes of it and no terminator.
    const Args a = Args(",s").raw("abcd", 4);
    auto bytes = a.exact();
    sc_msg_iter msg(a.size(), bytes.get());
    const char* s = msg.gets();
    REQUIRE(s != nullptr);
    CHECK(*s == 0);
    CHECK(msg.remain() == 0);
}

TEST_CASE("sc_msg_iter: a string tagged with no bytes left reads as empty", "[sc_msg_iter]") {
    // An int and a string tagged, only the int's bytes.
    const Args a = Args(",is").i(1000);
    auto bytes = a.exact();
    sc_msg_iter msg(a.size(), bytes.get());
    CHECK(msg.geti() == 1000);
    REQUIRE(msg.nextTag() == 's');
    const int32* name = msg.gets4();
    REQUIRE(name != nullptr);
    CHECK(*name == 0);
    CHECK(msg.remain() == 0);
    CHECK(msg.gets() == nullptr);
}

// gets4 hands back a name that Hash and the name tables read a word at a time,
// up to the first word whose last byte is zero: the word holding the
// terminator must lie inside the message and be padded with zeros.
TEST_CASE("sc_msg_iter: a name whose padding is not zero reads as empty", "[sc_msg_iter]") {
    // A string and an int tagged; the string's word is "ab", its terminator
    // and an 'X' where the padding belongs.
    const Args a = Args(",si").raw("ab\0X", 4).i(5);
    auto bytes = a.exact();
    sc_msg_iter msg(a.size(), bytes.get());
    const int32* name = msg.gets4();
    REQUIRE(name != nullptr);
    CHECK(*name == 0);
    CHECK(msg.geti() == 5);
}

TEST_CASE("sc_msg_iter: a name whose last word runs past the message reads as empty", "[sc_msg_iter]") {
    // A string tagged; "ab" and its terminator end the message one byte short
    // of a whole word.
    const Args a = Args(",s").raw("ab\0", 3);
    auto bytes = a.exact();
    sc_msg_iter msg(a.size(), bytes.get());
    const int32* name = msg.gets4();
    REQUIRE(name != nullptr);
    CHECK(*name == 0);
    CHECK(msg.remain() == 0);
}

TEST_CASE("sc_msg_iter: a blob longer than the message is not read", "[sc_msg_iter]") {
    // A blob tagged, claiming 1000 bytes and carrying four.
    const Args a = Args(",b").i(1000).raw("abcd", 4);
    auto bytes = a.exact();
    sc_msg_iter msg(a.size(), bytes.get());
    CHECK(msg.getbsize() == 0);
    msg.skipb();
    CHECK(msg.remain() == 0);
}

TEST_CASE("sc_msg_iter: type tags with no end inside the message carry no arguments", "[sc_msg_iter]") {
    const char tags[] = {',', 'i', 'i', 'i'};
    auto bytes = std::make_unique<char[]>(sizeof tags);
    std::memcpy(bytes.get(), tags, sizeof tags);
    sc_msg_iter msg(sizeof tags, bytes.get());
    CHECK(msg.remain() == 0);
    CHECK(msg.geti(-1) == -1);
}

TEST_CASE("sc_msg_iter: an empty message carries no arguments", "[sc_msg_iter]") {
    auto bytes = std::make_unique<char[]>(1);
    sc_msg_iter msg(0, bytes.get() + 1);
    CHECK(msg.remain() == 0);
    CHECK(msg.geti(-1) == -1);
}

TEST_CASE("sc_msg_iter: a well-formed message reads as before", "[sc_msg_iter]") {
    const Args a = Args(",isfb").i(42).str("amp").i(0x3e800000).i(3).raw("xyz\0", 4);
    auto bytes = a.exact();
    sc_msg_iter msg(a.size(), bytes.get());
    CHECK(msg.geti() == 42);
    CHECK(std::string(msg.gets()) == "amp");
    CHECK(msg.getf() == 0.25f);
    REQUIRE(msg.getbsize() == 3);
    char blob[3];
    msg.getb(blob, sizeof blob);
    CHECK(std::memcmp(blob, "xyz", 3) == 0);
    CHECK(msg.remain() == 0);
}

TEST_CASE("sc_msg_iter: a float read as an int is held to the int's range", "[sc_msg_iter]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Args a = Args(",ffff").i(bitsOf(1e20f)).i(bitsOf(-1e20f)).i(bitsOf(nan)).i(bitsOf(-7.9f));
    auto bytes = a.exact();
    sc_msg_iter msg(a.size(), bytes.get());
    CHECK(msg.geti() == std::numeric_limits<int32_t>::max());
    CHECK(msg.geti() == std::numeric_limits<int32_t>::min());
    CHECK(msg.geti() == 0);
    CHECK(msg.geti() == -7);
}

TEST_CASE("sc_msg_iter: a blob read as a number counts as one argument", "[sc_msg_iter]") {
    const Args a = Args(",bi").i(4).raw("abcd", 4).i(9);
    auto bytes = a.exact();
    sc_msg_iter msg(a.size(), bytes.get());
    CHECK(msg.geti(-1) == -1);
    CHECK(msg.geti(-1) == 9);
    CHECK(msg.remain() == 0);
}
