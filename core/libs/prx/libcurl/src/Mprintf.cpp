#include "prx/libcurl/include/InventoryExports.hpp"
#include "prx/libc/include/Api.hpp"
#include "prx/libc/include/FileStream.hpp"
#include "SceTypes.hpp"

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#define CURL_FORMAT_ARGS_START(last) __builtin_sysv_va_list args; __builtin_sysv_va_start(args, last)
#define CURL_FORMAT_ARGS_END() __builtin_sysv_va_end(args)
#else
#define CURL_FORMAT_ARGS_START(last) std::va_list args; va_start(args, last)
#define CURL_FORMAT_ARGS_END() va_end(args)
#endif

namespace {

constexpr std::int64_t MaxParameters = 128;
constexpr std::int64_t WorkSize = 326;
constexpr std::size_t AllocatedLimit = 8000000;

enum class ArgumentType { Unknown, String, Pointer, Integer, CountPointer, Double, Width };

enum : std::uint32_t {
    FlagSpace = 1u << 0,
    FlagShowSign = 1u << 1,
    FlagLeft = 1u << 2,
    FlagAlternate = 1u << 3,
    FlagShort = 1u << 4,
    FlagLong = 1u << 5,
    FlagLongLong = 1u << 6,
    FlagLongDouble = 1u << 7,
    FlagPadZero = 1u << 8,
    FlagUnsigned = 1u << 9,
    FlagOctal = 1u << 10,
    FlagHex = 1u << 11,
    FlagUpper = 1u << 12,
    FlagWidth = 1u << 13,
    FlagWidthParameter = 1u << 14,
    FlagPrecision = 1u << 15,
    FlagPrecisionParameter = 1u << 16,
    FlagChar = 1u << 17,
    FlagFloatE = 1u << 18,
    FlagFloatG = 1u << 19
};

struct Parameter {
    ArgumentType type = ArgumentType::Unknown;
    std::uint32_t flags = 0;
    std::int64_t width = 0;
    std::int64_t precision = 0;
    std::uint64_t bits = 0;
    double real = 0;
};

class GuestArguments {
    VaList* args;

    const void* Overflow() {
        const void* address = args->overflow_arg_area;
        args->overflow_arg_area = static_cast<char*>(args->overflow_arg_area) + 8;
        return address;
    }

public:
    explicit GuestArguments(VaList* source) : args(source) {}

    std::uint64_t Integer() {
        const void* address;
        if (args->gp_offset < 48) {
            address = static_cast<const char*>(args->reg_save_area) + args->gp_offset;
            args->gp_offset += 8;
        } else {
            address = Overflow();
        }
        std::uint64_t value;
        std::memcpy(&value, address, sizeof(value));
        return value;
    }

    std::int64_t Int() {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(Integer()));
    }

    double Double() {
        const void* address;
        if (args->fp_offset < 176) {
            address = static_cast<const char*>(args->reg_save_area) + args->fp_offset;
            args->fp_offset += 16;
        } else {
            address = Overflow();
        }
        double value;
        std::memcpy(&value, address, sizeof(value));
        return value;
    }
};

bool IsDigit(char value) {
    return value >= '0' && value <= '9';
}

std::int64_t ParseLong(const char*& text) {
    const char* cursor = text;
    while (*cursor == ' ' || (*cursor >= '\t' && *cursor <= '\r')) ++cursor;
    bool negative = false;
    if (*cursor == '+' || *cursor == '-') negative = *cursor++ == '-';
    if (!IsDigit(*cursor)) return 0;
    const std::uint64_t limit = negative ? std::uint64_t{1} << 63 : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    std::uint64_t value = 0;
    bool saturated = false;
    for (; IsDigit(*cursor); ++cursor) {
        const auto digit = static_cast<std::uint64_t>(*cursor - '0');
        if (saturated || value > (limit - digit) / 10) saturated = true;
        else value = value * 10 + digit;
    }
    text = cursor;
    if (saturated) value = limit;
    return negative ? static_cast<std::int64_t>(0 - value) : static_cast<std::int64_t>(value);
}

std::int64_t PositionalNumber(const char*& input) {
    const char* cursor = input;
    int number = 0;
    for (; IsDigit(*cursor); ++cursor) {
        if (number < MaxParameters) number = number * 10 + (*cursor - '0');
    }
    if (number <= MaxParameters && *cursor == '$') {
        input = cursor + 1;
        return number;
    }
    return 0;
}

