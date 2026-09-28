#include "sfnt_reader.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace {

constexpr int kFieldTypeString = 8;
constexpr int kFieldTypeStringWide = 11;
constexpr int kNoMoreFields = 0;
constexpr int kNoSuchField = -1;
constexpr int kFileError = -2;
constexpr int kFieldEmpty = -3;

constexpr std::array<const char*, 4> kFieldNames{
    "Family",
    "Style",
    "Full Name",
    "PostScript Name",
};

class WinHandle {
public:
    WinHandle() = default;
    explicit WinHandle(HANDLE value) noexcept : value_(value) {}
    ~WinHandle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }

    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;

    WinHandle(WinHandle&& other) noexcept
        : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}

    WinHandle& operator=(WinHandle&& other) noexcept {
        if (this != &other) {
            if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
                CloseHandle(value_);
            }
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

class MappedFile {
public:
    explicit MappedFile(const wchar_t* path)
        : file_(CreateFileW(
              path,
              GENERIC_READ,
              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
              nullptr,
              OPEN_EXISTING,
              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
              nullptr)) {
        if (!file_.valid()) {
            return;
        }

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_.get(), &size) || size.QuadPart <= 0 ||
            static_cast<unsigned long long>(size.QuadPart) >
                static_cast<unsigned long long>((std::numeric_limits<std::size_t>::max)())) {
            return;
        }
        size_ = static_cast<std::size_t>(size.QuadPart);

        mapping_ = WinHandle(CreateFileMappingW(
            file_.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
        if (!mapping_.valid()) {
            return;
        }

        data_ = static_cast<const std::byte*>(
            MapViewOfFile(mapping_.get(), FILE_MAP_READ, 0, 0, 0));
    }

    ~MappedFile() {
        if (data_ != nullptr) {
            UnmapViewOfFile(data_);
        }
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return valid() ? std::span<const std::byte>(data_, size_)
                       : std::span<const std::byte>();
    }

private:
    WinHandle file_;
    WinHandle mapping_;
    const std::byte* data_{nullptr};
    std::size_t size_{0};
};

struct ThreadCache {
    explicit ThreadCache(const wchar_t* file_name)
        : path(file_name), file(file_name), reader(file.bytes()) {}

    std::wstring path;
    MappedFile file;
    fonttrace::SfntReader reader;
    std::array<bool, kFieldNames.size()> parsed{};
    std::array<std::optional<std::wstring>, kFieldNames.size()> values{};
};

thread_local std::unique_ptr<ThreadCache> g_cache;

[[nodiscard]] fonttrace::NameField to_name_field(int field_index) noexcept {
    return static_cast<fonttrace::NameField>(field_index);
}

[[nodiscard]] int copy_result(
    const std::wstring& value,
    void* field_value,
    int max_length) noexcept {
    if (field_value == nullptr || max_length < static_cast<int>(sizeof(wchar_t))) {
        return kFileError;
    }

    const std::size_t capacity =
        static_cast<std::size_t>(max_length) / sizeof(wchar_t);
    const std::size_t length = (std::min)(value.size(), capacity - 1);
    auto* destination = static_cast<wchar_t*>(field_value);
    if (length != 0) {
        std::memcpy(destination, value.data(), length * sizeof(wchar_t));
    }
    destination[length] = L'\0';
    return kFieldTypeStringWide;
}

[[nodiscard]] int get_value_wide(
    const wchar_t* file_name,
    int field_index,
    void* field_value,
    int max_length) {
    if (file_name == nullptr || field_index < 0 ||
        field_index >= static_cast<int>(kFieldNames.size())) {
        return kNoSuchField;
    }

    if (!g_cache || g_cache->path != file_name) {
        g_cache = std::make_unique<ThreadCache>(file_name);
    }
    if (!g_cache->file.valid() || !g_cache->reader.valid()) {
        return kFileError;
    }

    const auto index = static_cast<std::size_t>(field_index);
    if (!g_cache->parsed[index]) {
        g_cache->values[index] = g_cache->reader.read_name(
            to_name_field(field_index), GetUserDefaultLangID());
        g_cache->parsed[index] = true;
    }

    if (!g_cache->values[index] || g_cache->values[index]->empty()) {
        return kFieldEmpty;
    }
    return copy_result(*g_cache->values[index], field_value, max_length);
}

[[nodiscard]] std::optional<std::wstring> ansi_to_wide(const char* text) {
    if (text == nullptr) {
        return std::nullopt;
    }
    const int length = static_cast<int>(std::strlen(text));
    if (length == 0) {
        return std::wstring();
    }
    const int wide_length = MultiByteToWideChar(
        CP_ACP, 0, text, length, nullptr, 0);
    if (wide_length <= 0) {
        return std::nullopt;
    }
    std::wstring result(static_cast<std::size_t>(wide_length), L'\0');
    if (MultiByteToWideChar(
            CP_ACP, 0, text, length, result.data(), wide_length) != wide_length) {
        return std::nullopt;
    }
    return result;
}

}  // namespace

