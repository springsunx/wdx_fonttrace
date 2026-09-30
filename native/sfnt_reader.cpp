#include "sfnt_reader.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <vector>

namespace fonttrace {
namespace {

constexpr std::uint32_t make_tag(char a, char b, char c, char d) noexcept {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8U) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

constexpr std::uint32_t kTtcTag = make_tag('t', 't', 'c', 'f');
constexpr std::uint32_t kNameTag = make_tag('n', 'a', 'm', 'e');
constexpr std::uint32_t kTrueTag = make_tag('t', 'r', 'u', 'e');
constexpr std::uint32_t kTyp1Tag = make_tag('t', 'y', 'p', '1');
constexpr std::uint32_t kOttoTag = make_tag('O', 'T', 'T', 'O');

constexpr std::uint16_t kPlatformMac = 1;
constexpr std::uint16_t kPlatformWindows = 3;
constexpr std::uint16_t kEncodingMacRoman = 0;
constexpr std::uint16_t kEncodingWindowsUnicodeBmp = 1;
constexpr std::uint16_t kEncodingWindowsShiftJis = 2;
constexpr std::uint16_t kEncodingWindowsGb2312 = 3;
constexpr std::uint16_t kEncodingWindowsBig5 = 4;
constexpr std::uint16_t kEncodingWindowsWansung = 5;
constexpr std::uint16_t kEncodingWindowsJohab = 6;
constexpr std::uint16_t kEncodingWindowsUnicodeFull = 10;
constexpr std::size_t kMaxNameId = 17;

[[nodiscard]] std::optional<std::uint16_t> read_u16(
    std::span<const std::byte> data,
    std::size_t offset) noexcept {
    if (offset > data.size() || data.size() - offset < 2) {
        return std::nullopt;
    }
    const auto hi = std::to_integer<std::uint8_t>(data[offset]);
    const auto lo = std::to_integer<std::uint8_t>(data[offset + 1]);
    return static_cast<std::uint16_t>((hi << 8U) | lo);
}

[[nodiscard]] std::optional<std::uint32_t> read_u32(
    std::span<const std::byte> data,
    std::size_t offset) noexcept {
    if (offset > data.size() || data.size() - offset < 4) {
        return std::nullopt;
    }
    return (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(data[offset])) << 24U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(data[offset + 1])) << 16U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(data[offset + 2])) << 8U) |
           static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(data[offset + 3]));
}

[[nodiscard]] std::optional<std::span<const std::byte>> checked_span(
    std::span<const std::byte> data,
    std::size_t offset,
    std::size_t length) noexcept {
    if (offset > data.size() || length > data.size() - offset) {
        return std::nullopt;
    }
    return data.subspan(offset, length);
}

[[nodiscard]] bool is_sfnt_signature(std::uint32_t signature) noexcept {
    return signature == 0x00010000U || signature == 0x00020000U ||
           signature == kTrueTag || signature == kTyp1Tag ||
           signature == kOttoTag;
}

[[nodiscard]] bool is_relevant_name_id(
    NameField field,
    std::uint16_t name_id) noexcept {
    switch (field) {
    case NameField::family:
        return name_id == 1 || name_id == 16;
    case NameField::style:
        return name_id == 2 || name_id == 17;
    case NameField::full_name:
        return name_id == 4;
    case NameField::postscript_name:
        return name_id == 6;
    case NameField::version:
        return name_id == 5;
    }
    return false;
}

[[nodiscard]] bool is_windows_codepage_encoding(
    std::uint16_t encoding) noexcept {
    return encoding >= kEncodingWindowsShiftJis &&
           encoding <= kEncodingWindowsJohab;
}

[[nodiscard]] UINT windows_codepage(std::uint16_t encoding) noexcept {
    switch (encoding) {
    case kEncodingWindowsShiftJis:
        return 932;
    case kEncodingWindowsGb2312:
        return 936;
    case kEncodingWindowsBig5:
        return 950;
    case kEncodingWindowsWansung:
        return 949;
    case kEncodingWindowsJohab:
        return 1361;
    default:
        return 0;
    }
}

