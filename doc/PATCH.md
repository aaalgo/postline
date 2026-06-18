# Patch Format

Postline patch documents are consumed by `apply_patch`, implemented in
`src/shell/apply_patch.cpp`. The tool accepts a patch from stdin, from `-`, or
from a path argument:

```sh
apply_patch --root /path/to/repo change.patch
apply_patch --root /path/to/repo --check change.patch
apply_patch --rollback /path/to/repo/.patch/20260617223821-1614ba.rollback
```

The format is byte-oriented. Marker lines are matched after removing one
trailing `LF` or `CRLF`. File payload lines keep their original line endings.

## Grammar

```text
patch       := comment* begin item* end comment*
item        := comment | op
begin       := "*** Begin Patch" eol
end         := "*** End Patch" eol?
comment     := "#" bytes eol

op          := add_file | delete_file | update_file

add_file    := "*** Add File: " path eol (comment | add_line)*
add_line    := "+" bytes

delete_file := "*** Delete File: " path eol

update_file := "*** Update File: " path eol comment* move_to? (comment | hunk)*
move_to     := "*** Move to: " path eol
hunk        := "@@" eol (comment | hunk_line)+
hunk_line   := (" " | "-" | "+") bytes
```

Operation boundaries are any line beginning with `*** Add File: `,
`*** Delete File: `, `*** Update File: `, or the exact end marker
`*** End Patch`.

Lines beginning with `#` are comments and are ignored. To add, remove, or match
a file line whose first content byte is `#`, put the normal patch-line prefix
before it, for example `+#define X 1`, `-#define X 0`, or ` # heading`.

Add-file payload lines must begin with `+`. The stored payload is the remainder
of the line, including its newline if present. Update hunk lines use:

- ` ` for context lines.
- `-` for removed lines.
- `+` for inserted lines.

## Paths

Paths are normalized before use:

- NUL bytes are rejected.
- Paths must be valid UTF-8.
- Backslashes are converted to slashes.
- Leading `./` prefixes are removed.
- Empty paths are rejected.
- Absolute paths are rejected.
- Empty path components and `.` components are collapsed.
- `..` components are rejected.

After normalization, every path is interpreted relative to `--root`.

## Semantics

`apply_patch` plans the whole patch against an in-memory view before writing any
files. Application is guarded by exact content checks, so a file changed between
planning and writing causes the patch to fail instead of overwriting the new
content.

Add creates a new file and any missing parent directories. The destination must
not already exist.

Delete removes an existing regular file. The file content must still match the
planned content when the delete action runs.

Update applies each hunk in order. For every hunk, the old side is the context
plus removed lines, and the new side is the context plus added lines. The old
side must match exactly one location in the current planned file content.

Update with `*** Move to:` moves the file. If the updated content is unchanged,
the action is a rename. If the content changes, the source is deleted and a new
destination file is created.

All file writes are atomic within the destination directory.

## Examples

Add a new file:

```text
*** Begin Patch
# Create a task list.
*** Add File: notes/todo.txt
+write patch docs
+add tests
*** End Patch
```

Update a file:

```text
*** Begin Patch
# Replace one unique matching block.
*** Update File: src/example.txt
@@
 first line
-old value
+new value
 last line
*** End Patch
```

Delete a file:

```text
*** Begin Patch
# Remove generated output.
*** Delete File: generated/cache.txt
*** End Patch
```

Move a file without changing content:

```text
*** Begin Patch
# Rename without changing content.
*** Update File: old/name.txt
*** Move to: new/name.txt
*** End Patch
```

Move a file and edit it:

```text
*** Begin Patch
# Rename and update content.
*** Update File: docs/draft.txt
*** Move to: docs/final.txt
@@
 Title: Draft
-Status: draft
+Status: final
*** End Patch
```

Multiple operations can appear in one patch:

```text
*** Begin Patch
# Multiple operations are planned together before any write happens.
*** Add File: a.txt
+hello
*** Update File: b.txt
@@
-before
+after
*** Delete File: old.txt
*** End Patch
```

An add-only hunk can create content in an empty existing file:

```text
*** Begin Patch
*** Update File: empty.txt
@@
+first line
*** End Patch
```

The same add-only hunk is rejected for a non-empty file because the insertion
location would be ambiguous.

## Rollback Records

Before applying actions, `apply_patch` writes a rollback record under:

```text
<root>/.patch/<YYYYmmddHHMMSS>-<random-hex>.rollback
```

The record is updated after each applied action by incrementing `applied_count`
and atomically replacing the rollback file. `--rollback <record>` reads the
record and reverts the first `applied_count` actions in reverse order, updating
`applied_count` after each reverted action.

The rollback format is a C++ binary format. It is not Python pickle-compatible.
All integer fields are unsigned 64-bit little-endian values. Strings are stored
as:

```text
string := byte_length:u64 payload:byte[byte_length]
```

Record layout:

```text
magic         := "POSTLINE_PATCH_ROLLBACK_V1\n"
root          := string
applied_count := u64
action_count  := u64
actions       := action[action_count]
```

Each action is serialized with a fixed field set:

```text
action        := kind:u64
                 path_count:u64
                 paths:string[path_count]
                 path:string
                 src:string
                 dst:string
                 expected:string
                 content:string
```

Action kind values:

```text
1  create_directories
2  create_file
3  delete_file
4  replace_file
5  move_file
```

Field use by action:

| Kind | Used fields |
| --- | --- |
| `create_directories` | `paths` |
| `create_file` | `path`, `content` |
| `delete_file` | `path`, `expected` |
| `replace_file` | `path`, `expected`, `content` |
| `move_file` | `src`, `dst`, `expected` |

Unused string fields are serialized as empty strings, and unused `paths` is
serialized with `path_count = 0`.

Rollback validation is deliberately strict. Paths inside the record are
normalized with the same path rules used for patch documents. For content
actions, rollback checks the current file content before changing it:

- `create_file` rollback removes the file only if it still equals `content`.
- `delete_file` rollback recreates the file only if the path does not exist.
- `replace_file` rollback restores `expected` only if the file still equals
  `content`.
- `move_file` rollback renames `dst` back to `src` only if `dst` still equals
  `expected` and `src` does not exist.
- `create_directories` rollback removes created directories in reverse order.
