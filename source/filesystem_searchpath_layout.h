#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace FileSystemLayout
{
// Linux32 public keeps the priority group after the flags; dev/prerelease
// moved it immediately after the store ID. SetSearchPathIsTrustedSource starts
// by clearing a flag in its CSearchPath argument, which identifies both ABIs
// without interpreting a group number as a pointer. Reject unrecognized code.
inline std::size_t Linux32PathIDOffset(const void* code, std::size_t size)
{
    constexpr std::uint8_t prefix[] = {
        0x55, 0x89, 0xE5, 0x57, 0x56, 0x53, 0x83, 0xEC, 0x2C,
        0x8B, 0x45, 0x0C, 0x8B, 0x75, 0x08, 0xC6, 0x40
    };
    if (!code || size < sizeof(prefix) + 2 ||
        std::memcmp(code, prefix, sizeof(prefix)) != 0)
        return 0;

    const auto* bytes = static_cast<const std::uint8_t*>(code);
    if (bytes[18] != 0)
        return 0;
    if (bytes[17] == 0x09)
        return 4; // public: pointer +4, flags +8
    if (bytes[17] == 0x0D)
        return 8; // dev/prerelease: group +4, pointer +8, flags +12
    return 0;
}
}