bool IsQualifier(char value) {
    return value != 0 && std::strchr("-+ #.0123456789hlLzq*O", value) != nullptr;
}

bool ValidIndex(std::int64_t index) {
    return index >= 0 && index < MaxParameters;
}

using Parameters = std::array<Parameter, MaxParameters>;

bool Collect(const char* format, Parameters& parameters, std::vector<const char*>& ends, GuestArguments& arguments) {
    const char* cursor = format;
    std::int64_t parameterNumber = 0;
    std::int64_t maxParameter = 0;
    while (*cursor) {
        if (*cursor++ != '%') continue;
        if (*cursor == '%') {
            ++cursor;
            continue;
        }
        std::uint32_t flags = 0;
        ++parameterNumber;
        std::int64_t thisParameter = PositionalNumber(cursor);
        if (thisParameter == 0) thisParameter = parameterNumber;
        if (thisParameter > maxParameter) maxParameter = thisParameter;
        std::int64_t width = 0;
        std::int64_t precision = 0;
        while (IsQualifier(*cursor)) {
            switch (*cursor++) {
            case ' ':
                flags |= FlagSpace;
                break;
            case '+':
                flags |= FlagShowSign;
                break;
            case '-':
                flags |= FlagLeft;
                flags &= ~FlagPadZero;
                break;
            case '#':
                flags |= FlagAlternate;
                break;
            case '.':
                if (*cursor == '*') {
                    flags |= FlagPrecisionParameter;
                    ++cursor;
                    ++parameterNumber;
                    const std::int64_t position = PositionalNumber(cursor);
                    precision = position ? position : parameterNumber;
                    if (precision > maxParameter) maxParameter = precision;
                } else {
                    flags |= FlagPrecision;
                    precision = ParseLong(cursor);
                }
                break;
            case 'h':
                flags |= FlagShort;
                break;
            case 'l':
                flags |= (flags & FlagLong) ? FlagLongLong : FlagLong;
                break;
            case 'L':
                flags |= FlagLongDouble;
                break;
            case 'q':
                flags |= FlagLongLong;
                break;
            case 'z':
            case 'O':
                flags |= FlagLong;
                break;
            case '0':
                if (!(flags & FlagLeft)) flags |= FlagPadZero;
                [[fallthrough]];
            case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
                flags |= FlagWidth;
                --cursor;
                width = ParseLong(cursor);
                break;
            case '*': {
                flags |= FlagWidthParameter;
                ++parameterNumber;
                const std::int64_t position = PositionalNumber(cursor);
                width = position ? position : parameterNumber;
                if (width > maxParameter) maxParameter = width;
                break;
            }
            default:
                break;
            }
        }
        const std::int64_t index = thisParameter - 1;
        if (!ValidIndex(index)) return false;
        auto& parameter = parameters[static_cast<std::size_t>(index)];
        switch (*cursor) {
        case 'S':
            flags |= FlagAlternate;
            [[fallthrough]];
        case 's':
            parameter.type = ArgumentType::String;
            break;
        case 'n':
            parameter.type = ArgumentType::CountPointer;
            break;
        case 'p':
            parameter.type = ArgumentType::Pointer;
            break;
        case 'd': case 'i':
            parameter.type = ArgumentType::Integer;
            break;
        case 'u':
            parameter.type = ArgumentType::Integer;
            flags |= FlagUnsigned;
            break;
        case 'o':
            parameter.type = ArgumentType::Integer;
            flags |= FlagOctal;
            break;
        case 'x':
            parameter.type = ArgumentType::Integer;
            flags |= FlagHex | FlagUnsigned;
            break;
        case 'X':
            parameter.type = ArgumentType::Integer;
            flags |= FlagHex | FlagUpper | FlagUnsigned;
            break;
        case 'c':
            parameter.type = ArgumentType::Integer;
            flags |= FlagChar;
            break;
        case 'f':
            parameter.type = ArgumentType::Double;
            break;
        case 'e':
            parameter.type = ArgumentType::Double;
            flags |= FlagFloatE;
            break;
        case 'E':
            parameter.type = ArgumentType::Double;
            flags |= FlagFloatE | FlagUpper;
            break;
        case 'g':
            parameter.type = ArgumentType::Double;
            flags |= FlagFloatG;
            break;
        case 'G':
            parameter.type = ArgumentType::Double;
            flags |= FlagFloatG | FlagUpper;
            break;
        default:
            parameter.type = ArgumentType::Unknown;
            break;
        }
        parameter.flags = flags;
        parameter.width = width;
        parameter.precision = precision;
        const auto bind = [&](std::int64_t position, std::int64_t& slot) {
            const std::int64_t holder = position - 1;
            if (!ValidIndex(holder)) return false;
            slot = holder;
            auto& target = parameters[static_cast<std::size_t>(holder)];
            target.type = ArgumentType::Width;
            target.flags = 0;
            target.width = 0;
            target.precision = 0;
            return true;
        };
        if ((flags & FlagWidthParameter) && !bind(width, parameter.width)) return false;
        if ((flags & FlagPrecisionParameter) && !bind(precision, parameter.precision)) return false;
        ends.push_back(cursor + (*cursor ? 1 : 0));
    }
    for (std::int64_t index = 0; index < maxParameter; ++index) {
        auto& parameter = parameters[static_cast<std::size_t>(index)];
        if (parameter.flags & FlagWidthParameter)
            parameters[static_cast<std::size_t>(parameter.width)].bits = static_cast<std::uint64_t>(arguments.Int());
        if (parameter.flags & FlagPrecisionParameter)
            parameters[static_cast<std::size_t>(parameter.precision)].bits = static_cast<std::uint64_t>(arguments.Int());
        switch (parameter.type) {
        case ArgumentType::String:
        case ArgumentType::CountPointer:
        case ArgumentType::Unknown:
        case ArgumentType::Pointer:
            parameter.bits = arguments.Integer();
            break;
        case ArgumentType::Integer:
            if (parameter.flags & (FlagLongLong | FlagLong)) parameter.bits = arguments.Integer();
            else if (parameter.flags & FlagUnsigned) parameter.bits = static_cast<std::uint32_t>(arguments.Integer());
            else parameter.bits = static_cast<std::uint64_t>(arguments.Int());
            break;
        case ArgumentType::Double:
            parameter.real = arguments.Double();
            break;
        case ArgumentType::Width:
            parameter.type = ArgumentType::Integer;
            break;
        }
    }
    return true;
}

