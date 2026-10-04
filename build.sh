#!/bin/bash

# **********************************************************************
# * Copyright (C) 2017-2026 MX Authors
# *
# * Authors: Adrian
# *          Dolphin_Oracle
# *          MX Linux <http://mxlinux.org>
# *
# * This file is part of mx-packageinstaller.
# *
# * mx-packageinstaller is free software: you can redistribute it and/or modify
# * it under the terms of the GNU General Public License as published by
# * the Free Software Foundation, either version 3 of the License, or
# * (at your option) any later version.
# *
# * mx-packageinstaller is distributed in the hope that it will be useful,
# * but WITHOUT ANY WARRANTY; without even the implied warranty of
# * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# * GNU General Public License for more details.
# *
# * You should have received a copy of the GNU General Public License
# * along with mx-packageinstaller.  If not, see <http://www.gnu.org/licenses/>.
# **********************************************************************/

set -e

# Derive this package's own name/version (matching the artifact filenames
# dpkg-buildpackage drops in the parent directory, e.g.
# mx-packageinstaller_26.07.3_amd64.deb) from debian/changelog. Used to scope
# the debs/ move and cleanup globs so they can never touch unrelated files
# that merely happen to contain "build" in their name.
package_artifact_prefix() {
    local pkg version
    pkg=$(dpkg-parsechangelog -S Source)
    version=$(dpkg-parsechangelog -S Version)
    # Debian policy: artifact filenames never include the epoch.
    version="${version#*:}"
    echo "${pkg}_${version}"
}

# Default values
BUILD_DIR="build"
BUILD_TYPE="Release"
USE_CLANG=false
CLEAN=false
DEBIAN_BUILD=false
ARCH_BUILD=false
BUILD_TESTS=false
RUN_TESTS_ONLY=false

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -d|--debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        -c|--clang)
            USE_CLANG=true
            shift
            ;;
        --clean)
            CLEAN=true
            shift
            ;;
        --debian)
            DEBIAN_BUILD=true
            shift
            ;;
        --arch)
            ARCH_BUILD=true
            shift
            ;;
        -t|--tests)
            BUILD_TESTS=true
            shift
            ;;
        --test)
            RUN_TESTS_ONLY=true
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS]"
            echo "Options:"
            echo "  -d, --debug     Build in Debug mode (default: Release)"
            echo "  -c, --clang     Use clang compiler"
            echo "  -t, --tests     Build with unit tests enabled and run them"
            echo "  --test          Run tests only (no build)"
            echo "  --clean         Clean build directory before building"
            echo "  --debian        Build Debian package"
            echo "  --arch          Build Arch Linux package"
            echo "  -h, --help      Show this help message"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Run tests only (no build)
if [ "$RUN_TESTS_ONLY" = true ]; then
    if [ ! -d "$BUILD_DIR/Testing" ]; then
        echo "Error: Tests not found. Build with --tests first."
        exit 1
    fi
    echo "Running tests..."
    cd "$BUILD_DIR/Testing" && ctest --verbose
    exit $?
fi

