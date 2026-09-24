// Appended to the shared relocated-ELF adapter from stringtable_resolver_harness.
// No code in the engine fixture is executed.
int main(int argc, char** argv)
{
 if (argc != 5) return 2;
 auto image = LoadImage(argv[1]);
 auto* bytes = image.data();
 const uintptr_t base = (uintptr_t)bytes;
 const uintptr_t cvar = strtoull(argv[2], nullptr, 16);
 const uintptr_t iterator = strtoull(argv[3], nullptr, 16);
 const std::string mode = argv[4];
 auto* header = (Elf64_Ehdr*)bytes;
 auto* segments = (Elf64_Phdr*)(bytes + header->e_phoff);
 fixtureMap.l_addr = base;
 fixtureInfo = {base, segments, header->e_phnum};
 void* module = &fixtureMap;
 const void* cvarVTable = bytes + cvar;

 if (mode == "unknown-build" || mode == "malformed-note" || mode == "missing-note")
 {
  bool changed = false;
  for (uint16_t i = 0; i < header->e_phnum; ++i)
   if (segments[i].p_type == PT_NOTE)
   {
    if (mode == "unknown-build") bytes[segments[i].p_vaddr + 16] ^= 1;
    if (mode == "malformed-note") memset(bytes + segments[i].p_vaddr, 0xff, 4);
    if (mode == "missing-note") segments[i].p_type = 0;
    changed = true;
   }
  if (!changed) return 2;
 }
 else if (mode == "wrong-factory") WritePointer(bytes, cvar + 42 * 8, ReadPointer(bytes, iterator));
 else if (mode == "null-factory") WritePointer(bytes, cvar + 42 * 8, 0);
 else if (mode == "swapped-methods")
 {
  const uintptr_t saved = ReadPointer(bytes, iterator + 2 * 8);
  WritePointer(bytes, iterator + 2 * 8, ReadPointer(bytes, iterator + 3 * 8));
  WritePointer(bytes, iterator + 3 * 8, saved);
 }
 else if (mode == "null-method") WritePointer(bytes, iterator + 5 * 8, 0);
 else if (mode == "wrong-rtti") WritePointer(bytes, iterator - 8, ReadPointer(bytes, cvar - 8));
 else if (mode == "wrong-cvar") cvarVTable = bytes + iterator;
 else if (mode == "null-cvar") cvarVTable = nullptr;
 else if (mode == "null-module") module = nullptr;
 else if (mode == "no-read" || mode == "non-executable")
 {
  for (uint16_t i = 0; i < header->e_phnum; ++i)
   if (segments[i].p_type == PT_LOAD)
    segments[i].p_flags &= ~(mode == "no-read" ? PF_R : PF_X);
 }
 else if (mode != "nominal") return 2;

 void** result = (void**)1;
 const bool valid = Symbols::ResolveCVarIteratorLayout(module, cvarVTable, result);
 Check(valid == (mode == "nominal"), "recognized layout or clean rejection");
 Check(result == (mode == "nominal" ? (void**)(bytes + iterator) : nullptr), "exact method table or null");
 printf("%s: %d checks, %d failures\n", mode.c_str(), checks, failures);
 return failures ? 1 : 0;
}