template <typename TSink>
class Formatter {
    TSink& sink;
    Parameters parameters{};
    std::vector<const char*> ends;
    int done = 0;

    bool Out(char value) {
        if (!sink(static_cast<unsigned char>(value))) return false;
        ++done;
        return true;
    }

    bool Pad(std::int64_t& width, char value) {
        while (width-- > 0) {
            if (!Out(value)) return false;
        }
        return true;
    }

    bool Character(const Parameter& parameter, std::int64_t width) {
        if (!(parameter.flags & FlagLeft)) {
            while (--width > 0) {
                if (!Out(' ')) return false;
            }
        }
        if (!Out(static_cast<char>(parameter.bits))) return false;
        if (parameter.flags & FlagLeft) {
            while (--width > 0) {
                if (!Out(' ')) return false;
            }
        }
        return true;
    }

    bool Number(const Parameter& parameter, std::uint64_t number, std::uint64_t base, bool negative, bool alternate, std::int64_t width, std::int64_t precision) {
        const char* digits = (parameter.flags & FlagUpper) ? "0123456789ABCDEF" : "0123456789abcdef";
        std::array<char, WorkSize> work;
        const std::int64_t workEnd = WorkSize - 2;
        std::int64_t position = workEnd;
        if (precision == -1) precision = 1;
        while (number > 0) {
            work[static_cast<std::size_t>(position--)] = digits[number % base];
            number /= base;
        }
        width -= workEnd - position;
        precision -= workEnd - position;
        if (alternate && base == 8 && precision <= 0) {
            work[static_cast<std::size_t>(position--)] = '0';
            --width;
        }
        if (precision > 0) {
            width -= precision;
            while (precision-- > 0 && position >= 0) work[static_cast<std::size_t>(position--)] = '0';
        }
        if (alternate && base == 16) width -= 2;
        if (negative || (parameter.flags & (FlagShowSign | FlagSpace))) --width;
        if (!(parameter.flags & (FlagLeft | FlagPadZero)) && !Pad(width, ' ')) return false;
        if (negative) {
            if (!Out('-')) return false;
        } else if (parameter.flags & FlagShowSign) {
            if (!Out('+')) return false;
        } else if (parameter.flags & FlagSpace) {
            if (!Out(' ')) return false;
        }
        if (alternate && base == 16) {
            if (!Out('0') || !Out((parameter.flags & FlagUpper) ? 'X' : 'x')) return false;
        }
        if (!(parameter.flags & FlagLeft) && (parameter.flags & FlagPadZero) && !Pad(width, '0')) return false;
        while (++position <= workEnd) {
            if (!Out(work[static_cast<std::size_t>(position)])) return false;
        }
        return !(parameter.flags & FlagLeft) || Pad(width, ' ');
    }

