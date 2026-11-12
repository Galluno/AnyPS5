#include "prx/libcurl/libcurl.hpp"
#include "prx/libc/include/ApplicationHeap.hpp"
#include "prx/libc/include/Api.hpp"
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <source_location>
#include <string>

static void Require(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(stderr, "curl format check failed at line %u\n", static_cast<unsigned>(location.line()));
        std::abort();
    }
}

struct Expected {
    const char* text;
    std::source_location location;

    Expected(const char* value, std::source_location where = std::source_location::current()) : text(value), location(where) {}
};

template <typename... TArguments>
static void Check(Expected expected, const char* format, TArguments... arguments) {
    std::array<char, 512> buffer;
    buffer.fill('#');
    const int count = curl_msnprintf_nid_postfix(buffer.data(), buffer.size(), format, arguments...);
    if (count != static_cast<int>(std::strlen(expected.text)) || std::strcmp(buffer.data(), expected.text) != 0) {
        std::fprintf(stderr, "curl format mismatch at line %u: \"%s\" (%d)\n", static_cast<unsigned>(expected.location.line()), buffer.data(), count);
        std::abort();
    }
    std::array<char, 512> unbounded;
    Require(curl_msprintf_nid_postfix(unbounded.data(), format, arguments...) == count, expected.location);
    Require(std::strcmp(unbounded.data(), expected.text) == 0, expected.location);
}

static int APS5_VABI BoundedList(char* buffer, size_t size, const char* format, ...) {
#ifdef _WIN32
    __builtin_sysv_va_list args;
    __builtin_sysv_va_start(args, format);
    const int result = curl_mvsnprintf_nid_postfix(buffer, size, format, reinterpret_cast<VaList*>(args));
    __builtin_sysv_va_end(args);
#else
    std::va_list args;
    va_start(args, format);
    const int result = curl_mvsnprintf_nid_postfix(buffer, size, format, reinterpret_cast<VaList*>(args));
    va_end(args);
#endif
    return result;
}

static void CheckIntegers() {
    Check("42", "%d", 42);
    Check("   42|42   |00042", "%5d|%-5d|%05d", 42, 42, 42);
    Check("+5  5 -5", "%+d % d %+d", 5, 5, -5);
    Check("ff FF 0xff 0XFF 010 10", "%x %X %#x %#X %#o %o", 255, 255, 255, 255, 8, 8);
    Check("0x0", "%#x", 0);
    Check("", "%.0d", 0);
    Check("0042", "%.4d", 42);
    Check("  0042", "%6.4d", 42);
    Check("ffffffff", "%x", -1);
    Check("1777777777777777777777", "%o", -1);
    Check("4294967295", "%u", -1);
    Check("ffffffffffffffff", "%lx", std::int64_t{-1});
    Check("-9223372036854775808", "%ld", std::numeric_limits<std::int64_t>::min());
    Check("18446744073709551615", "%lu", std::numeric_limits<std::uint64_t>::max());
    Check("18446744073709551615 -1 5000000000 5000000000", "%llu %qd %zu %Od", std::numeric_limits<std::uint64_t>::max(), std::int64_t{-1},
        std::uint64_t{5000000000}, std::int64_t{5000000000});
    Check("70000", "%hd", 70000);
    Check("1 2 3 4 5 6 7 8 9", "%d %d %d %d %d %d %d %d %d", 1, 2, 3, 4, 5, 6, 7, 8, 9);
    Check("   42|42   |0042|ab", "%*d|%*d|%.*d|%.*s", 5, 42, -5, 42, 4, 42, 2, "abc");
    Check("42", "%.*d", -3, 42);
}

static void CheckText() {
    Check("A|  A|A  |", "%c|%3c|%-3c|", 'A', 'A', 'A');
    Check("text", "%s", "text");
    Check("(nil)||(nil)|(nil)", "%s|%.3s|%.5s|%S", nullptr, nullptr, nullptr, nullptr);
    Check("\"ab\"|\"   ab\"|\"ab   \"", "%S|%5S|%-5S", "ab", "ab", "ab");
    Check("     ab|ab    |a", "%10.5s|%-6s|%.1s", "ab", "ab", "abc");
    Check("0x1234|(nil)|(nil)   |   (nil)|", "%p|%p|%8p|%-8p|", reinterpret_cast<void*>(std::uintptr_t{0x1234}), nullptr, nullptr, nullptr);
    Check("100%", "100%%");
    Check("100", "100%");
    Check("ab7", "a%yb%d", nullptr, 7);
    Check("b a", "%2$s %1$s", "a", "b");
    Check("b a c", "%2$s %1$s %s", "a", "b", "c");
    Check("     7", "%1$*2$d", 6, 7);
}

