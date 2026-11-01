# Fuzzing zmail's parsers

libFuzzer harnesses for the code that reads untrusted input:

| Harness | What it feeds |
|---|---|
| `fuzz_rawmime` | raw RFC 822 (the IMAP path): `RawMime` then `MessageParser` |
| `fuzz_messageparser` | Gmail API message JSON: `MessageParser` |
| `fuzz_htmlsanitizer` | HTML: `html::sanitize` |
| `fuzz_calendarinvite` | iCalendar text: `CalendarInvite::parse` |
| `fuzz_searchquery` | search box text: `SearchQuery`, run against a real FTS5 table |

Build with clang, ASan and UBSan:

```sh
CC=clang CXX=clang++ cmake -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DZMAIL_SANITIZE=ON -DZMAIL_FUZZ=ON
cmake --build build-fuzz
ctest --test-dir build-fuzz            # unit tests + each seed corpus replayed once
build-fuzz/fuzz/fuzz_rawmime -max_total_time=60 fuzz/corpus/rawmime
```

CI (`fuzz` job in `.github/workflows/ci.yml`) does the same and runs each
fuzzer for 60 seconds. Seeds in `corpus/` are small and synthetic (example.com
addresses only, never real mail). When a fuzzer finds a crash, fix it, add the
input to the harness's `corpus/` directory so the ctest replays it, and add a
unit test.
