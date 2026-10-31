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

Installation requires a jailbroken Kindle, root shell access, and an opkg
environment installed under `/mnt/us/.eink-sdk` with its launcher and runtime
links available. Activation uses writable runtime state and the existing
firmware services. Run it after each reboot from the library scriptlet or a
root shell.

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

`epubctl` is a native C++ executable installed by opkg. It starts the reader
adapters and catalogue worker on demand. The payload uses process preloads and
writable AppReg records. Stock firmware files are not rewritten. K4 uses
temporary bind mounts for its Java security policy and pmond reader
configuration; stopping the runtime removes them. Catalogue integration uses
stock Java on K4, a scanner plugin on Touch/PW1, and a worker through the stock
EBridge on later profiles.

## Install and read

Install the package from a configured feed containing it:

```sh
/mnt/us/.eink-sdk/control/run opkg install kindle-epub
```

Exit the package manager, then open **Native EPUB** from the Kindle library, or
start it from a root shell:

```sh
/mnt/us/.eink-sdk/control/run epubctl start
/mnt/us/.eink-sdk/control/run epubctl status
```

Activation restarts affected reader services and verifies the selected native
interfaces. Installation itself does not start the runtime. Repeat activation
after each reboot. No persistent boot hook is installed or required.
The library scriptlet uses `start --background` to detach before the framework
restarts, with activation output in `/var/run/kindle-epub.log`.

Copy `.epub` or `.EPUB` books into `/mnt/us/documents`, let the stock catalogue
scan finish, and open them from Home.

## Stop, upgrade, or remove

```sh
/mnt/us/.eink-sdk/control/run epubctl stop
/mnt/us/.eink-sdk/control/run opkg upgrade kindle-epub
# Or remove the package:
/mnt/us/.eink-sdk/control/run opkg remove kindle-epub
```

Stop from a root shell before upgrading or removing the package. Package hooks
reject changes while activation or recovery state remains: restarting the
framework during an opkg transaction can terminate a graphical package manager.
After upgrading, open Native EPUB again to activate the installed version.

Stopping restores owned AppReg rows and reader toggles. Conflicting edits by
another application are reported instead of overwritten. Books, catalogue
reading state, and thumbnails are retained. Existing EPUB entries may remain
visible while the runtime is stopped.

## Paths

Package files are stored under `/mnt/us/.eink-sdk`, with runtime links under
`/var/run/eink-sdk`:

| Package path | Contents |
| --- | --- |
| `usr/bin/epubctl` | Runtime controller. |
| `usr/libexec/kindle-epub` | Native probe and catalogue worker. |
| `usr/lib/kindle-epub` | Reader adapters and JVM support libraries. |
| `usr/share/kindle-epub` | Java helper, native contracts, runtime metadata and launcher template. |
| `var/lib/kindle-epub` | Persistent AppReg and reader-toggle recovery state. |

The library launcher is `/mnt/us/documents/kindle-epub.sh`. Volatile runtime
state and locks are under `/var/run/kindle-epub`; the lifecycle lock is
`/var/run/kindle-epub.lock`. Java discovery and probe logs are in its `prepare`
subdirectory, and the worker log is `worker.log`. Native errors also go to syslog.
Firmware-owned databases and thumbnails retain their stock locations.

## Cross-build

Supply a CMake cross-toolchain file and sysroot for one of the three supported
ABIs. The sysroot needs zlib, libxml2, libzip 1.11.4+, cJSON 1.7.17+, SQLite,
and the C++ runtime. Host tools include pkg-config configured for that sysroot,
ECJ with Java 1.4 support, and a JDK with development tools and JNI headers.

For example, configure the hard-float runtime with paths from your build
environment:

```sh
cmake -S . -B build/kindlehf \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DEPUB_BUILD_RUNTIME=ON \
  -DEPUB_ARCHITECTURE=kindlehf \
  -DEPUB_LIBZIP_SOURCE_DIR=/path/to/libzip-source \
  -DPKG_CONFIG_EXECUTABLE=/path/to/target-pkg-config \
  -DEPUB_ECJ_EXECUTABLE=/path/to/ecj \
  -DEPUB_JDK_INCLUDE_ROOT=/path/to/jdk/include
cmake --build build/kindlehf --parallel
DESTDIR=/path/to/package-root cmake --install build/kindlehf
```

Use `kindle5` or `kindlepw2` with the corresponding toolchain for the other ABIs.
CMake builds each architecture's firmware interface variants from
`architectures.json`. Set `JAVA_HOME` to the host JDK or supply the
`Java_JAVA_EXECUTABLE`, `Java_JAVAC_EXECUTABLE`, `Java_JAVADOC_EXECUTABLE`, and
`Java_JAR_EXECUTABLE` cache variables explicitly.

The libzip source must match the headers installed in the sysroot. The runtime
builds a private store/deflate-only archive from that source and hides its
symbols inside the adapters. This keeps incompatible crypto libraries out of
firmware processes. CMake does not download toolchains or dependencies, or
create package archives. No firmware image, stock library, or Amazon JAR is a
build input. `EPUB_ENABLE_TRACE=ON` enables diagnostic tracing.

## Test and lint

Host builds need a C++11 compiler, CMake, pkg-config, and development files for
zlib, libxml2, libzip 1.11.4+, cJSON 1.7.17+, and libarchive.

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

Host tests do not exercise device activation or reader rendering.
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
| [src/control](src/control) | Native `epubctl`: activation, catalogue ownership and recovery. |
| [cmake](cmake) | Architecture builds, Java compilation, installation and checks. |
| [tests](tests) | Host tests and fixture creation. |

Local builds belong in `build/`, which is ignored by Git. Packaging systems can
supply their own build and staging directories.
