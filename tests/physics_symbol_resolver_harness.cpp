// Fixture harness for the production Linux x86-64 physics symbol resolvers.
// The fixture images are relocated in memory but never executed; no engine or
// game instruction runs here.
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define ELFMAG "\x7f" "ELF"
#define SELFMAG 4
#define EI_CLASS 4
#define ELFCLASS64 2
#define EM_X86_64 62
#define PT_LOAD 1
#define PT_NOTE 4
#define PF_X 1
#define PF_R 4
#define SHT_RELA 4

struct Elf64_Shdr
{
	uint32_t sh_name;
	uint32_t sh_type;
	uint64_t sh_flags;
	uint64_t sh_addr;
	uint64_t sh_offset;
	uint64_t sh_size;
	uint32_t sh_link;
	uint32_t sh_info;
	uint64_t sh_addralign;
	uint64_t sh_entsize;
};

struct Elf64_Ehdr
{
	unsigned char e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct Elf64_Phdr
{
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

struct link_map
{
	uintptr_t l_addr;
};

struct dl_phdr_info
{
	uintptr_t dlpi_addr;
	const Elf64_Phdr* dlpi_phdr;
	uint16_t dlpi_phnum;
};
static link_map fixtureMap = {};
static dl_phdr_info fixtureInfo = {};
static const int RTLD_DI_LINKMAP = 2;
static int dlinfo(void* handle, int request, void* output)
{
	if (handle != &fixtureMap || request != RTLD_DI_LINKMAP) return -1;
	*(link_map**)output = &fixtureMap;
	return 0;
}
static int dl_iterate_phdr(int (*callback)(dl_phdr_info*, size_t, void*), void* context)
{
	return callback(&fixtureInfo, sizeof(fixtureInfo), context);
}

namespace Symbols
{
// INSERT_PRODUCTION_RESOLVER
}

static std::vector<unsigned char> LoadImage(const char* path)
{
	FILE* file = fopen(path, "rb");
	if (!file)
	{
		fprintf(stderr, "cannot open %s\n", path);
		exit(2);
	}

	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	std::vector<unsigned char> raw((size_t)size);
	if (fread(raw.data(), 1, raw.size(), file) != raw.size())
		exit(2);
	fclose(file);

	const Elf64_Ehdr* header = (const Elf64_Ehdr*)raw.data();
	const Elf64_Phdr* phdrs = (const Elf64_Phdr*)(raw.data() + header->e_phoff);

	uint64_t imageSize = 0;
	for (uint16_t i = 0; i < header->e_phnum; ++i)
		if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_vaddr + phdrs[i].p_memsz > imageSize)
			imageSize = phdrs[i].p_vaddr + phdrs[i].p_memsz;

	std::vector<unsigned char> image((size_t)imageSize, 0);
	for (uint16_t i = 0; i < header->e_phnum; ++i)
	{
		const Elf64_Phdr& phdr = phdrs[i];
		if (phdr.p_type != PT_LOAD)
			continue;

		memcpy(image.data() + phdr.p_vaddr, raw.data() + phdr.p_offset, (size_t)phdr.p_filesz);
	}

	// Apply R_X86_64_RELATIVE relocations the way the dynamic loader would.
	const Elf64_Shdr* sections = (const Elf64_Shdr*)(raw.data() + header->e_shoff);
	for (uint16_t i = 0; i < header->e_shnum; ++i)
	{
		const Elf64_Shdr& section = sections[i];
		if (section.sh_type != SHT_RELA)
			continue;

		const size_t count = (size_t)(section.sh_size / section.sh_entsize);
		for (size_t j = 0; j < count; ++j)
		{
			uint64_t offset;
			uint64_t info;
			int64_t addend;
			memcpy(&offset, raw.data() + section.sh_offset + j * section.sh_entsize, 8);
			memcpy(&info, raw.data() + section.sh_offset + j * section.sh_entsize + 8, 8);
			memcpy(&addend, raw.data() + section.sh_offset + j * section.sh_entsize + 16, 8);
			if ((uint32_t)info == 8) // R_X86_64_RELATIVE
			{
				uint64_t value = (uint64_t)image.data() + (uint64_t)addend;
				memcpy(image.data() + offset, &value, 8);
			}
		}
	}

	return image;
}

static int checks = 0, failures = 0;
static void Check(bool condition, const char* message)
{
	++checks;
	if (!condition) { ++failures; fprintf(stderr, "FAIL: %s\n", message); }
}

static uintptr_t FindBytes(const std::vector<unsigned char>& image, const char* pNeedle, size_t nSize, uintptr_t nFrom = 0)
{
	for (uintptr_t i = nFrom; i + nSize <= image.size(); ++i)
	{
		if (memcmp(image.data() + i, pNeedle, nSize) == 0)
			return i;
	}
	return UINTPTR_MAX;
}

static uintptr_t FindPadding(const std::vector<unsigned char>& image, uintptr_t nBegin, uintptr_t nEnd, size_t nSize)
{
	uintptr_t nRun = 0;
	for (uintptr_t i = nBegin; i < nEnd; ++i)
	{
		nRun = image[i] == 0 ? nRun + 1 : 0;
		if (nRun >= nSize)
			return i + 1 - nSize;
	}
	return UINTPTR_MAX;
}

static uintptr_t ReadPointerAt(const std::vector<unsigned char>& image, uintptr_t rva)
{
	uintptr_t value;
	memcpy(&value, image.data() + rva, sizeof(value));
	return value;
}

static void WritePointerAt(std::vector<unsigned char>& image, uintptr_t rva, uintptr_t value)
{
	memcpy(image.data() + rva, &value, sizeof(value));
}

// Locates the CPhysicsHook vtable address point by exact RTTI name, mirroring the
// production check but implemented independently in the harness.
static uintptr_t FindCPhysicsHookVTable(const std::vector<unsigned char>& image, uintptr_t nDataBegin)
{
	uintptr_t base = (uintptr_t)image.data();
	for (uintptr_t i = nDataBegin; i + 16 <= image.size(); ++i)
	{
		if (ReadPointerAt(image, i) != 0) continue;
		uintptr_t typeInfo = ReadPointerAt(image, i + 8);
		if (typeInfo < base || typeInfo + 16 > base + image.size()) continue;
		uintptr_t name = ReadPointerAt(image, typeInfo - base + 8);
		if (name < base || name + 14 > base + image.size()) continue;
		if (memcmp(image.data() + (name - base), "12CPhysicsHook", 14) == 0)
			return i + 16; // i is the offset-to-top slot; the address point is 16 bytes in.
	}
	return UINTPTR_MAX;
}

int main(int argc, char** argv)
{
	if (argc != 5) return 2;
	auto image = LoadImage(argv[1]);
	auto* bytes = image.data();
	uintptr_t base = (uintptr_t)bytes;
	uintptr_t phook = strtoull(argv[2], nullptr, 16);
	uintptr_t fus = strtoull(argv[3], nullptr, 16);
	std::string mode = argv[4];
	auto* header = (Elf64_Ehdr*)bytes;
	auto* segments = (Elf64_Phdr*)(bytes + header->e_phoff);
	fixtureMap.l_addr = base;
	fixtureInfo = {base, segments, header->e_phnum};
	void* module = &fixtureMap;
	bool expectPhook = true, expectFus = true;

	uintptr_t rxBegin = 0, rxEnd = 0;
	for (uint16_t i = 0; i < header->e_phnum; ++i)
	{
		if (segments[i].p_type == PT_LOAD && (segments[i].p_flags & (PF_X | PF_R)) == (PF_X | PF_R))
		{
			rxBegin = segments[i].p_vaddr;
			rxEnd = segments[i].p_vaddr + segments[i].p_filesz;
			break;
		}
	}

	if (mode == "corrupt-phook-name")
	{
		uintptr_t off = FindBytes(image, "CPhysicsHook::FrameUpdatePostEntityThink", 38);
		if (off == UINTPTR_MAX) return 2;
		bytes[off + 3] ^= 1;
		expectPhook = false;
	}
	else if (mode == "duplicate-phook-name")
	{
		uintptr_t off = FindBytes(image, "CPhysicsHook::FrameUpdatePostEntityThink", strlen("CPhysicsHook::FrameUpdatePostEntityThink") + 1);
		if (off == UINTPTR_MAX) return 2;
		uintptr_t pad = FindPadding(image, rxBegin, rxEnd, strlen("CPhysicsHook::FrameUpdatePostEntityThink") + 1 + 16);
		if (pad == UINTPTR_MAX) return 2;
		memcpy(bytes + pad, bytes + off, strlen("CPhysicsHook::FrameUpdatePostEntityThink") + 1);
		expectPhook = false;
	}
	else if (mode == "wrong-type-name")
	{
		uintptr_t off = FindBytes(image, "12CPhysicsHook", 14);
		if (off == UINTPTR_MAX) return 2;
		bytes[off + 2] ^= 1;
		expectPhook = false;
	}
	else if (mode == "zero-vtable-typeinfo")
	{
		uintptr_t vt = FindCPhysicsHookVTable(image, rxEnd);
		if (vt == UINTPTR_MAX) return 2;
		WritePointerAt(image, vt - 8, 0);
		expectPhook = false;
	}
	else if (mode == "remove-slot")
	{
		uintptr_t vt = FindCPhysicsHookVTable(image, rxEnd);
		if (vt == UINTPTR_MAX) return 2;
		uintptr_t slot = UINTPTR_MAX;
		for (size_t n = 0; n < 64; ++n)
		{
			if (ReadPointerAt(image, vt + n * 8) == base + phook)
			{
				slot = vt + n * 8;
				break;
			}
		}
		if (slot == UINTPTR_MAX) return 2;
		WritePointerAt(image, slot, base);
		expectPhook = false;
	}
	else if (mode == "wrong-slot")
	{
		uintptr_t vt = FindCPhysicsHookVTable(image, rxEnd);
		if (vt == UINTPTR_MAX || ReadPointerAt(image, vt + 16 * 8) != base + phook) return 2;
		WritePointerAt(image, vt + 17 * 8, base + phook);
		WritePointerAt(image, vt + 16 * 8, base);
		expectPhook = false;
	}
	else if (mode == "oversized-data-segment")
	{
		for (uint16_t i = 0; i < header->e_phnum; ++i)
			if (segments[i].p_type == PT_LOAD && !(segments[i].p_flags & PF_X))
				segments[i].p_filesz = segments[i].p_memsz + 1;
		expectPhook = false;
	}

	else if (mode == "duplicate-fus-signature")
	{
		// Copy the unique fus entry signature into executable padding so the
		// globally unique requirement must reject both copies.
		uintptr_t entry = base + fus;
		if (entry < base || entry >= base + image.size()) return 2;
		uintptr_t pad = FindPadding(image, rxBegin, rxEnd, 64);
		if (pad == UINTPTR_MAX) return 2;
		memcpy(bytes + pad, bytes + (entry - base), 34);
		expectFus = false;
	}
	else if (mode == "remove-callers")
	{
		int patched = 0;
		for (uintptr_t i = rxBegin; i + 5 <= rxEnd; ++i)
		{
			if (bytes[i] != 0xE8) continue;
			int32_t rel;
			memcpy(&rel, bytes + i + 1, 4);
			if ((int64_t)i + 5 + rel != (int64_t)fus) continue;
			bytes[i] = 0x90;
			patched++;
		}
		if (!patched) return 2;
		expectFus = false;
	}
	else if (mode == "no-read" || mode == "non-executable")
	{
		for (uint16_t i = 0; i < header->e_phnum; ++i)
			if (segments[i].p_type == PT_LOAD)
				segments[i].p_flags &= ~(mode == "no-read" ? PF_R : PF_X);
		expectPhook = expectFus = false;
	}
	else if (mode == "two-executable-segments")
	{
		for (uint16_t i = 0; i < header->e_phnum; ++i)
			if (segments[i].p_type == PT_LOAD && !(segments[i].p_flags & PF_X))
				segments[i].p_flags |= PF_X;
		expectPhook = expectFus = false;
	}
	else if (mode == "null-module")
	{
		module = nullptr;
		expectPhook = expectFus = false;
	}
	else if (mode != "nominal") return 2;

	void* actualPhook = Symbols::ResolveCPhysicsHookFrameUpdatePostEntityThink(module);
	void* actualFus = Symbols::ResolveCCollisionEventFrameUpdate(module);
	Check(actualPhook == (expectPhook ? (void*)(base + phook) : nullptr), "physics hook exact result or clean rejection");
	Check(actualFus == (expectFus ? (void*)(base + fus) : nullptr), "collision event exact result or clean rejection");
	Check(Symbols::ResolveCPhysicsHookFrameUpdatePostEntityThink(nullptr) == nullptr, "null module rejected (hook)");
	Check(Symbols::ResolveCCollisionEventFrameUpdate(nullptr) == nullptr, "null module rejected (collision)");
	printf("%s: %d checks, %d failures\n", mode.c_str(), checks, failures);
	return failures ? 1 : 0;
}
