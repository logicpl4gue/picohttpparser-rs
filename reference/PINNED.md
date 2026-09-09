# Pinned Reference — PicoHTTPParser

Milestone 0 artifact. The exact upstream revision treated as the behavioral
oracle for the Rust rewrite. This tree is immutable: never edit files here,
re-fetch from the pinned commit instead.

## Pin

| Field | Value |
|---|---|
| Repository | `h2o/picohttpparser` (github.com) |
| Commit | `f4d94b48b31e0abae029ebeafcfd9ca0680ede58` |
| Short SHA | `f4d94b48` |
| Date fetched (UTC) | 2026-09-08T23:14:09Z |
| Version macro | `PICOHTTPPARSER_VERSION "1.dev"` (master-tracked, treated as newer than any 1.x snapshot) |

Raw fetch base URL (used for every file below):

```
https://raw.githubusercontent.com/h2o/picohttpparser/f4d94b48b31e0abae029ebeafcfd9ca0680ede58/<filename>
```

## Files

All eight files below were fetched **successfully** (HTTP 200, zero errors).
Sizes in bytes; verify with `(cd reference && sha256sum -c SHA256SUMS)`.

| File | Size (bytes) | SHA-256 |
|---|---|---|
| `picohttpparser.c` | 28113 | `ddada2e27e9010f678a68a93a08fc13dee32178cc497b602322d80900eb94044` |
| `picohttpparser.h` | 3970 | `1fc9074dd12418b2b91e55ef3a8279bccf8d3577b790dea6f2f908ab2a157f1c` |
| `test.c` | 22628 | `3690351c16fdc40c23d1489af4524b179b3e1909f3382c67774e4cf7bb2f9c8c` |
| `bench.c` | 3769 | `cf44504db5908792d4f7c6355a7a05ba80cf2411efb85995fc6e2933ec9feca7` |
| `Makefile` | 1579 | `5eea1c5269a2684a658aeaaf1cb8b672722437fe281a8341c1bdf81458647ecd` |
| `README.md` | 4343 | `6ea77dd3359d5d3964e4d8d44c2d085065bad0280a9fdda9416958249ae63c66` |
| `picotest/picotest.c` | 2455 | `72120332672545ec7ea481652fb423d3e3c958cff82239eeb3afb052ec37ed66` |
| `picotest/picotest.h` | 1450 | `e5bbe9d14f4b7083328f61d3c27387fd24bd810961d2042800ebac95056625b6` |
| **Total (8 files)** | 68307 | — |

Upstream submodule pin: `h2o/picotest @ 70b9797596d81896cba49e5918fd5b1edf57269b`.

Fetch command used per file:

```bash
curl -fsSL -o <filename> https://raw.githubusercontent.com/h2o/picohttpparser/f4d94b48b31e0abae029ebeafcfd9ca0680ede58/<filename>
```

## Verify

```bash
cd reference
sha256sum picohttpparser.c picohttpparser.h test.c bench.c Makefile README.md \
  picotest/picotest.c picotest/picotest.h
# compare against the table above (or simply: sha256sum -c SHA256SUMS)
```

## Known Gaps (honest)

- **`picotest` harness fetched 2026-09-09** at the upstream submodule pin
  `70b9797596d81896cba49e5918fd5b1edf57269b`
  (`reference/picotest/picotest.c`, `picotest.h`; hashes in `SHA256SUMS`).
- **No sanitizers in this toolchain** (w64devkit GCC 16.2.0 ships no
  libasan/libubsan): the suite builds with plain `-O2`. ASan/UBSan coverage
  is pending a toolchain that ships them.
- **`prove` absent** (Git-Bash perl has no Test::Harness `prove` script): the
  suite binary runs directly, which is equivalent — `prove -v ./test-bin`
  only executes the TAP-emitting binary.

## Environment (recorded at fetch time)

- OS: Windows (Git-Bash / MSYS2 userland), unknown kernel patch level
- curl: `/mingw64/bin/curl`
- git: 2.54.0.windows.1
- rustc: 1.96.1 (31fca3adb 2026-06-26)
- cargo: 1.96.1 (356927216 2026-06-26)