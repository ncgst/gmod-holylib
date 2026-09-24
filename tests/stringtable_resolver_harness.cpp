
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define ELFMAG "\x7f" "ELF"
#define SELFMAG 4
#define EI_CLASS 4
#define ELFCLASS64 2
#define EM_X86_64 62
#define PT_LOAD 1
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



// The fixture images are never executed. These loader adapters let the exact
// production resolver inspect relocated ELF images on Linux or Windows.
#include <string>
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
static uintptr_t ReadPointer(const unsigned char* image, uintptr_t rva)
{
 uintptr_t value;
 memcpy(&value, image + rva, sizeof(value));
 return value;
}
static void WritePointer(unsigned char* image, uintptr_t rva, uintptr_t value)
{
 memcpy(image + rva, &value, sizeof(value));
}
static void WriteRelative(unsigned char* image, uintptr_t instructionEnd, uintptr_t target)
{
 int64_t delta = (int64_t)target - (int64_t)instructionEnd;
 if (delta < INT32_MIN || delta > INT32_MAX) exit(2);
 int32_t displacement = (int32_t)delta;
 memcpy(image + instructionEnd - 4, &displacement, sizeof(displacement));
}

int main(int argc, char** argv)
{
 if (argc != 7) return 2;
 auto image = LoadImage(argv[1]);
 auto* bytes = image.data();
 uintptr_t base = (uintptr_t)bytes;
 uintptr_t d1 = strtoull(argv[2], nullptr, 16);
 uintptr_t tableVTable = strtoull(argv[3], nullptr, 16);
 uintptr_t containerVTable = strtoull(argv[4], nullptr, 16);
 uintptr_t removeAll = strtoull(argv[5], nullptr, 16);
 std::string mode = argv[6];
 auto* header = (Elf64_Ehdr*)bytes;
 auto* segments = (Elf64_Phdr*)(bytes + header->e_phoff);
 fixtureMap.l_addr = base;
 fixtureInfo = {base, segments, header->e_phnum};
 uintptr_t container = base + containerVTable;
 void* module = &fixtureMap;
 bool expectD1 = true, expectFallback = true;

 if (mode == "corrupt-deleting")
 {
  bytes[ReadPointer(bytes, tableVTable + 8) - base + 5] ^= 0xff;
  expectD1 = false;
 }
 else if (mode == "wrong-table-type" || mode == "wrong-container-type")
 {
  uintptr_t vt = mode == "wrong-table-type" ? tableVTable : containerVTable;
  uintptr_t typeinfo = ReadPointer(bytes, vt - 8) - base;
  bytes[ReadPointer(bytes, typeinfo + 8) - base] ^= 1;
  if (mode == "wrong-table-type") expectD1 = false;
  else expectFallback = false;
 }
 else if (mode == "invalid-container-vptr")
 {
  container = UINTPTR_MAX - 8;
  expectFallback = false;
 }
 else if (mode == "invalid-vtable-displacement")
 {
  int32_t invalid = INT32_MAX;
  memcpy(bytes + d1 + 4, &invalid, sizeof(invalid));
  expectD1 = false;
 }
 else if (mode == "no-read" || mode == "non-executable")
 {
  for (uint16_t i = 0; i < header->e_phnum; ++i)
   if (segments[i].p_type == PT_LOAD)
    segments[i].p_flags &= ~(mode == "no-read" ? PF_R : PF_X);
  expectD1 = expectFallback = false;
 }
 else if (mode == "null-module")
 {
  module = nullptr;
  expectD1 = expectFallback = false;
 }
 else if (mode == "ambiguous")
 {
  // A second internally consistent copy must cause rejection, not first-match
  // selection. Copies stay inside their original readable/executable segments.
  uintptr_t code = d1 - 0x1000;
  uintptr_t secondVTable = 0;
  for (uint16_t i = 0; i < header->e_phnum; ++i)
   if (segments[i].p_type == PT_LOAD && (segments[i].p_flags & PF_R) && !(segments[i].p_flags & PF_X))
    secondVTable = (segments[i].p_vaddr + segments[i].p_memsz - 0x1000) & ~uintptr_t(7);
  if (!secondVTable) return 2;
  uintptr_t deleting = ReadPointer(bytes, tableVTable + 8) - base;
  memcpy(bytes + code, bytes + d1, 24);
  memcpy(bytes + code + 64, bytes + deleting, 31);
  memcpy(bytes + secondVTable - 16, bytes + tableVTable - 16, 18 * sizeof(uintptr_t));
  WritePointer(bytes, secondVTable, base + code);
  WritePointer(bytes, secondVTable + 8, base + code + 64);
  WriteRelative(bytes, code + 8, secondVTable);
  WriteRelative(bytes, code + 64 + 17, code);
  int32_t originalFree;
  memcpy(&originalFree, bytes + deleting + 27, 4);
  uintptr_t freeRVA = uintptr_t(int64_t(deleting + 31) + originalFree);
  WriteRelative(bytes, code + 64 + 31, freeRVA);
  expectD1 = false;
 }
 else if (mode != "nominal") return 2;

 void* actualD1 = Symbols::ResolveCNetworkStringTableDeconstructor(module);
 void* actualFallback = Symbols::ResolveCNetworkStringTableContainerRemoveAllTables(module, &container);
 Check(actualD1 == (expectD1 ? (void*)(base + d1) : nullptr), "exact destructor or clean rejection");
 Check(actualFallback == (expectFallback ? (void*)(base + removeAll) : nullptr), "exact fallback or clean rejection");
 Check(Symbols::ResolveCNetworkStringTableContainerRemoveAllTables(module, nullptr) == nullptr, "null container rejected");
 if (mode == "ambiguous")
 {
  // Prove the synthetic copy really was valid, then restore its ambiguity.
  bytes[d1] ^= 1;
  Check(Symbols::ResolveCNetworkStringTableDeconstructor(module) == bytes + d1 - 0x1000, "second candidate independently resolves");
 }
 if (mode == "nominal")
 {
  uintptr_t result = 0;
  Check(!Symbols::AddRelativeDisplacement(UINTPTR_MAX, 1, result), "positive displacement overflow rejected");
  Check(!Symbols::AddRelativeDisplacement(0, INT32_MIN, result), "negative displacement underflow rejected");
  Check(Symbols::AddRelativeDisplacement(0x80000000u, INT32_MIN, result) && result == 0, "minimum displacement handled");
  Elf64_Phdr segment = {};
  segment.p_type = PT_LOAD; segment.p_flags = PF_R; segment.p_memsz = 16;
  Symbols::EngineModule range = {UINTPTR_MAX - 8, &segment, 1};
  Check(!Symbols::IsRangeAccessible(range, (void*)range.base, 1, false), "segment overflow rejected");
 }
 printf("%s: %d checks, %d failures\n", mode.c_str(), checks, failures);
 return failures ? 1 : 0;
}