# Build Debian package
if [ "$DEBIAN_BUILD" = true ]; then
    echo "Building Debian package..."
    debuild -us -uc

    echo "Creating debs directory and moving debian artifacts..."
    mkdir -p debs
    ARTIFACT_PREFIX=$(package_artifact_prefix)
    mv ../"${ARTIFACT_PREFIX}"*.deb debs/ 2>/dev/null || true
    mv ../"${ARTIFACT_PREFIX}"*.changes debs/ 2>/dev/null || true
    mv ../"${ARTIFACT_PREFIX}"*.dsc debs/ 2>/dev/null || true
    mv ../"${ARTIFACT_PREFIX}"*.tar.* debs/ 2>/dev/null || true
    mv ../"${ARTIFACT_PREFIX}"*.buildinfo debs/ 2>/dev/null || true
    mv ../"${ARTIFACT_PREFIX}"*.build debs/ 2>/dev/null || true

    echo "Cleaning build directory and debian artifacts..."
    rm -rf "$BUILD_DIR"
    rm -f debian/*.debhelper.log debian/*.substvars debian/files
    rm -rf debian/.debhelper/ debian/mx-packageinstaller/ obj-*/
    rm -f translations/*.qm version.h
    rm -f ../"${ARTIFACT_PREFIX}"*.build ../"${ARTIFACT_PREFIX}"*.buildinfo 2>/dev/null || true

    echo "Debian package build completed!"
    echo "Debian artifacts moved to debs/ directory"
    exit 0
fi

# Build Arch Linux package
if [ "$ARCH_BUILD" = true ]; then
    echo "Building Arch Linux package..."

    if ! command -v makepkg &> /dev/null; then
        echo "Error: makepkg not found. Please install base-devel package."
        exit 1
    fi

    if [ ! -f PKGBUILD ]; then
        echo "Error: PKGBUILD not found; cannot determine version for Arch build."
        exit 1
    fi
    PKGVER_LINE=$(sed -n 's/^pkgver=//p' PKGBUILD | head -n 1)
    PKGREL=$(sed -n 's/^pkgrel=//p' PKGBUILD | head -n 1)
    if [ -z "$PKGVER_LINE" ]; then
        echo "Error: could not parse pkgver from PKGBUILD."
        exit 1
    fi
    if [[ "$PKGVER_LINE" =~ ^\$\{PKGVER:-([^}]+)\}$ ]]; then
        PKGVER="${BASH_REMATCH[1]}"
    else
        PKGVER="$PKGVER_LINE"
    fi
    if [ -n "$PKGREL" ]; then
        ARCH_VERSION="${PKGVER}-${PKGREL}"
    else
        ARCH_VERSION="${PKGVER}"
    fi
    echo "Using version ${ARCH_VERSION} from PKGBUILD"

    ARCH_BUILDDIR=$(mktemp -d -p "$PWD" archpkgbuild.XXXXXX)
    # Also on failure: a pkg/ left behind ends up in the Debian source tarball.
    trap 'rm -rf "$ARCH_BUILDDIR" pkg' EXIT

    rm -rf pkg *.pkg.tar.zst

    PKG_DEST_DIR="$PWD/build"
    mkdir -p "$PKG_DEST_DIR"

    PKGDEST="$PKG_DEST_DIR" PKGVER="$PKGVER" PKGREL="$PKGREL" makepkg -f

    echo "Cleaning makepkg artifacts..."
    rm -rf pkg

    echo "Arch Linux package build completed!"
    echo "Package: $(ls *.pkg.tar.zst 2>/dev/null || echo 'not found')"
    echo "Binary available at: build/mx-packageinstaller"
    exit 0
fi

# Clean build directory if requested
if [ "$CLEAN" = true ]; then
    echo "Cleaning build directory and debian artifacts..."
    rm -rf "$BUILD_DIR"
    rm -f debian/*.debhelper.log debian/*.substvars debian/files
    rm -rf debian/.debhelper/ debian/mx-packageinstaller/ obj-*/
    rm -f translations/*.qm version.h
    if command -v dpkg-parsechangelog >/dev/null 2>&1; then
        ARTIFACT_PREFIX=$(package_artifact_prefix)
        rm -f ../"${ARTIFACT_PREFIX}"*.build ../"${ARTIFACT_PREFIX}"*.buildinfo 2>/dev/null || true
    fi
fi

# Create build directory
mkdir -p "$BUILD_DIR"

# Configure CMake with Ninja
echo "Configuring CMake with Ninja generator..."
CMAKE_ARGS=(
    -G Ninja
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
)

if [ "$USE_CLANG" = true ]; then
    CMAKE_ARGS+=(-DUSE_CLANG=ON)
    echo "Using clang compiler"
fi

if [ "$BUILD_TESTS" = true ]; then
    CMAKE_ARGS+=(-DBUILD_TESTS=ON)
    echo "Building with tests enabled"
fi

cmake "${CMAKE_ARGS[@]}"

# Build the project
echo "Building project with Ninja..."
cmake --build "$BUILD_DIR" --parallel

echo "Build completed successfully!"
echo "Executable: $BUILD_DIR/mx-packageinstaller"

# Run tests if built with tests
if [ "$BUILD_TESTS" = true ]; then
    echo "Running tests..."
    cd "$BUILD_DIR" && ctest --verbose
    cd - > /dev/null
fi