    bool Integer(const Parameter& parameter, bool alternate, std::int64_t width, std::int64_t precision) {
        if (parameter.flags & FlagChar) return Character(parameter, width);
        if (parameter.flags & FlagOctal) return Number(parameter, parameter.bits, 8, false, alternate, width, precision);
        if (parameter.flags & FlagHex) return Number(parameter, parameter.bits, 16, false, alternate, width, precision);
        if (parameter.flags & FlagUnsigned) return Number(parameter, parameter.bits, 10, false, alternate, width, precision);
        const bool negative = static_cast<std::int64_t>(parameter.bits) < 0;
        return Number(parameter, negative ? 0 - parameter.bits : parameter.bits, 10, negative, alternate, width, precision);
    }

    bool String(Parameter& parameter, std::int64_t width, std::int64_t precision) {
        const char* text = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(parameter.bits));
        std::size_t length;
        if (!text) {
            if (precision == -1 || precision >= 5) {
                text = "(nil)";
                length = 5;
                parameter.flags &= ~FlagAlternate;
            } else {
                text = "";
                length = 0;
            }
        } else if (precision != -1) {
            length = static_cast<std::size_t>(precision);
        } else {
            length = std::strlen(text);
        }
        const auto longMax = static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max());
        width = static_cast<std::int64_t>(static_cast<std::uint64_t>(width) - (length > longMax ? longMax : length));
        if ((parameter.flags & FlagAlternate) && !Out('"')) return false;
        if (!(parameter.flags & FlagLeft) && !Pad(width, ' ')) return false;
        for (; length && *text; --length) {
            if (!Out(*text++)) return false;
        }
        if ((parameter.flags & FlagLeft) && !Pad(width, ' ')) return false;
        return !(parameter.flags & FlagAlternate) || Out('"');
    }

    bool Pointer(const Parameter& parameter, std::int64_t width, std::int64_t precision) {
        if (parameter.bits) return Number(parameter, parameter.bits, 16, false, true, width, precision);
        width -= 5;
        if ((parameter.flags & FlagLeft) && !Pad(width, ' ')) return false;
        for (const char* text = "(nil)"; *text; ++text) {
            if (!Out(*text)) return false;
        }
        return (parameter.flags & FlagLeft) || Pad(width, ' ');
    }

    bool Double(const Parameter& parameter) {
        std::int64_t width = -1;
        if (parameter.flags & FlagWidth) width = parameter.width;
        else if (parameter.flags & FlagWidthParameter) width = static_cast<std::int64_t>(parameters[static_cast<std::size_t>(parameter.width)].bits);
        std::int64_t precision = -1;
        if (parameter.flags & FlagPrecision) precision = parameter.precision;
        else if (parameter.flags & FlagPrecisionParameter) precision = static_cast<std::int64_t>(parameters[static_cast<std::size_t>(parameter.precision)].bits);
        std::string conversion = "%";
        if (parameter.flags & FlagLeft) conversion += '-';
        if (parameter.flags & FlagShowSign) conversion += '+';
        if (parameter.flags & FlagSpace) conversion += ' ';
        if (parameter.flags & FlagAlternate) conversion += '#';
        if (width >= 0) {
            if (width >= WorkSize) width = WorkSize - 1;
            conversion += std::to_string(width);
        }
        if (precision >= 0) {
            std::uint64_t maxPrecision = WorkSize - 2;
            double value = parameter.real;
            if (width > 0 && precision <= width) maxPrecision -= static_cast<std::uint64_t>(width);
            if (value > std::numeric_limits<double>::max())
                throw std::runtime_error("curl formatting of positive infinity with a precision does not terminate in curl 7.81.0");
            while (value >= 10.0) {
                value /= 10;
                --maxPrecision;
            }
            if (precision > static_cast<std::int64_t>(maxPrecision)) precision = static_cast<std::int64_t>(maxPrecision) - 1;
            if (precision < 0) precision = 0;
            conversion += '.';
            conversion += std::to_string(precision);
        }
        if (parameter.flags & FlagLong) conversion += 'l';
        if (parameter.flags & FlagFloatE) conversion += (parameter.flags & FlagUpper) ? 'E' : 'e';
        else if (parameter.flags & FlagFloatG) conversion += (parameter.flags & FlagUpper) ? 'G' : 'g';
        else conversion += 'f';
        std::array<char, WorkSize> work;
        const int length = snprintf_nid_postfix(work.data(), work.size(), conversion.c_str(), parameter.real);
        if (length < 0 || length >= WorkSize) throw std::length_error("curl floating-point conversion exceeds the 326 byte work buffer");
        for (int index = 0; index < length; ++index) {
            if (!Out(work[static_cast<std::size_t>(index)])) return false;
        }
        return true;
    }

    void Count(const Parameter& parameter) {
        void* target = reinterpret_cast<void*>(static_cast<std::uintptr_t>(parameter.bits));
        if (parameter.flags & (FlagLongLong | FlagLong)) {
            const auto value = static_cast<std::int64_t>(done);
            std::memcpy(target, &value, sizeof(value));
        } else if (!(parameter.flags & FlagShort)) {
            const auto value = static_cast<std::int32_t>(done);
            std::memcpy(target, &value, sizeof(value));
        } else {
            const auto value = static_cast<std::int16_t>(done);
            std::memcpy(target, &value, sizeof(value));
        }
    }

