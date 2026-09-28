#include <Windows.h>

#include <array>
#include <cstring>
#include <iostream>
#include <string_view>

namespace {

using GetDetectStringFn = void(__stdcall*)(char*, int);
using GetSupportedFieldFn = int(__stdcall*)(int, char*, char*, int);

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

    constexpr std::array<const char*, 4> kFields{
        "Family",
        "Style",
        "Full Name",
        "PostScript Name",
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
            get_supported(4,
                          name.data(),
                          units.data(),
                          static_cast<int>(name.size())) == 0,
            "field enumeration must stop after four fields");
    }

    FreeLibrary(module);
    if (passed) {
        std::cout << "wdx_exports_tests: all tests passed\n";
    }
    return passed ? 0 : 1;
}
