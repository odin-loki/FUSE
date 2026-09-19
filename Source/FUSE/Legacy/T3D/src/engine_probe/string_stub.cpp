// Minimal Torque String for FUSE_T3D_LEGACY_ENGINE_PROBE (hashFunction.cpp getStringHash64).
// Satisfies only the methods exercised by the probe batch — not a full str.cpp substitute.

#include "core/util/str.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <algorithm>
#include <cctype>

namespace
{

String::SizeType clampPos(const String& s, String::SizeType pos)
{
    if (pos > s.length()) {
        return s.length();
    }
    return pos;
}

} // namespace

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

String& String::operator=(const StringChar* str) {
    delete _string;
    _string = new StringData(str, str ? static_cast<SizeType>(std::strlen(str)) : 0);
    return *this;
}

String& String::operator=(StringChar c) {
    delete _string;
    char text[2] = {c, '\0'};
    _string = new StringData(text, 1);
    return *this;
}

bool String::operator==(const String& str) const {
    return _string->value == str._string->value;
}

String& String::erase(SizeType pos, SizeType len) {
    pos = clampPos(*this, pos);
    if (len == static_cast<SizeType>(-1) || pos + len > length()) {
        len = length() - pos;
    }
    _string->value.erase(pos, len);
    return *this;
}

S32 String::compare(const char* str1, const char* str2) {
    if (str1 == nullptr || str2 == nullptr) {
        return str1 == str2 ? 0 : (str1 ? 1 : -1);
    }
    return static_cast<S32>(std::strcmp(str1, str2));
}

S32 String::compare(const UTF16* /*str1*/, const UTF16* /*str2*/) {
    return 0;
}

String String::VToString(const char* format, va_list args) {
    char buffer[512];
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    return String(buffer);
}

String::SizeType String::find(StringChar c, SizeType pos, U32 mode) const
{
    pos = clampPos(*this, pos);
    const char* text = c_str();
    if (mode & Right) {
        if (length() == 0) {
            return NPos;
        }
        const SizeType start = length() - 1;
        for (SizeType i = start; i >= pos; --i) {
            if (text[i] == c) {
                return i;
            }
            if (i == 0) {
                break;
            }
        }
        return NPos;
    }

    for (SizeType i = pos; i < length(); ++i) {
        if (text[i] == c) {
            return i;
        }
    }
    return NPos;
}

String::SizeType String::find(const StringChar* str, SizeType pos, U32 mode) const
{
    if (str == nullptr || str[0] == '\0') {
        return NPos;
    }
    pos = clampPos(*this, pos);
    const char* hay = c_str();
    const SizeType needleLen = static_cast<SizeType>(std::strlen(str));

    if (mode & Right) {
        if (length() < needleLen) {
            return NPos;
        }
        const SizeType start = length() - needleLen;
        for (SizeType i = start; i >= pos; --i) {
            if (std::strncmp(hay + i, str, needleLen) == 0) {
                return i;
            }
            if (i == 0) {
                break;
            }
        }
        return NPos;
    }

    for (SizeType i = pos; i + needleLen <= length(); ++i) {
        if (std::strncmp(hay + i, str, needleLen) == 0) {
            return i;
        }
    }
    return NPos;
}

String::SizeType String::find(const String& str, SizeType pos, U32 mode) const
{
    return find(str.c_str(), pos, mode);
}

String String::substr(SizeType pos, SizeType len) const
{
    pos = clampPos(*this, pos);
    if (len == static_cast<SizeType>(-1) || pos + len > length()) {
        len = length() - pos;
    }
    return String(c_str() + pos, len);
}

String& String::replace(SizeType pos, SizeType len, const StringChar* str)
{
    pos = clampPos(*this, pos);
    if (len == static_cast<SizeType>(-1) || pos + len > length()) {
        len = length() - pos;
    }
    _string->value.replace(pos, len, str ? str : "");
    return *this;
}

String& String::replace(SizeType pos, SizeType len, const String& str)
{
    return replace(pos, len, str.c_str());
}

String& String::replace(StringChar c1, StringChar c2)
{
    for (char& ch : _string->value) {
        if (ch == c1) {
            ch = c2;
        }
    }
    return *this;
}

bool String::equal(const String& str, U32 mode) const
{
    if (mode & NoCase) {
        if (_string->value.size() != str._string->value.size()) {
            return false;
        }
        for (SizeType i = 0; i < length(); ++i) {
            if (std::tolower(static_cast<unsigned char>((*this)[i])) !=
                std::tolower(static_cast<unsigned char>(str[i]))) {
                return false;
            }
        }
        return true;
    }
    return _string->value == str._string->value;
}

S32 String::compare(const StringChar* str, SizeType len, U32 mode) const
{
    if (str == nullptr) {
        return isEmpty() ? 0 : 1;
    }
    const SizeType otherLen = len ? len : static_cast<SizeType>(std::strlen(str));
    const SizeType cmpLen = std::min(length(), otherLen);
    S32 result = 0;
    if (mode & NoCase) {
        for (SizeType i = 0; i < cmpLen; ++i) {
            const int a = std::tolower(static_cast<unsigned char>((*this)[i]));
            const int b = std::tolower(static_cast<unsigned char>(str[i]));
            if (a != b) {
                result = a - b;
                break;
            }
        }
    } else {
        result = std::strncmp(c_str(), str, cmpLen);
    }
    if (result != 0) {
        return result;
    }
    if (length() == otherLen) {
        return 0;
    }
    return length() < otherLen ? -1 : 1;
}

S32 String::compare(const String& str, SizeType len, U32 mode) const
{
    return compare(str.c_str(), len, mode);
}

String& String::insert(SizeType pos, const StringChar* str)
{
    return insert(pos, str, str ? static_cast<SizeType>(std::strlen(str)) : 0);
}

String& String::insert(SizeType pos, const StringChar* str, SizeType len)
{
    pos = clampPos(*this, pos);
    _string->value.insert(pos, str ? str : "", len);
    return *this;
}

String& String::insert(SizeType pos, const String& str)
{
    return insert(pos, str.c_str(), str.length());
}

String& String::operator+=(StringChar c)
{
    _string->value.push_back(c);
    return *this;
}

String& String::operator+=(const StringChar* str)
{
    if (str != nullptr) {
        _string->value.append(str);
    }
    return *this;
}

String& String::operator+=(const String& str)
{
    _string->value.append(str._string->value);
    return *this;
}

String operator+(const String& a, const String& b)
{
    String out(a);
    out += b;
    return out;
}

String operator+(const String& a, StringChar c)
{
    String out(a);
    out += c;
    return out;
}

String operator+(StringChar c, const String& a)
{
    String out;
    out += c;
    out += a;
    return out;
}

String operator+(const String& a, const StringChar* b)
{
    String out(a);
    out += b;
    return out;
}

String operator+(const StringChar* a, const String& b)
{
    String out(a);
    out += b;
    return out;
}

bool String::operator==(StringChar c) const
{
    return length() == 1 && c_str()[0] == c;
}
