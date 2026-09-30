#include "sfnt_reader.h"

#include <Windows.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct TestRecord {
    std::uint16_t platform;
    std::uint16_t encoding;
    std::uint16_t language;
    std::uint16_t name_id;
    std::vector<std::byte> value;
};

void append_u16(std::vector<std::byte>& data, std::uint16_t value) {
    data.push_back(static_cast<std::byte>(value >> 8U));
    data.push_back(static_cast<std::byte>(value & 0xFFU));
}

void append_u32(std::vector<std::byte>& data, std::uint32_t value) {
    data.push_back(static_cast<std::byte>(value >> 24U));
    data.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
    data.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    data.push_back(static_cast<std::byte>(value & 0xFFU));
}

std::vector<std::byte> utf16_be(std::wstring_view text) {
    std::vector<std::byte> result;
    result.reserve(text.size() * 2);
    for (const wchar_t ch : text) {
        append_u16(result, static_cast<std::uint16_t>(ch));
    }
    return result;
}

std::vector<std::byte> encode_codepage(std::wstring_view text, UINT codepage) {
    const int length = WideCharToMultiByte(
        codepage,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    assert(length > 0);
    std::vector<char> encoded(static_cast<std::size_t>(length));
    assert(WideCharToMultiByte(
               codepage,
               0,
               text.data(),
               static_cast<int>(text.size()),
               encoded.data(),
               length,
               nullptr,
               nullptr) == length);

    std::vector<std::byte> result(encoded.size());
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        result[i] = static_cast<std::byte>(
            static_cast<unsigned char>(encoded[i]));
    }
    return result;
}

std::vector<std::byte> make_font(const std::vector<TestRecord>& records) {
    std::vector<std::byte> name;
    append_u16(name, 0);
    append_u16(name, static_cast<std::uint16_t>(records.size()));
    const auto storage_offset = static_cast<std::uint16_t>(6 + records.size() * 12);
    append_u16(name, storage_offset);

    std::uint16_t string_offset = 0;
    for (const auto& record : records) {
        append_u16(name, record.platform);
        append_u16(name, record.encoding);
        append_u16(name, record.language);
        append_u16(name, record.name_id);
        append_u16(name, static_cast<std::uint16_t>(record.value.size()));
        append_u16(name, string_offset);
        string_offset = static_cast<std::uint16_t>(
            string_offset + record.value.size());
    }
    for (const auto& record : records) {
        name.insert(name.end(), record.value.begin(), record.value.end());
    }

    std::vector<std::byte> font;
    append_u32(font, 0x00010000U);
    append_u16(font, 1);
    append_u16(font, 0);
    append_u16(font, 0);
    append_u16(font, 0);
    append_u32(font, 0x6E616D65U);  // name
    append_u32(font, 0);
    append_u32(font, 28);
    append_u32(font, static_cast<std::uint32_t>(name.size()));
    font.insert(font.end(), name.begin(), name.end());
    return font;
}

void test_preferred_chinese_family() {
    const auto font = make_font({
        {3, 1, 0x0409, 1, utf16_be(L"English Family")},
        {3, 1, 0x0804, 1, utf16_be(L"中文字体")},
    });
    const fonttrace::SfntReader reader(font);
    assert(reader.valid());
    assert(reader.read_name(fonttrace::NameField::family, 0x0804) == L"中文字体");
}

void test_typographic_family_overrides_legacy_family() {
    const auto font = make_font({
        {3, 1, 0x0804, 1, utf16_be(L"旧字体族")},
        {3, 1, 0x0804, 16, utf16_be(L"排印字体族")},
    });
    const fonttrace::SfntReader reader(font);
    assert(reader.read_name(fonttrace::NameField::family, 0x0804) == L"排印字体族");
}

void test_irrelevant_broken_record_is_not_decoded() {
    auto font = make_font({
        {3, 1, 0x0804, 4, utf16_be(L"损坏的全名")},
        {3, 1, 0x0804, 1, utf16_be(L"可用字体族")},
    });

    // name table begins at byte 28; first record length is at 28+6+8.
    const std::size_t broken_length = 42;
    font[broken_length] = std::byte{0xFF};
    font[broken_length + 1] = std::byte{0xFF};

    const fonttrace::SfntReader reader(font);
    assert(reader.read_name(fonttrace::NameField::family, 0x0804) == L"可用字体族");
    assert(!reader.read_name(fonttrace::NameField::full_name, 0x0804));
}

void test_clean_gb2312_repairs_bad_unicode_name() {
    const auto font = make_font({
        {3, 1, 0x0804, 1, utf16_be(L"乱码测试字体名称")},
        {3, 3, 0x0804, 1, encode_codepage(L"文鼎粗黑简", 936)},
    });
    const fonttrace::SfntReader reader(font);
    assert(reader.read_name(fonttrace::NameField::family, 0x0804) == L"文鼎粗黑简");
}

void test_version_name() {
    const auto font = make_font({
        {3, 1, 0x0409, 5, utf16_be(L"Version 1.234; Build 20260929")},
    });
    const fonttrace::SfntReader reader(font);
    assert(reader.read_name(fonttrace::NameField::version, 0x0804) ==
           L"Version 1.234; Build 20260929");
}

void test_english_fallback_beats_unrelated_localization() {
    const auto font = make_font({
        {3, 1, 0x0409, 1, utf16_be(L"Malgun Gothic")},
        {3, 1, 0x0412, 1, utf16_be(L"맑은 고딕")},
        {3, 5, 0x0412, 1, encode_codepage(L"맑은 고딕", 949)},
    });
    const fonttrace::SfntReader reader(font);
    assert(reader.read_name(fonttrace::NameField::family, 0x0804) ==
           L"Malgun Gothic");
    assert(reader.read_name(fonttrace::NameField::family, 0x0412) ==
           L"맑은 고딕");
}

}  // namespace

int wmain() {
    test_preferred_chinese_family();
    test_typographic_family_overrides_legacy_family();
    test_irrelevant_broken_record_is_not_decoded();
    test_clean_gb2312_repairs_bad_unicode_name();
    test_version_name();
    test_english_fallback_beats_unrelated_localization();
    std::wcout << L"sfnt_reader_tests: all tests passed\n";
    return 0;
}
