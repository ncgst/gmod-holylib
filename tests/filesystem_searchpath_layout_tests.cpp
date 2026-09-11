#include "filesystem_searchpath_layout.h"
#include <array>
#include <cassert>
#include <iostream>

int main()
{
    // SetSearchPathIsTrustedSource prefixes from the GLuaTest public and
    // dev/prerelease dedicated_srv.so binaries (September 2026).
    std::array<std::uint8_t, 19> publicCode = {
        0x55, 0x89, 0xE5, 0x57, 0x56, 0x53, 0x83, 0xEC, 0x2C,
        0x8B, 0x45, 0x0C, 0x8B, 0x75, 0x08, 0xC6, 0x40, 0x09, 0x00
    };
    auto devCode = publicCode;
    devCode[17] = 0x0D;
    assert(FileSystemLayout::Linux32PathIDOffset(publicCode.data(), publicCode.size()) == 4);
    assert(FileSystemLayout::Linux32PathIDOffset(devCode.data(), devCode.size()) == 8);
    assert(FileSystemLayout::Linux32PathIDOffset(nullptr, publicCode.size()) == 0);
    for (std::size_t size = 0; size < publicCode.size(); ++size)
        assert(FileSystemLayout::Linux32PathIDOffset(publicCode.data(), size) == 0);

    for (std::size_t index = 0; index < publicCode.size(); ++index)
    {
        auto unknownCode = publicCode;
        unknownCode[index] ^= 0xFF;
        assert(FileSystemLayout::Linux32PathIDOffset(unknownCode.data(), unknownCode.size()) == 0);
    }
    std::cout << "Search-path ABI fixtures and unknown-layout rejection passed\n";
}