public:
    explicit Formatter(TSink& output) : sink(output) {}

    int Run(const char* format, VaList* args) {
        GuestArguments arguments(args);
        if (!Collect(format, parameters, ends, arguments)) return -1;
        std::size_t end = 0;
        std::int64_t parameterNumber = 0;
        const char* cursor = format;
        while (*cursor) {
            if (*cursor != '%') {
                do {
                    if (!Out(*cursor)) return done;
                } while (*++cursor && *cursor != '%');
                continue;
            }
            ++cursor;
            if (*cursor == '%') {
                ++cursor;
                if (!Out('%')) return done;
                continue;
            }
            std::int64_t index = PositionalNumber(cursor);
            index = index ? index - 1 : parameterNumber;
            ++parameterNumber;
            if (!ValidIndex(index) || end == ends.size())
                throw std::out_of_range("curl format string reads past the parameter table of curl 7.81.0");
            auto& parameter = parameters[static_cast<std::size_t>(index)];
            std::int64_t width;
            if (parameter.flags & FlagWidthParameter) {
                width = static_cast<std::int64_t>(parameters[static_cast<std::size_t>(parameter.width)].bits);
                ++parameterNumber;
                if (width < 0) {
                    width = -width;
                    parameter.flags |= FlagLeft;
                    parameter.flags &= ~FlagPadZero;
                }
            } else {
                width = parameter.width;
            }
            std::int64_t precision;
            if (parameter.flags & FlagPrecisionParameter) {
                precision = static_cast<std::int64_t>(parameters[static_cast<std::size_t>(parameter.precision)].bits);
                ++parameterNumber;
                if (precision < 0) precision = -1;
            } else if (parameter.flags & FlagPrecision) {
                precision = parameter.precision;
            } else {
                precision = -1;
            }
            const bool alternate = (parameter.flags & FlagAlternate) != 0;
            bool more = true;
            switch (parameter.type) {
            case ArgumentType::Integer:
                more = Integer(parameter, alternate, width, precision);
                break;
            case ArgumentType::String:
                more = String(parameter, width, precision);
                break;
            case ArgumentType::Pointer:
                more = Pointer(parameter, width, precision);
                break;
            case ArgumentType::Double:
                more = Double(parameter);
                break;
            case ArgumentType::CountPointer:
                Count(parameter);
                break;
            default:
                break;
            }
            if (!more) return done;
            cursor = ends[end++];
        }
        return done;
    }
};

template <typename TSink>
int Format(TSink& sink, const char* format, VaList* args) {
    return Formatter<TSink>(sink).Run(format, args);
}

struct BoundedSink {
    char* buffer;
    std::size_t length;
    std::size_t max;

