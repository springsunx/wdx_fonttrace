#include <Windows.h>

#include <array>
#include <cstring>
#include <iostream>
#include <string_view>

namespace {

using GetDetectStringFn = void(__stdcall*)(char*, int);
using GetSupportedFieldFn = int(__stdcall*)(int, char*, char*, int);
using GetValueWFn = int(__stdcall*)(wchar_t*, int, int, void*, int, int);
using PluginUnloadingFn = void(__stdcall*)();

template <typename Function>
[[nodiscard]] Function load_function(HMODULE module, const char* name) {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

[[nodiscard]] bool require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

[[nodiscard]] bool test_file_is_released(
    GetValueWFn get_value,
    PluginUnloadingFn unloading) {
    std::array<wchar_t, MAX_PATH> temp_directory{};
    std::array<wchar_t, MAX_PATH> temp_file{};
    if (GetTempPathW(
            static_cast<DWORD>(temp_directory.size()),
            temp_directory.data()) == 0 ||
        GetTempFileNameW(
            temp_directory.data(), L"wft", 0, temp_file.data()) == 0) {
        return require(false, "temporary test file must be created");
    }

    const HANDLE handle = CreateFileW(
        temp_file.data(),
        GENERIC_WRITE,
        0,
        nullptr,
        TRUNCATE_EXISTING,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        DeleteFileW(temp_file.data());
        return require(false, "temporary test file must open");
    }

    constexpr char kContents[] = "not-a-font";
    DWORD written = 0;
    const BOOL write_ok = WriteFile(
        handle,
        kContents,
        static_cast<DWORD>(sizeof(kContents)),
        &written,
        nullptr);
    CloseHandle(handle);
    if (!write_ok || written != sizeof(kContents)) {
        DeleteFileW(temp_file.data());
        return require(false, "temporary test file must be written");
    }

    std::array<wchar_t, 64> value{};
    (void)get_value(
        temp_file.data(),
        0,
        0,
        value.data(),
        static_cast<int>(value.size() * sizeof(wchar_t)),
        0);

    const HANDLE exclusive = CreateFileW(
        temp_file.data(),
        GENERIC_READ | DELETE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    const bool released = exclusive != INVALID_HANDLE_VALUE;
    if (released) {
        CloseHandle(exclusive);
    } else {
        unloading();
    }
    const BOOL deleted = DeleteFileW(temp_file.data());
    const bool handle_ok = require(
        released,
        "plugin must release the font file before returning a field value");
    const bool delete_ok = require(
        deleted != FALSE,
        "font file must be deletable while the plugin remains loaded");
    return handle_ok && delete_ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (!require(argc == 2, "expected plugin path")) {
        return 1;
    }

    const HMODULE module = LoadLibraryW(argv[1]);
    if (!require(module != nullptr, "plugin must load")) {
        return 1;
    }

    constexpr std::array<const char*, 5> kExports{
        "ContentGetDetectString",
        "ContentGetSupportedField",
        "ContentGetValue",
        "ContentGetValueW",
        "ContentPluginUnloading",
    };
    bool passed = true;
    for (const char* name : kExports) {
        passed &= require(GetProcAddress(module, name) != nullptr, name);
    }

    const auto get_detect =
        load_function<GetDetectStringFn>(module, "ContentGetDetectString");
    const auto get_supported =
        load_function<GetSupportedFieldFn>(module, "ContentGetSupportedField");
    const auto get_value =
        load_function<GetValueWFn>(module, "ContentGetValueW");
    const auto unloading =
        load_function<PluginUnloadingFn>(module, "ContentPluginUnloading");

    if (get_detect != nullptr) {
        std::array<char, 256> detect{};
        get_detect(detect.data(), static_cast<int>(detect.size()));
        const std::string_view expression(detect.data());
        passed &= require(expression.find("TTF") != std::string_view::npos,
                          "detect string must include TTF");
        passed &= require(expression.find("OTF") != std::string_view::npos,
                          "detect string must include OTF");
        passed &= require(expression.find("TTC") != std::string_view::npos,
                          "detect string must include TTC");
    }

    constexpr std::array<const char*, 5> kFields{
        "Family",
        "Style",
        "Full Name",
        "PostScript Name",
        "Version",
    };
    if (get_supported != nullptr) {
        for (int index = 0; index < static_cast<int>(kFields.size()); ++index) {
            std::array<char, 128> name{};
            std::array<char, 16> units{};
            const int type = get_supported(
                index,
                name.data(),
                units.data(),
                static_cast<int>(name.size()));
            passed &= require(type == 8, "field type must be string");
            passed &= require(
                std::strcmp(name.data(),
                            kFields[static_cast<std::size_t>(index)]) == 0,
                "field name mismatch");
        }
        std::array<char, 8> name{};
        std::array<char, 8> units{};
        passed &= require(
            get_supported(5,
                          name.data(),
                          units.data(),
                          static_cast<int>(name.size())) == 0,
            "field enumeration must stop after five fields");
    }

    if (get_value != nullptr && unloading != nullptr) {
        passed &= test_file_is_released(get_value, unloading);
    }

    if (unloading != nullptr) {
        unloading();
    }
    FreeLibrary(module);
    if (passed) {
        std::cout << "wdx_exports_tests: all tests passed\n";
    }
    return passed ? 0 : 1;
}
