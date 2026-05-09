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
- tmux, if you want to use the `pl` launcher

Example packages:

```sh
sudo apt install build-essential cmake git python3-dev tmux
```

Package names vary by distribution. Install the equivalent compiler, CMake, Git,
Python development, and tmux packages for your system.

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

Add the path containing `postline` (build/install/bin) to PATH in
.bashrc.

The standard setup to run Postline is to have the runtime occupying one
window and the user interface occupying another window.  The script `pl`
(at the same directory as the postline binary) runs both in a tmux
session.

## Integrated mode

Use `pl` to start Postline in integrated mode:

```sh
pl
```

## Separated mode

The integrated mode is not good in retaining screen output when the
system crashes.  There's a separated mode:

```sh
pld
```

Upon start it will print the command to run in another terminal session.
You need to copy and run the command to start the CLI user interface.

## `cli` vs `ftxcli`

Postline treats the user as an agent, and there are two adapters that
connects the user into the system: `cli` and `ftxcli`:

- `ftxcli`: the fancy CLI interface, used by `pl`.
- `cli`: the plain CLI interface, which accepts very rudimentary
  commands.


If you start postline with `pld`, the printed commands uses `cli` by
default.  By default each time you are allowed one line of input with
return, and it goes to the message body.  If the line starts with /,
it's interpreted as a command.  Currently it supports three commands
only:

- /t target: set To: to target
- /s subject: set Subject: to subject
- /x : send the exit message to runtime.