#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:ContentGetDetectString=_ContentGetDetectString@8")
#pragma comment(linker, "/EXPORT:ContentGetSupportedField=_ContentGetSupportedField@16")
#pragma comment(linker, "/EXPORT:ContentGetValue=_ContentGetValue@24")
#pragma comment(linker, "/EXPORT:ContentGetValueW=_ContentGetValueW@24")
#pragma comment(linker, "/EXPORT:ContentPluginUnloading=_ContentPluginUnloading@0")
#define WDX_EXPORT extern "C"
#else
#define WDX_EXPORT extern "C" __declspec(dllexport)
#endif

WDX_EXPORT void __stdcall ContentGetDetectString(
    char* detect_string,
    int max_length) noexcept {
    constexpr char kDetect[] =
        "EXT=\"OTC\"|EXT=\"OTF\"|EXT=\"OTB\"|EXT=\"TTC\"|EXT=\"TTF\"";
    if (detect_string == nullptr || max_length <= 0) {
        return;
    }
    const auto length = (std::min)(
        sizeof(kDetect) - 1,
        static_cast<std::size_t>(max_length - 1));
    std::memcpy(detect_string, kDetect, length);
    detect_string[length] = '\0';
}

WDX_EXPORT int __stdcall ContentGetSupportedField(
    int field_index,
    char* field_name,
    char* units,
    int max_length) noexcept {
    if (field_index < 0 ||
        field_index >= static_cast<int>(kFieldNames.size())) {
        return kNoMoreFields;
    }
    if (field_name == nullptr || units == nullptr || max_length <= 0) {
        return kNoMoreFields;
    }

    units[0] = '\0';
    const auto* name = kFieldNames[static_cast<std::size_t>(field_index)];
    const auto length = (std::min)(
        std::strlen(name), static_cast<std::size_t>(max_length - 1));
    std::memcpy(field_name, name, length);
    field_name[length] = '\0';
    return kFieldTypeString;
}

WDX_EXPORT int __stdcall ContentGetValueW(
    wchar_t* file_name,
    int field_index,
    int /*unit_index*/,
    void* field_value,
    int max_length,
    int /*flags*/) noexcept {
    try {
        return get_value_wide(
            file_name, field_index, field_value, max_length);
    } catch (...) {
        return kFileError;
    }
}

WDX_EXPORT int __stdcall ContentGetValue(
    char* file_name,
    int field_index,
    int /*unit_index*/,
    void* field_value,
    int max_length,
    int /*flags*/) noexcept {
    try {
        const auto wide_name = ansi_to_wide(file_name);
        if (!wide_name) {
            return kFileError;
        }
        return get_value_wide(
            wide_name->c_str(), field_index, field_value, max_length);
    } catch (...) {
        return kFileError;
    }
}

WDX_EXPORT void __stdcall ContentPluginUnloading() noexcept {
    g_cache.reset();
}
