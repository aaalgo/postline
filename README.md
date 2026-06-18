Postline: an agent runtime based on message-passing.
======================================================

# Requirements

Postline supports modern mainstream Linux distributions only.
The build uses CMake and automatically downloads C++ dependencies.

- Linux
- CMake 3.20 or newer
- A C++23-capable compiler, such as a recent GCC or Clang
- Git
- Python 3 with development headers
- Bash and standard Unix command-line tools

Example packages:

```sh
sudo apt install build-essential cmake git python3-dev
```

Package names vary by distribution. Install the equivalent compiler, CMake, Git,
Python development packages for your system.

# Build

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

The Debug mode is sufficently efficient (>5000 messages/s) and there's no need to go
Release mode.

By default, the build places the runnable installation tree under:

```sh
build/install
```

The main binaries and launch scripts are in:

```sh
build/install/bin
```


# Running

Add the path containing `pl`(that is, the full path to `build/install/bin`), to your `PATH`, for example
from `.bashrc`.

Run Postline with the command-line launcher:

```sh
pl
```