    bool operator()(unsigned char value) {
        if (length >= max) return false;
        *buffer++ = static_cast<char>(value);
        ++length;
        return true;
    }
};

struct UnboundedSink {
    char* buffer;

    bool operator()(unsigned char value) {
        *buffer++ = static_cast<char>(value);
        return true;
    }
};

struct StreamSink {
    FileStream* stream;

    bool operator()(unsigned char value) {
        return fputc_nid_postfix(value, stream) != -1;
    }
};

struct AllocatedSink {
    std::string text;
    bool failed = false;

    bool operator()(unsigned char value) {
        if (text.size() + 2 > AllocatedLimit) {
            failed = true;
            return false;
        }
        text.push_back(static_cast<char>(value));
        return true;
    }
};

int FormatBounded(char* buffer, std::size_t maxLength, const char* format, VaList* args) {
    BoundedSink sink{buffer, 0, maxLength};
    int result = Format(sink, format, args);
    if (result != -1 && sink.max) {
        if (sink.max == sink.length) {
            sink.buffer[-1] = 0;
            --result;
        } else {
            sink.buffer[0] = 0;
        }
    }
    return result;
}

int FormatUnbounded(char* buffer, const char* format, VaList* args) {
    UnboundedSink sink{buffer};
    const int result = Format(sink, format, args);
    *sink.buffer = 0;
    return result;
}

int FormatStream(FileStream* stream, const char* format, VaList* args) {
    StreamSink sink{stream};
    return Format(sink, format, args);
}

char* FormatAllocated(const char* format, VaList* args) {
    try {
        AllocatedSink sink;
        const int result = Format(sink, format, args);
        if (result == -1 || sink.failed) return nullptr;
        if (sink.text.empty()) return strdup_nid_postfix("");
        auto* output = static_cast<char*>(malloc_nid_postfix(sink.text.size() + 1));
        if (!output) return nullptr;
        std::memcpy(output, sink.text.c_str(), sink.text.size() + 1);
        return output;
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

}

extern "C" {

int APS5_VABI curl_mvfprintf_nid_postfix(FileStream* whereto, const char* format, VaList* args) {
    return FormatStream(whereto, format, args);
}

int APS5_VABI curl_mfprintf_nid_postfix(FileStream* whereto, const char* format, ...) {
    CURL_FORMAT_ARGS_START(format);
    const int result = FormatStream(whereto, format, reinterpret_cast<VaList*>(args));
    CURL_FORMAT_ARGS_END();
    return result;
}

int APS5_VABI curl_mvsprintf_nid_postfix(char* buffer, const char* format, VaList* args) {
    return FormatUnbounded(buffer, format, args);
}

int APS5_VABI curl_msprintf_nid_postfix(char* buffer, const char* format, ...) {
    CURL_FORMAT_ARGS_START(format);
    const int result = FormatUnbounded(buffer, format, reinterpret_cast<VaList*>(args));
    CURL_FORMAT_ARGS_END();
    return result;
}

int APS5_VABI curl_mvsnprintf_nid_postfix(char* buffer, size_t maxlength, const char* format, VaList* args) {
    return FormatBounded(buffer, maxlength, format, args);
}

int APS5_VABI curl_msnprintf_nid_postfix(char* buffer, size_t maxlength, const char* format, ...) {
    CURL_FORMAT_ARGS_START(format);
    const int result = FormatBounded(buffer, maxlength, format, reinterpret_cast<VaList*>(args));
    CURL_FORMAT_ARGS_END();
    return result;
}

int APS5_VABI curl_mvprintf_nid_postfix(const char* format, VaList* args) {
    return FormatStream(&_Stdout_nid_postfix, format, args);
}

int APS5_VABI curl_mprintf_nid_postfix(const char* format, ...) {
    CURL_FORMAT_ARGS_START(format);
    const int result = FormatStream(&_Stdout_nid_postfix, format, reinterpret_cast<VaList*>(args));
    CURL_FORMAT_ARGS_END();
    return result;
}

char* APS5_VABI curl_mvaprintf_nid_postfix(const char* format, VaList* args) {
    return FormatAllocated(format, args);
}

char* APS5_VABI curl_maprintf_nid_postfix(const char* format, ...) {
    CURL_FORMAT_ARGS_START(format);
    char* result = FormatAllocated(format, reinterpret_cast<VaList*>(args));
    CURL_FORMAT_ARGS_END();
    return result;
}

}
