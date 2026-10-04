# mx-packageinstaller

[![latest packaged version(s)](https://repology.org/badge/latest-versions/mx-packageinstaller.svg)](https://repology.org/project/mx-packageinstaller/versions)
[![build result](https://build.opensuse.org/projects/home:mx-packaging/packages/mx-packageinstaller/badge.svg?type=default)](https://software.opensuse.org//download.html?project=home%3Amx-packaging&package=mx-packageinstaller)
[![Continous Integration](https://github.com/AdrianTM/mx-packageinstaller/actions/workflows/main.yml/badge.svg)](https://github.com/AdrianTM/mx-packageinstaller/actions/workflows/main.yml)

A comprehensive package management GUI for MX Linux with support for multiple package sources including APT repositories, Flatpak applications, and curated popular applications.

![image](https://github.com/MX-Linux/mx-packageinstaller/assets/418436/315e76dd-a6ff-43c7-af3c-ff02a8c83271)

## Project Structure

```
mx-packageinstaller/
├── src/                 # Source code (*.cpp, *.h, *.ui files)
├── Testing/            # Unit tests and test configuration
├── translations/       # Translation files (*.ts)
├── scripts/           # Helper scripts and policies
├── help/              # Documentation and help files
├── icons/             # Application icons and resources
├── debian/            # Debian packaging files
└── CMakeLists.txt     # Build configuration
```

## Building

This is a Qt6 application using CMake build system.

### Build Commands

```bash
# Basic build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Build with tests
cmake -B build -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run tests
cd build && ctest
# or run an individual test binary directly (built under build/Testing/)
cd build/Testing && ./test_versionnumber && ./test_aptcache

# Continuous testing (requires inotify-tools)
cd build && make watch_tests

# Clean build
cmake --build build --target clean

# Using the build script
./build.sh                    # Release build
./build.sh --debug            # Debug build
./build.sh --tests            # Build with tests
./build.sh --clang            # Use Clang compiler
./build.sh --clean            # Clean before build
./build.sh --debian           # Build Debian package
```

## Testing

The project includes comprehensive unit tests for critical components:

- **test_versionnumber**: Tests Debian version comparison logic including epochs, revisions, tildes, and real-world scenarios (14 tests)
- **test_aptcache**: Tests APT cache parsing, architecture filtering, version selection, and file processing (9 tests)

All tests use QtTest framework and can be run individually or through CMake's test runner.

## Features

- **Multiple Package Sources**: APT repositories, Flatpak, curated popular applications
- **Architecture Filtering**: Automatic detection and filtering for current system architecture
- **Version Management**: Sophisticated Debian version comparison and selection
- **Privilege Escalation**: Secure administrative operations via pkexec
- **Comprehensive Testing**: Unit tests for critical package management logic
- **Internationalization**: Full translation support for multiple languages

## Logging

Application logs are written to `/tmp/mxpi.log` during runtime and copied to `/var/log/mxpi.log` at exit.

## Requirements

- Qt6 (Core, Gui, Widgets, Network, Xml, LinguistTools)
- C++20 compiler (GCC 12+ or Clang)
- CMake 3.16+
- Ninja (recommended for faster builds)

