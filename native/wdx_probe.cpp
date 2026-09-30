#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr int kFieldTypeStringWide = 11;

using GetSupportedFieldFn = int(__stdcall*)(int, char*, char*, int);
using GetValueWFn = int(__stdcall*)(wchar_t*, int, int, void*, int, int);
using PluginUnloadingFn = void(__stdcall*)();

[[nodiscard]] std::string utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        size,
        nullptr,
        nullptr);
    return result;
}

[[nodiscard]] bool is_font_file(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return extension == L".ttf" || extension == L".otf" ||
           extension == L".ttc" || extension == L".otc" ||
           extension == L".otb";
}

[[nodiscard]] std::vector<std::filesystem::path> collect_fonts(
    const std::filesystem::path& input) {
    std::vector<std::filesystem::path> result;
    std::error_code error;
    if (std::filesystem::is_regular_file(input, error)) {
        if (is_font_file(input)) {
            result.push_back(std::filesystem::absolute(input, error));
        }
        return result;
    }
    if (!std::filesystem::is_directory(input, error)) {
        return result;
    }

    for (std::filesystem::recursive_directory_iterator iterator(input, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        if (iterator->is_regular_file(error) && is_font_file(iterator->path())) {
            result.push_back(iterator->path());
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

template <typename Function>
[[nodiscard]] Function load_function(HMODULE module, const char* name) {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr
            << "usage: wdx_probe <fonttrace.wdx[64]> <font-file-or-folder> "
               "[exact-postscript-name]\n";
        return 2;
    }

    const HMODULE module = LoadLibraryW(argv[1]);
    if (module == nullptr) {
        std::cerr << "LoadLibrary failed: " << GetLastError() << '\n';
        return 1;
    }

    const auto get_supported =
        load_function<GetSupportedFieldFn>(module, "ContentGetSupportedField");
    const auto get_value =
        load_function<GetValueWFn>(module, "ContentGetValueW");
    const auto unloading =
        load_function<PluginUnloadingFn>(module, "ContentPluginUnloading");
    if (get_supported == nullptr || get_value == nullptr || unloading == nullptr) {
        std::cerr << "required WDX export is missing\n";
        FreeLibrary(module);
        return 1;
    }

    std::array<std::string, 5> field_names;
    for (int field = 0; field < static_cast<int>(field_names.size()); ++field) {
        std::array<char, 128> name{};
        std::array<char, 16> units{};
        const int type = get_supported(
            field,
            name.data(),
            units.data(),
            static_cast<int>(name.size()));
        if (type <= 0) {
            std::cerr << "field enumeration failed at index " << field << '\n';
            unloading();
            FreeLibrary(module);
            return 1;
        }
        field_names[static_cast<std::size_t>(field)] = name.data();
    }

    auto fonts = collect_fonts(argv[2]);
    if (fonts.empty()) {
        std::cerr << "no supported font files found\n";
        unloading();
        FreeLibrary(module);
        return 1;
    }

    std::cout << "files: " << fonts.size() << '\n';
    std::cout << "sample: " << utf8(fonts.front().wstring()) << '\n';

    if (argc == 4) {
        const std::wstring wanted = argv[3];
        std::size_t readable = 0;
        std::size_t matches = 0;
        std::array<wchar_t, 2048> value{};
        const auto started = std::chrono::steady_clock::now();
        for (const auto& font : fonts) {
            std::wstring path = font.wstring();
            const int type = get_value(
                path.data(),
                3,
                0,
                value.data(),
                static_cast<int>(value.size() * sizeof(wchar_t)),
                0);
            if (type != kFieldTypeStringWide) {
                continue;
            }
            ++readable;
            if (_wcsicmp(value.data(), wanted.c_str()) == 0) {
                ++matches;
                std::cout << "match: " << utf8(font.wstring()) << '\n';
            }
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started);
        std::cout << "PostScript exact search: readable=" << readable << '/'
                  << fonts.size() << " matches=" << matches << "  "
                  << std::fixed << std::setprecision(2) << elapsed.count()
                  << " ms\n";
        unloading();
        FreeLibrary(module);
        return matches == 0 ? 3 : 0;
    }

    for (int field = 0; field < static_cast<int>(field_names.size()); ++field) {
        std::array<wchar_t, 2048> value{};
        std::wstring path = fonts.front().wstring();
        const int type = get_value(
            path.data(),
            field,
            0,
            value.data(),
            static_cast<int>(value.size() * sizeof(wchar_t)),
            0);
        std::cout << field_names[static_cast<std::size_t>(field)] << ": ";
        if (type == kFieldTypeStringWide) {
            std::cout << utf8(value.data());
        } else {
            std::cout << "<error " << type << ">";
        }
        std::cout << '\n';
    }

    for (int field = 0; field < static_cast<int>(field_names.size()); ++field) {
        std::size_t found = 0;
        std::vector<std::pair<std::filesystem::path, int>> failures;
        std::array<wchar_t, 2048> value{};
        const auto started = std::chrono::steady_clock::now();
        for (const auto& font : fonts) {
            std::wstring path = font.wstring();
            const int type = get_value(
                path.data(),
                field,
                0,
                value.data(),
                static_cast<int>(value.size() * sizeof(wchar_t)),
                0);
            if (type == kFieldTypeStringWide) {
                ++found;
            } else {
                failures.emplace_back(font, type);
            }
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started);
        const double rate = elapsed.count() > 0.0
                                ? static_cast<double>(fonts.size()) * 1000.0 /
                                      elapsed.count()
                                : 0.0;
        std::cout << std::left << std::setw(16)
                  << field_names[static_cast<std::size_t>(field)] << std::right
                  << " found=" << found << '/' << fonts.size()
                  << "  " << std::fixed << std::setprecision(2)
                  << elapsed.count() << " ms  " << std::setprecision(0)
                  << rate << " files/s\n";
        for (const auto& [font, error] : failures) {
            std::cout << "  error=" << error
                      << "  " << utf8(font.wstring()) << '\n';
        }
    }

    unloading();
    FreeLibrary(module);
    return 0;
}