[[nodiscard]] bool is_printable_ascii(
    std::span<const std::byte> bytes) noexcept {
    for (const auto value : bytes) {
        const auto ch = std::to_integer<std::uint8_t>(value);
        if (ch < 32 || ch > 126) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::wstring> decode_utf16_be(
    std::span<const std::byte> bytes) {
    const std::size_t char_count = bytes.size() / 2;
    if (char_count == 0) {
        return std::nullopt;
    }

    std::wstring result(char_count, L'\0');
    for (std::size_t i = 0; i < char_count; ++i) {
        const auto hi = std::to_integer<std::uint8_t>(bytes[i * 2]);
        const auto lo = std::to_integer<std::uint8_t>(bytes[i * 2 + 1]);
        result[i] = static_cast<wchar_t>((hi << 8U) | lo);
    }
    return result;
}

[[nodiscard]] std::optional<std::wstring> decode_multibyte(
    std::span<const std::byte> bytes,
    UINT codepage) {
    if (bytes.empty() ||
        bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }

    const auto* source = reinterpret_cast<const char*>(bytes.data());
    const int source_length = static_cast<int>(bytes.size());
    const int result_length = MultiByteToWideChar(
        codepage, 0, source, source_length, nullptr, 0);
    if (result_length <= 0) {
        return std::nullopt;
    }

    std::wstring result(static_cast<std::size_t>(result_length), L'\0');
    if (MultiByteToWideChar(
            codepage,
            0,
            source,
            source_length,
            result.data(),
            result_length) != result_length) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::vector<std::byte> strip_null_padding(
    std::span<const std::byte> bytes) {
    std::vector<std::byte> result;
    result.reserve(bytes.size() / 2);
    for (std::size_t i = 1; i < bytes.size(); i += 2) {
        result.push_back(bytes[i]);
    }
    return result;
}

[[nodiscard]] bool is_clean_cjk(std::wstring_view text) noexcept {
    bool has_cjk = false;
    for (const wchar_t ch : text) {
        if (ch == L'?' || (ch >= L'a' && ch <= L'z') ||
            (ch >= L'A' && ch <= L'Z')) {
            return false;
        }
        if (ch >= static_cast<wchar_t>(0x4E00) &&
            ch <= static_cast<wchar_t>(0x9FFF)) {
            has_cjk = true;
        }
    }
    return has_cjk;
}

struct NameRecord {
    std::uint16_t platform{};
    std::uint16_t encoding{};
    std::uint16_t language{};
    std::uint16_t name_id{};
    std::uint16_t length{};
    std::uint16_t string_offset{};
};

[[nodiscard]] std::optional<NameRecord> read_record(
    std::span<const std::byte> table,
    std::size_t offset) noexcept {
    NameRecord record;
    const auto platform = read_u16(table, offset);
    const auto encoding = read_u16(table, offset + 2);
    const auto language = read_u16(table, offset + 4);
    const auto name_id = read_u16(table, offset + 6);
    const auto length = read_u16(table, offset + 8);
    const auto string_offset = read_u16(table, offset + 10);
    if (!platform || !encoding || !language || !name_id || !length ||
        !string_offset) {
        return std::nullopt;
    }
    record.platform = *platform;
    record.encoding = *encoding;
    record.language = *language;
    record.name_id = *name_id;
    record.length = *length;
    record.string_offset = *string_offset;
    return record;
}

struct DecodedName {
    std::wstring value;
    int language_priority{};
    bool unicode{};
    bool codepage{};
};

[[nodiscard]] int windows_language_priority(
    std::uint16_t language,
    std::uint16_t preferred_language) noexcept {
    constexpr std::uint16_t kPrimaryLanguageMask = 0x03FF;
    constexpr std::uint16_t kEnglishPrimaryLanguage = 0x0009;
    constexpr std::uint16_t kEnglishUnitedStates = 0x0409;

    if (language == preferred_language) {
        return 5;
    }
    if ((language & kPrimaryLanguageMask) ==
        (preferred_language & kPrimaryLanguageMask)) {
        return 4;
    }
    if (language == kEnglishUnitedStates) {
        return 3;
    }
    if ((language & kPrimaryLanguageMask) == kEnglishPrimaryLanguage) {
        return 2;
    }
    return 1;
}

[[nodiscard]] std::optional<DecodedName> decode_record(
    std::span<const std::byte> table,
    std::size_t storage_offset,
    const NameRecord& record,
    std::uint16_t preferred_language) {
    if (record.length == 0) {
        return std::nullopt;
    }
    const auto raw = checked_span(
        table,
        storage_offset + static_cast<std::size_t>(record.string_offset),
        record.length);
    if (!raw) {
        return std::nullopt;
    }

    DecodedName result;

    if (record.platform == kPlatformMac) {
        if (record.encoding != kEncodingMacRoman) {
            return std::nullopt;
        }
        const auto decoded = decode_multibyte(*raw, 10000);
        if (!decoded) {
            return std::nullopt;
        }
        result.value = *decoded;
        return result;
    }

    if (record.platform != kPlatformWindows) {
        return std::nullopt;
    }

    result.language_priority = windows_language_priority(
        record.language, preferred_language);

    if (is_windows_codepage_encoding(record.encoding)) {
        std::span<const std::byte> encoded = *raw;
        std::vector<std::byte> unpadded;
        if (encoded.size() > 2 && encoded.size() % 2 == 0 &&
            encoded.front() == std::byte{0}) {
            unpadded = strip_null_padding(encoded);
            encoded = unpadded;
        }

        const auto codepage = windows_codepage(record.encoding);
        const auto decoded = decode_multibyte(encoded, codepage);
        if (!decoded) {
            return std::nullopt;
        }
        result.value = *decoded;
        result.codepage = true;
        return result;
    }

    if (record.encoding != kEncodingWindowsUnicodeBmp &&
        record.encoding != kEncodingWindowsUnicodeFull) {
        return std::nullopt;
    }

    const auto decoded = decode_utf16_be(*raw);
    if (!decoded) {
        return std::nullopt;
    }
    result.value = *decoded;
    result.unicode = !is_printable_ascii(*raw);
    return result;
}

}  // namespace

SfntReader::SfntReader(std::span<const std::byte> file) noexcept {
    const auto signature = read_u32(file, 0);
    if (!signature) {
        return;
    }

    std::size_t face_offset = 0;
    if (*signature == kTtcTag) {
        const auto num_fonts = read_u32(file, 8);
        const auto first_face = read_u32(file, 12);
        if (!num_fonts || *num_fonts == 0 || !first_face) {
            return;
        }
        face_offset = *first_face;
    }

    const auto face_signature = read_u32(file, face_offset);
    const auto num_tables = read_u16(file, face_offset + 4);
    if (!face_signature || !is_sfnt_signature(*face_signature) ||
        !num_tables || *num_tables == 0) {
        return;
    }

    constexpr std::size_t kOffsetTableSize = 12;
    constexpr std::size_t kDirectoryEntrySize = 16;
    const std::size_t directory_offset = face_offset + kOffsetTableSize;
    if (directory_offset > file.size() ||
        static_cast<std::size_t>(*num_tables) >
            (file.size() - directory_offset) / kDirectoryEntrySize) {
        return;
    }

    for (std::size_t i = 0; i < *num_tables; ++i) {
        const std::size_t entry = directory_offset + i * kDirectoryEntrySize;
        const auto tag = read_u32(file, entry);
        if (!tag || *tag != kNameTag) {
            continue;
        }

        const auto offset = read_u32(file, entry + 8);
        const auto length = read_u32(file, entry + 12);
        if (!offset || !length) {
            return;
        }
        const auto table = checked_span(file, *offset, *length);
        if (!table) {
            return;
        }
        name_table_ = *table;
        valid_ = true;
        return;
    }
}

std::optional<std::wstring> SfntReader::read_name(
    NameField field,
    std::uint16_t preferred_language) const {
    if (!valid_) {
        return std::nullopt;
    }

    const auto count = read_u16(name_table_, 2);
    const auto storage_offset_word = read_u16(name_table_, 4);
    if (!count || *count == 0 || !storage_offset_word) {
        return std::nullopt;
    }

    constexpr std::size_t kHeaderSize = 6;
    constexpr std::size_t kRecordSize = 12;
    const std::size_t storage_offset = *storage_offset_word;
    if (name_table_.size() < kHeaderSize ||
        static_cast<std::size_t>(*count) >
            (name_table_.size() - kHeaderSize) / kRecordSize ||
        storage_offset > name_table_.size()) {
        return std::nullopt;
    }

    std::array<int, kMaxNameId + 1> best_language_priority{};
    best_language_priority.fill(-1);
    std::array<bool, kMaxNameId + 1> best_unicode{};
    std::array<std::optional<std::wstring>, kMaxNameId + 1> best_value{};
    std::array<int, kMaxNameId + 1> codepage_language_priority{};
    codepage_language_priority.fill(-1);
    std::array<std::optional<std::wstring>, kMaxNameId + 1> codepage_fallback{};

    for (std::size_t i = 0; i < *count; ++i) {
        const auto record = read_record(
            name_table_, kHeaderSize + i * kRecordSize);
        if (!record || record->name_id > kMaxNameId ||
            !is_relevant_name_id(field, record->name_id)) {
            continue;
        }

        const auto decoded = decode_record(
            name_table_, storage_offset, *record, preferred_language);
        if (!decoded) {
            continue;
        }

        const auto id = static_cast<std::size_t>(record->name_id);
        if (decoded->codepage && is_clean_cjk(decoded->value) &&
            decoded->language_priority > codepage_language_priority[id]) {
            codepage_fallback[id] = decoded->value;
            codepage_language_priority[id] = decoded->language_priority;
        }

        const bool better_language =
            decoded->language_priority > best_language_priority[id];
        const bool better_encoding =
            decoded->language_priority == best_language_priority[id] &&
            decoded->unicode && !best_unicode[id];
        if (!best_value[id] || better_language || better_encoding) {
            best_value[id] = decoded->value;
            best_language_priority[id] = decoded->language_priority;
            best_unicode[id] = decoded->unicode;
        }
    }

    std::size_t selected_id = 0;
    switch (field) {
    case NameField::family:
        selected_id = best_value[16] ? 16 : 1;
        break;
    case NameField::style:
        selected_id = best_value[17] ? 17 : 2;
        break;
    case NameField::full_name:
        selected_id = 4;
        break;
    case NameField::postscript_name:
        selected_id = 6;
        break;
    case NameField::version:
        selected_id = 5;
        break;
    }

    auto selected = best_value[selected_id];
    const auto& fallback = codepage_fallback[selected_id];
    if (fallback &&
        codepage_language_priority[selected_id] >=
            best_language_priority[selected_id] &&
        is_clean_cjk(*fallback) &&
        selected && fallback->size() <= selected->size()) {
        selected = *fallback;
    }

    if (!selected || selected->empty()) {
        return std::nullopt;
    }
    return selected;
}

}  // namespace fonttrace
