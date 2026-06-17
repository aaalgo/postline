# Shell Tools

Shell tools are small command-line helpers used by agents and humans. New tools
should be implemented as Python scripts under `python/shell`.

## Location and Names

Create a new tool as:

```
python/shell/new_tool.py
```

The installed command name omits the `.py` suffix:

```
install/bin/shell/new_tool
```

## Documentation

Every shell tool must accept `--doc`, print usage documentation to stdout, and
exit successfully without performing the tool action. This output is intended
for automatic prompt construction, so it should be complete enough to describe:

- what the tool does;
- the command syntax;
- all important options;
- output format;
- at least one copyable example.

Keep `--doc` deterministic. It should not read project files, access the
network, or require other arguments.

## Python Pattern

Use an executable Python script with a shebang:

```python
#!/usr/bin/env python3
```

Prefer `argparse` for option parsing. Set `prog` to the installed command name
without `.py`, so help and error messages match the command users run:

```python
parser = argparse.ArgumentParser(prog="tool", description="...")
parser.add_argument("--doc", action="store_true", help="print usage and exit")
```

Handle `--doc` immediately after parsing and before doing any work:

```python
if args.doc:
    print_doc()
    return 0
```

Use normal process exit codes: return `0` for success, `1` for operational
failures, and let argument parsing return `2` for invalid command-line usage.

## Build Integration

CMake copies every `python/shell/*.py` script to `install/bin/shell` using the
source basename without the `.py` suffix. The source file should be executable
in the repository so the copied command is directly runnable.

Do not add new C++ shell tools unless there is a concrete reason Python is not a
good fit. Existing C++ tools may be replaced by Python tools with the same
installed command name.
