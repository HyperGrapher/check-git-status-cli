# C++ implementation

This directory contains a dependency-free C++20 port of `check-git-status`. It mirrors the Go CLI's repository discovery, bounded concurrent Git checks, status semantics, warning layout, exit codes, and timing report.

Directories named `node_modules` or `build` are excluded from recursive discovery. Repository status is collected with two Git commands per repository.

## Requirements

- CMake 3.20 or newer
- A C++20 compiler
- Git available on `PATH` when running the tool

## Build and test

```sh
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build --config Release
ctest --test-dir cpp/build -C Release --output-on-failure
```

The executable is named `check-git-status-cpp` (`check-git-status-cpp.exe` on Windows).

## Usage

```sh
cpp/build/check-git-status-cpp /path/to/projects
cpp/build/check-git-status-cpp --dirty-only --workers 8 /path/to/projects
```

When using a multi-configuration generator, the executable may be under a configuration directory such as `cpp/build/Release/`.
