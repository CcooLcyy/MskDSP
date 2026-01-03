# Repository Guidelines

## Project Structure & Module Organization
Core code lives in `src/`: `core/ModuleManager` manages module lifecycles and exposes gRPC services; `DataCenter` and `IEC104` are plugin-style shared libraries linked through the manager; `main.cc` boots the manager on a background thread. Protobuf contracts are in `protobuf/` and generate gRPC stubs linked by `dspProto`. Third-party code sits in `3rdlibs/siren/` (built as a subproject). Build outputs land in `package/` (`package/MskDSP` for the executable, `package/lib` for shared libs, `package/conf` for generated config headers). Helper scripts live in `script/`.

## Build, Test, and Development Commands
Dependencies are managed via CMake with vcpkg manifests (`vcpkg*.json`) for gRPC, Protobuf, Boost, and GTest. Typical workflow:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build --parallel
ctest --test-dir build        # when tests are defined
```
Run `cmake --build build --target install` if you need to stage artifacts; binaries and libs will appear under `package/`. Use `CMAKE_BUILD_TYPE=Release` for production binaries.

## Coding Style & Naming Conventions
Formatting is enforced by `.clang-format` (Google base): 2-space indents, no tabs, unlimited line length, brace lists tightened. Write modern C++23 and prefer RAII; keep pointer alignment on the right (`Type* ptr`). gRPC/Proto files should use PascalCase messages and service names; generated headers should be consumed via the `dspProto` target. Keep module library names consistent with their `cmake/LibInfo.cmake` settings.

## Testing Guidelines
Unit tests use GTest (enabled in Debug for `3rdlibs/siren`; add top-level tests under `test/` or module-specific `test` folders). Name files `*_test.cc`, register with `add_test`, and run with `ctest --output-on-failure`. Cover module lifecycle edges (load/unload, port reuse) and protobuf/grpc contract compatibility. Favor deterministic tests; avoid binding real service ports unless isolated via random available ports.

## Commit & Pull Request Guidelines
Recent history uses bracketed prefixes (`[feature]`, etc.) plus concise Mandarin summaries. Follow that pattern (`[fix]`, `[refactor]` as appropriate) and keep the first line under ~72 chars. For PRs, include: scope/intent, key changes per module, build/test commands run, and any port/config changes. Link related issues and attach screenshots or logs for protocol/interop changes when relevant.

## Architecture Notes
Modules are shared libraries loaded by the manager; version metadata is generated via `cmake/LibInfo.h.in` into each module’s `include/`. gRPC services are the primary integration surface—update `.proto` files first, then regenerate via CMake. Keep `package/conf` under source control only for templates; avoid committing local runtime artifacts.
