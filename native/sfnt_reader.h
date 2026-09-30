#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace fonttrace {

enum class NameField : std::uint8_t {
    family,
    style,
    full_name,
    postscript_name,
    version,
};

class SfntReader {
public:
    explicit SfntReader(std::span<const std::byte> file) noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }

    // preferred_language is a Windows LANGID, for example 0x0804 for
    // Simplified Chinese or 0x0409 for English (United States).
    [[nodiscard]] std::optional<std::wstring> read_name(
        NameField field,
        std::uint16_t preferred_language) const;

private:
    std::span<const std::byte> name_table_{};
    bool valid_{false};
};

}  // namespace fonttrace
