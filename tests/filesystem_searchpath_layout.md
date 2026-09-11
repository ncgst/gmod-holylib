# Filesystem compatibility regression

The Linux32 public engine and dev/prerelease engines use different private
`CBaseFileSystem::CSearchPath` layouts. Both structures are 32 bytes, so a size
assertion cannot detect the difference.

| Field | public offset | dev/prerelease offset |
| --- | ---: | ---: |
| Store ID | 0 | 0 |
| Priority group | 12 | 4 |
| Path ID information pointer | 4 | 8 |
| Flags | 8 | 12 |
| Path symbol | 16 | 16 |
| Debug path | 20 | 20 |
| Pack pointers | 24, 28 | 24, 28 |

The old pointer access reads priority group 6 as address `0x6` on dev/prerelease.
Select the pointer offset before installing filesystem detours. The named
`SetSearchPathIsTrustedSource` function's initial flag write identifies the
layout. Check the entire instruction prefix and reject unfamiliar code rather
than guessing an offset. An unrecognized Linux32 layout leaves the engine's
filesystem implementation in place and logs a warning.

The fixtures were verified against the engines in the GLuaTest images used by
GitHub Actions run `34579931073`, downloaded September 11, 2026:

- `raphaelit7/gluatest:public`, `bin/dedicated_srv.so` SHA256
  `c94f09641782e772ba36e71aec1cdd3b5dd3ae64224dc4014bb7d31fc3179028`
- `raphaelit7/gluatest:dev`, `bin/dedicated_srv.so` SHA256
  `a2567b228f2b63320da494459f0eeae1fdba58a48fbec593a190c27c3a8e9ba0`
- `raphaelit7/gluatest:prerelease`, `bin/dedicated_srv.so` SHA256
  `3bfd42a2e9755a93ae0de7caf8a6a532bd6ac01e8097a9f85b4d0d76766e97aa`

Run the fixture check with:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -pedantic -Isource tests/filesystem_searchpath_layout_tests.cpp -o /tmp/filesystem_layout_tests
/tmp/filesystem_layout_tests
```

The full GLuaTest matrix must still boot and finish on public, dev, prerelease
and x86-64. Fixture checks alone do not validate engine behavior.
