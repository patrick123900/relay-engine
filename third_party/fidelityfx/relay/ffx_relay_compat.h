// Relay: portable stand-ins for the Microsoft C runtime "secure" string functions that the
// FidelityFX SDK 1.1.4 host code calls. Force-included into the FidelityFX sources on platforms
// other than Windows; not part of the upstream SDK.
#pragma once

#ifndef _WIN32


#ifndef FFX_UNUSED
#define FFX_UNUSED(x) ((void)(x))
#endif

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif

inline int wcscpy_s(wchar_t* destination, std::size_t size, const wchar_t* source)
{
    if (destination == nullptr || size == 0) return 1;
    if (source == nullptr) { destination[0] = L'\0'; return 1; }
    std::size_t length = std::wcslen(source);
    if (length >= size) length = size - 1;
    std::wmemcpy(destination, source, length);
    destination[length] = L'\0';
    return 0;
}

template <std::size_t Size>
int wcscpy_s(wchar_t (&destination)[Size], const wchar_t* source)
{
    return wcscpy_s(destination, Size, source);
}

inline int strcpy_s(char* destination, std::size_t size, const char* source)
{
    if (destination == nullptr || size == 0) return 1;
    if (source == nullptr) { destination[0] = '\0'; return 1; }
    std::size_t length = std::strlen(source);
    if (length >= size) length = size - 1;
    std::memcpy(destination, source, length);
    destination[length] = '\0';
    return 0;
}

template <std::size_t Size>
int strcpy_s(char (&destination)[Size], const char* source)
{
    return strcpy_s(destination, Size, source);
}

inline int sprintf_s(char* buffer, std::size_t size, const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return written;
}

inline int swprintf_s(wchar_t* buffer, std::size_t size, const wchar_t* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vswprintf(buffer, size, format, arguments);
    va_end(arguments);
    return written;
}

// Converts at most `count` bytes of output and always terminates `destination`.
inline int wcstombs_s(std::size_t* converted, char* destination, std::size_t size,
                      const wchar_t* source, std::size_t count)
{
    if (destination == nullptr || size == 0) return 1;
    std::size_t limit = count < size ? count : size - 1;
    std::size_t written = 0;
    for (; written < limit && source != nullptr && source[written] != L'\0'; ++written) {
        const auto character = static_cast<unsigned long>(source[written]);
        destination[written] = character < 0x80 ? static_cast<char>(character) : '?';
    }
    destination[written] = '\0';
    if (converted != nullptr) *converted = written + 1;
    return 0;
}

#endif // _WIN32
