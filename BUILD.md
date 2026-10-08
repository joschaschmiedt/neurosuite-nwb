# Building neurosuite-nwb

neurosuite-nwb is built with CMake. Its dependencies come from [vcpkg](https://vcpkg.io), which is
included as a git submodule, pinned to the same baseline as dh5cpp:

| Dependency | Version | Used for |
|------------|---------|----------|
| [aqnwb](https://github.com/NeurodataWithoutBorders/aqnwb) | 0.4.0 | reading NWB files; from the overlay port in `ports/aqnwb` |
| [HDF5](https://www.hdfgroup.org/solutions/hdf5/) | 1.14.6 | file access (C and C++ libraries, with zlib) |
| [GoogleTest](https://github.com/google/googletest) | 1.17.0 | unit tests (only with the `tests` feature) |

All dependencies are linked statically, so the plugin is a single file that needs no HDF5 at run
time. On Linux the C++ runtime is linked statically too if the static libstdc++ is installed
(Fedora: `sudo dnf install libstdc++-static`); otherwise CMake warns and the plugin uses the
system's libstdc++.

The plugin is built against the plugin interface of an installed NeuroScope (the CMake package
`NeuroScopePlugin`). Pass its installation prefix in `CMAKE_PREFIX_PATH`.

## Requirements

- CMake 3.21 or newer, Ninja, a C++17 compiler
- git, curl, zip, unzip and tar, which vcpkg needs
- NeuroScope 3, installed (e.g. `cmake --install` into a prefix)

## Getting the sources

```bash
git clone --recurse-submodules <url> neurosuite-nwb
```

In an existing clone, fetch the vcpkg submodule with `git submodule update --init`.

## Building with the presets

```bash
cmake --preset ninja -DCMAKE_PREFIX_PATH=/path/to/neuroscope/install
cmake --build --preset ninja-release
ctest --preset ninja-release
```

The `dev` preset is a Debug build with AddressSanitizer, UBSan and warnings as errors. A plugin
built with sanitizers cannot be loaded by a NeuroScope built without them; use it for the tests.

```bash
cmake --preset dev -DCMAKE_PREFIX_PATH=/path/to/neuroscope/install
cmake --build --preset dev
ctest --preset dev
```

On Windows, use the `default` preset (Visual Studio 2022, triplet `x64-windows-static-md`) and the
`release` build and test presets.

## Trying the plugin

NeuroScope loads plugins from its plugin directory and from the directories in
`NEUROSCOPE_PLUGIN_PATH`:

```bash
NEUROSCOPE_PLUGIN_PATH=build/ninja/lib/Release neuroscope file.nwb
```

`nwbinfo file.nwb --samples 5` prints the streams, channels and events the plugin sees.

To install the plugin into NeuroScope's plugin directory, install with the prefix of NeuroScope:
`cmake --install build/ninja --config Release --prefix /path/to/neuroscope/install`.

## Test files

`tests/data/rate.nwb` and `tests/data/timestamps.nwb` are written by
`tests/data/make_test_files.py` with pynwb:

```bash
uv run --with pynwb==3.1.2 python tests/data/make_test_files.py tests/data
```

The tests also open every `.nwb` file in the directory given by `NEUROSUITE_NWB_EXTRA_TEST_DATA`,
e.g. the files from DANDI in the neurosuite's `data/nwb`:

```bash
cmake --preset dev -DNEUROSUITE_NWB_EXTRA_TEST_DATA=/path/to/neurosuite/data/nwb
```