static void CheckFloatingPoint() {
    Check("1.500000", "%f", 1.5);
    Check(" 3.14|1.500   |", "%5.2f|%-8.3f|", 3.14159, 1.5);
    Check("+1.2e+04 1.2E+04", "%+.1e %.1E", 12345.678, 12345.678);
    Check("0.5 1e+10 1E+10", "%g %g %G", 0.5, 1e10, 1e10);
    Check("1 2 3 4 5 6 7 8 9 10", "%.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f", 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0);
    Check("7 2.5 text 8", "%d %.1f %s %d", 7, 2.5, "text", 8);
    Check("  1.50", "%*.*f", 6, 2, 1.5);
}

static void CheckCounts() {
    int count = -1;
    std::int64_t wide = -1;
    std::array<std::int16_t, 2> narrow{-1, -1};
    Check("abcdefg", "abc%nde%lnf%hng", &count, &wide, narrow.data());
    Require(count == 3 && wide == 5 && narrow[0] == 6 && narrow[1] == -1);
}

static void CheckBounds() {
    std::array<char, 8> buffer;
    buffer.fill('#');
    Require(curl_msnprintf_nid_postfix(buffer.data(), 4, "%s", "abcdef") == 3);
    Require(std::memcmp(buffer.data(), "abc\0####", 8) == 0);
    buffer.fill('#');
    Require(curl_msnprintf_nid_postfix(buffer.data(), 4, "abc") == 3);
    Require(std::memcmp(buffer.data(), "abc\0####", 8) == 0);
    buffer.fill('#');
    Require(curl_msnprintf_nid_postfix(buffer.data(), 0, "abc") == 0);
    Require(curl_msnprintf_nid_postfix(nullptr, 0, "abc") == 0);
    Require(buffer[0] == '#');
    Require(curl_msnprintf_nid_postfix(buffer.data(), 1, "abc") == 0 && buffer[0] == 0 && buffer[1] == '#');
    int count = -1;
    Require(curl_msnprintf_nid_postfix(buffer.data(), 3, "abcd%n", &count) == 2 && count == -1);
    Require(BoundedList(buffer.data(), buffer.size(), "%d-%s", 12, "ab") == 5);
    Require(std::strcmp(buffer.data(), "12-ab") == 0);
    Require(BoundedList(buffer.data(), buffer.size(), "%s%s", "abcde", "fghij") == 7);
    Require(std::strcmp(buffer.data(), "abcdefg") == 0);

    std::string tooMany;
    for (int index = 0; index < 129; ++index) tooMany += "%%%c";
    buffer.fill('#');
    Require(curl_msnprintf_nid_postfix(buffer.data(), buffer.size(), tooMany.c_str()) == -1 && buffer[0] == '#');
    Require(curl_maprintf_nid_postfix(tooMany.c_str()) == nullptr);
}

static void CheckAllocated() {
    char* text = curl_maprintf_nid_postfix("%s-%d-%.1f", "ab", 12, 0.5);
    Require(text != nullptr && std::strcmp(text, "ab-12-0.5") == 0);
    free_nid_postfix(text);
    text = curl_maprintf_nid_postfix("");
    Require(text != nullptr && text[0] == 0);
    free_nid_postfix(text);
    text = curl_maprintf_nid_postfix("%s", "");
    Require(text != nullptr && text[0] == 0);
    free_nid_postfix(text);
    text = curl_maprintf_nid_postfix("%7999999d", 1);
    Require(text != nullptr && std::strlen(text) == 7999999 && text[0] == ' ' && text[7999998] == '1');
    free_nid_postfix(text);
    Require(curl_maprintf_nid_postfix("%8000000d", 1) == nullptr);
}

static void CheckStreams() {
    const char* path = "guest_curl_format.tmp";
    auto* stream = fopen_nid_postfix(path, "w+b");
    Require(stream != nullptr);
    Require(curl_mfprintf_nid_postfix(stream, "%s=%05d;%c", "key", 42, 0) == 11);
    rewind_nid_postfix(stream);
    std::array<char, 16> contents{};
    Require(fread_nid_postfix(contents.data(), 1, contents.size(), stream) == 11);
    Require(std::memcmp(contents.data(), "key=00042;\0", 11) == 0);
    Require(fclose_nid_postfix(stream) == 0);
    Require(std::remove(path) == 0);
    Require(curl_mprintf_nid_postfix("%s %d\n", "curl format", 3) == 14);
}

int main() {
    ApplicationHeapInitialize_nid_no_patch(ApplicationEmptyProcessParameters_nid_no_patch());
    CheckIntegers();
    CheckText();
    CheckFloatingPoint();
    CheckCounts();
    CheckBounds();
    CheckAllocated();
    CheckStreams();
    return 0;
}
