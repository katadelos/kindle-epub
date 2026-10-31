# Native EPUB for Kindle

Read EPUB files in the stock Kindle reader, with library metadata, covers,
contents navigation, and saved reading positions. Books remain EPUB files;
layout and reading controls come from the firmware's reader.

The supported scope is DRM-free, reflowable EPUB 2 and 3: ZIP/ZIP64 archives,
NCX and EPUB navigation, XHTML, CSS, images, fonts, and internal links.
Encrypted ZIP entries are rejected, and scripts and active embedded content
are removed. DRM, media overlays, and fixed-layout features are outside this
scope.

## Compatibility and requirements

Installation requires a jailbroken Kindle, root shell access, executable
userstore, and a supported writable boot hook. The installer recognizes the
jailbreak bridge that invokes `/mnt/us/emergency.sh`, or a stock userstore
startup script that sources the `MNTUS_PARAMS` file selected by
`/etc/sysconfig/paths`.

Choose a package for the device's userspace and native reader interfaces:

| Package | Included profiles | Calling convention |
| --- | --- | --- |
| `kindle5` | K4, Touch, and PW1 (`k4`, `kt1`, `pw1`) | ARM soft-float, old C++ string ABI |
| `kindlepw2` | Later Mobi8 (`mobi8-unsized`) | ARM soft-float, both string ABIs |
| `kindlehf` | Later Mobi8 (`mobi8`) | ARM hard-float, both string ABIs |

`/lib/ld-linux-armhf.so.3` identifies hard-float userspace. Package names alone
are not a compatibility check: activation inspects the installed ELF symbols,
virtual method layouts, and calling conventions, then selects an included
adapter. Unsupported or ambiguous interfaces stop activation. The profile
list lives in [architectures.json](architectures.json).

On Java firmware, activation discovers the format-capability class in the
installed JARs. A live VM probe selects JVMTI replacement or a shadow classpath;
JVMTI replacement also checks the original class bytes. KPP is enabled only
when its native hooks pass discovery. A `/etc/JAVA_FREE` configuration requires
a supported KPP reader, a native catalogue route, and a native application
association; the file's presence alone is insufficient.

`epubctl` is a native C++ executable. The boot hook launches a persistent copy
under `/var/local/kindle-epub`, which waits for userstore and starts the installed
payload. The payload uses process preloads and writable AppReg records. Stock firmware
files are not rewritten. K4 uses temporary bind mounts for its Java security
policy and pmond reader configuration; stopping the runtime removes them.
Catalogue integration uses stock Java on K4, a scanner plugin on Touch/PW1,
and a worker through the stock EBridge on later profiles.

## Install and read

Copy the matching `kindle-epub-ARCHITECTURE.tar.gz` to the Kindle. From a root
shell, replace `ARCHITECTURE` below with the package name:

```sh
tar -xzf /mnt/us/kindle-epub-ARCHITECTURE.tar.gz -C /mnt/us
/mnt/us/kindle-epub/epubctl install
/mnt/us/kindle-epub/epubctl status
```

Installation restarts affected reader services, verifies activation, and enables
the boot hook. Keep the extracted payload directory in place. Custom payload
paths must use only letters, digits, `/`, `_`, `.`, and `-`.

Copy `.epub` or `.EPUB` books into `/mnt/us/documents`, let the stock catalogue
scan finish, and open them from Home. If a previous installation modified
firmware files, restore those files with its matching uninstaller first.

## Stop, upgrade, or remove

Run these commands from the installed payload:

| Command | Effect |
| --- | --- |
| `./epubctl status` | Show the payload, boot setting, and runtime health. |
| `./epubctl stop` | Stop the runtime until the next start or boot. |
| `./epubctl start` | Activate the installed payload. |
| `./epubctl disable` | Stop and remove boot integration. |
| `./epubctl rollback` | Activate the previous payload, or restore stock behavior after a first install. |
| `./epubctl uninstall` | Stop, remove boot integration, and remove runtime state. |

For an upgrade, extract into a **new directory** and run its `epubctl install`.
Keep the previous payload directory for rollback. Do not overwrite a payload
that is in use.

Removal retains payload files, books, and catalogue reading state. Existing
EPUB entries may remain visible while the runtime is disabled. Owned AppReg
rows and reader toggles are restored; conflicting edits by another application
are reported instead of overwritten.

Use `epubctl status` first when diagnosing activation. Persistent lifecycle state
and the boot log are under `/var/local/kindle-epub`. Java discovery and probe
logs are under `/var/run/kindle-epub/prepare`; the catalogue worker log is
`/var/run/kindle-epub/worker.log`. Native errors also go to device syslog.

## Build packages

Use x86_64 Linux with CMake 3.24+, Ninja or Make, a JDK, and ECJ 3.32.0
for Java 1.4 target support. CMake downloads the pinned public KOReader
cross-toolchains and builds zlib, libzip, libxml2, and cJSON. No firmware image,
stock library, or Amazon JAR is a build input.

