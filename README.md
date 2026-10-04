# zmail

A local Linux desktop email client for Gmail that plays a custom sound for
new mail, per sender or per rule, the way Eudora did.

**Status:** planning + scaffold (v0.1.0). See [docs/PLAN.md](docs/PLAN.md).

- Linux amd64 first, packaged as a `.deb`
- Gmail over OAuth 2.0 only; no passwords are ever stored, and tokens live in
  the OS keyring (Secret Service / libsecret)
- Local offline cache with full-text search
- C++20, Qt 6 Widgets, CMake/CPack (same stack as
  [zterminal](https://github.com/sbj-ee/zterminal) and
  [zwriter](https://github.com/sbj-ee/zwriter))

## Build

```sh
sudo apt install qt6-base-dev cmake ninja-build g++
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cd build && cpack -G DEB   # zmail_0.1.0_amd64.deb
```

The version lives only in `project(zmail VERSION ...)` in `CMakeLists.txt`.

## License

MIT. See [LICENSE](LICENSE).
