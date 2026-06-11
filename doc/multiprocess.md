# Multiprocess Bitcoin

_This document describes usage of the multiprocess feature. For design information, see the [design/multiprocess.md](design/multiprocess.md) file._

## Debugging

The `-debug=ipc` command line option can be used to see requests and responses between processes.

## Installation

[Cap'n Proto](https://capnproto.org/) is a required dependency. See [build-unix.md](build-unix.md) and [build-osx.md](build-osx.md) for information about installing dependencies.

### Depends installation

Alternatively the [depends system](../depends) can be used to avoid needing to install local dependencies:

```
cd <BITCOIN_SOURCE_DIRECTORY>
make -C depends
# Set host platform to output of gcc -dumpmachine or clang -dumpmachine or check the depends/ directory for the generated subdirectory name
HOST_PLATFORM="x86_64-pc-linux-gnu"
cmake -B build --toolchain=depends/$HOST_PLATFORM/toolchain.cmake
cmake --build build
build/bin/bitcoin -m node -regtest -printtoconsole -debug=ipc
```

### Cross-compiling

When cross-compiling and not using depends, native code generation tools from
[Cap'n Proto](https://capnproto.org/) are required. They can be passed to the
cmake build by specifying `-DCAPNP_EXECUTABLE=/path/to/capnp
-DCAPNPC_CXX_EXECUTABLE=/path/to/capnpc-c++` options.

## Usage

Recommended way to use multiprocess binaries is to invoke `bitcoin` CLI like `bitcoin -m node -debug=ipc`.

When the `-m` (`--multiprocess`) option is used the `bitcoin` command will execute the multiprocess binary instead of the monolithic one (`bitcoin-node` instead of `bitcoind`). The multiprocess binary can also be invoked directly, but this is not recommended as it may change or be renamed in the future, and it is not installed in the PATH.

The multiprocess binaries currently function the same as the monolithic binaries, except they support an `-ipcbind` option.