```sh
cmake -S . -B build/runtime -G Ninja -DEPUB_BUILD_RUNTIME=ON
CMAKE_BUILD_PARALLEL_LEVEL=4 cmake --build build/runtime
cmake --build build/runtime --target package
```

The package target writes `build/runtime/kindle-epub-ARCHITECTURE.tar.gz` for
each configured architecture. Each archive contains a `kindle-epub/` directory.
To build one architecture, add `-DEPUB_ARCHITECTURES=kindlepw2` at configuration;
a semicolon-separated list selects several. To install an unpacked payload:

```sh
cmake --install build/runtime --component kindlepw2 --prefix "$PWD/dist/kindlepw2"
```

`CMAKE_BUILD_PARALLEL_LEVEL` also bounds each nested dependency and native build.
Build directories and dependency caches stay under the chosen `-B` directory.
`-DEPUB_ENABLE_TRACE=ON` enables diagnostic tracing. Dependencies are linked
privately into device binaries, with their licenses included. Reader adapters
use the device's dynamic C++ runtime; the standalone controller links its C++
runtime statically so it can run on older firmware.

Use CMake cache options to supply existing tools:

- `KINDLE5_TOOLCHAIN_ROOT`, `KINDLEPW2_TOOLCHAIN_ROOT`, or `KINDLEHF_TOOLCHAIN_ROOT`
  selects an installed cross-toolchain. The corresponding `*_TOOLCHAIN_ARCHIVE`
  accepts a local archive from the pinned release.
- `EPUB_JDK_INCLUDE_ROOT` selects JNI headers, and `EPUB_ECJ_EXECUTABLE` selects ECJ.
  CMake's `FindJava` locates the JDK tools.
- `FETCHCONTENT_SOURCE_DIR_ZLIB`, `FETCHCONTENT_SOURCE_DIR_LIBXML2`,
  `FETCHCONTENT_SOURCE_DIR_LIBZIP`, and `FETCHCONTENT_SOURCE_DIR_CJSON` select
  extracted dependency sources for offline builds.

## Test and lint

Host builds need a C++11 compiler, CMake, pkg-config, and development files for
zlib, libxml2, libzip 1.12.1+, cJSON 1.7.17+, and libarchive. If system libraries
are older, build the pinned dependencies with a host C compiler:

```sh
cmake -S cmake/public-deps -B build/host-deps \
    -DCMAKE_INSTALL_PREFIX="$PWD/build/host-deps-install"
cmake --build build/host-deps --parallel
cmake --install build/host-deps
export PKG_CONFIG_PATH="$PWD/build/host-deps-install/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export CMAKE_PREFIX_PATH="$PWD/build/host-deps-install${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
```

Libarchive is a separate fixture dependency. Configure and run the existing
parser, catalogue, scanner, Mobi8, and KPP tests:

```sh
cmake -S . -B build/host -DEPUB_BUILD_TESTS=ON -DEPUB_BUILD_MOBI8_MODULE=ON
cmake --build build/host --parallel
ctest --test-dir build/host --output-on-failure
```

Use LLVM 18 for the checked-in formatting and lint rules:

```sh
cmake --build build/host --target format-check
cmake --build build/host --target lint
# Apply formatting:
cmake --build build/host --target format
```

Lint enables all `readability-*`, `bugprone-*`, and `performance-*` checks plus
selected C++11 modernization checks. Diagnostics fail the command. It uses the
build directory's `compile_commands.json`, so device-only sources require a
device build directory. For cross-builds, pass
`-DEPUB_CLANG_TIDY_EXTRA_ARGS=--gcc-toolchain=PATH` at configuration when Clang
needs the cross-toolchain's C++ headers. `EPUB_CLANG_FORMAT_EXECUTABLE`,
`EPUB_CLANG_TIDY_EXECUTABLE`, and `EPUB_RUN_CLANG_TIDY_EXECUTABLE` select explicit
LLVM 18 tools.

Host tests do not exercise device activation, boot hooks, or reader rendering.
Those require Kindle firmware on a device or in an emulator.

## Source map

| Path | Responsibility |
| --- | --- |
| [src/epub](src/epub) | Archive, path, XML, package, and identity parsing. |
| [src/reader](src/reader) | Kindle document serialization, CSS rewriting, links, and text positions. |
| [src/mobi8](src/mobi8) | Stock reader interface declarations, contract data, and adapters. |
| [src/kpp](src/kpp) | KPP discovery, hooks, and trampolines. |
| [src/scanner](src/scanner) | Catalogue metadata, scanner plugin, and worker. |
| [src/runtime](src/runtime) | Native capability probe, process injection, and JVM agent. |
| [src/java](src/java) | On-device Java discovery and class adaptation. |
| [src/control](src/control) | Native `epubctl`: activation, boot integration, catalogue ownership, and rollback. |
| [cmake](cmake) | Toolchains, dependencies, architecture builds, installation, packaging, and checks. |
| [tests](tests) | Host tests and fixture creation. |

Generated packages belong in `dist/` and build products in `build/`; both are
ignored by Git.
