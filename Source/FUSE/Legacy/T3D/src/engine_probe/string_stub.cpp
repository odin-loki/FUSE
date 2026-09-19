// Minimal Torque String for FUSE_T3D_LEGACY_ENGINE_PROBE (hashFunction.cpp getStringHash64).
// Satisfies only the methods exercised by the probe batch — not a full str.cpp substitute.

#include "core/util/str.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>

const String::SizeType String::NPos = U32(~0);
const String String::EmptyString;

class String::StringData
{
public:
    std::string value;

    explicit StringData(const char* text, SizeType len)
        : value(text ? std::string(text, len) : std::string()) {}
};

String::String() : _string(new StringData("", 0)) {}

String::String(const String& str) : _string(new StringData(str._string->value.c_str(), str._string->value.size())) {}

String::String(const StringChar* str) : _string(new StringData(str, str ? std::strlen(str) : 0)) {}

String::String(const StringChar* str, SizeType size) : _string(new StringData(str, size)) {}

String::String(const UTF16* /*str*/) : _string(new StringData("", 0)) {}

String::~String() {
    delete _string;
}

const UTF8* String::c_str() const {
    return _string->value.c_str();
}

const UTF16* String::utf16() const {
    return nullptr;
}

String::SizeType String::length() const {
    return static_cast<SizeType>(_string->value.size());
}

String::SizeType String::size() const {
    return static_cast<SizeType>(_string->value.size() + 1);
}

String::SizeType String::numChars() const {
    return length();
}

bool String::isEmpty() const {
    return _string->value.empty();
}

String& String::operator=(const String& str) {
    if (this != &str) {
        delete _string;
        _string = new StringData(str._string->value.c_str(), str._string->value.size());
    }
    return *this;
}

String& String::replace(const String& s1, const String& s2) {
    std::string& value = _string->value;
    if (s1.isEmpty()) {
        return *this;
    }
    const std::string& needle = s1._string->value;
    const std::string& replacement = s2._string->value;
    std::size_t pos = 0;
    while ((pos = value.find(needle, pos)) != std::string::npos) {
        value.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
    return *this;
}

String String::ToString(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return String(buffer);
}

String String::VToString(const char* format, va_list args) {
    char buffer[512];
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    return String(buffer);
}
