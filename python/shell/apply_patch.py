#!/usr/bin/env python3
import argparse
import os
import pickle
import secrets
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


class PatchError(Exception):
    pass


def check(cond: bool, msg: str) -> None:
    if not cond:
        raise PatchError(msg)


def strip_eol(line: bytes) -> bytes:
    if line.endswith(b"\r\n"):
        return line[:-2]
    if line.endswith(b"\n"):
        return line[:-1]
    return line


def is_comment(line: bytes) -> bool:
    return line.startswith(b"#")


def is_operation_boundary(line: bytes) -> bool:
    marker = strip_eol(line)
    return marker == b"*** End Patch" \
        or marker.startswith(b"*** Add File: ") \
        or marker.startswith(b"*** Delete File: ") \
        or marker.startswith(b"*** Update File: ")


def safe_path(path: bytes | str) -> str:
    if isinstance(path, bytes):
        check(b"\x00" not in path, "path contains NUL")
        path_str = path.decode("utf-8", "strict")
    else:
        check("\x00" not in path, "path contains NUL")
        path_str = path

    path_str = path_str.replace("\\", "/")
    while path_str.startswith("./"):
        path_str = path_str[2:]

    parts = []
    for part in path_str.split("/"):
        if part in ("", "."):
            continue
        check(part != "..", f"path escapes root: {path_str}")
        parts.append(part)

    check(parts, "empty path")
    check(not os.path.isabs(path_str), f"absolute path rejected: {path_str}")
    return "/".join(parts)


def full_path(root: Path, path: str) -> Path:
    return root / Path(path)


def read_file(root: Path, path: str) -> bytes:
    try:
        return full_path(root, path).read_bytes()
    except FileNotFoundError:
        raise PatchError(f"file does not exist: {path}") from None


def write_file_atomic(root: Path, path: str, content: bytes) -> None:
    target = full_path(root, path)
    check(target.parent.is_dir(), f"parent directory does not exist: {path}")
    tmp = target.with_name(f".{target.name}.tmp-{os.getpid()}-{secrets.token_hex(4)}")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o666)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(content)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, target)
    except Exception:
        try:
            tmp.unlink()
        except FileNotFoundError:
            pass
        raise


@dataclass
class PatchLine:
    kind: str
    text: bytes


@dataclass
class PatchHunk:
    lines: list[PatchLine]


@dataclass
class PatchOp:
    kind: str
    path: str
    move_to: str | None = None
    hunks: list[PatchHunk] | None = None
    add_lines: list[bytes] | None = None


@dataclass
class CreateDirectories:
    paths: list[str]

    kind = "create_directories"

    def apply(self, root: Path) -> None:
        for path in self.paths:
            target = full_path(root, path)
            check(not target.exists(), f"directory already exists: {path}")
            target.mkdir()

    def rollback(self, root: Path) -> None:
        for path in reversed(self.paths):
            target = full_path(root, path)
            check(target.is_dir(), f"not a directory during rollback: {path}")
            target.rmdir()

    def to_record(self) -> dict[str, Any]:
        return {"kind": self.kind, "paths": self.paths}


@dataclass
class CreateFile:
    path: str
    content: bytes

    kind = "create_file"

    def apply(self, root: Path) -> None:
        target = full_path(root, self.path)
        check(not target.exists(), f"path already exists: {self.path}")
        write_file_atomic(root, self.path, self.content)

    def rollback(self, root: Path) -> None:
        check(read_file(root, self.path) == self.content,
              f"file changed after apply: {self.path}")
        full_path(root, self.path).unlink()

    def to_record(self) -> dict[str, Any]:
        return {"kind": self.kind, "path": self.path, "content": self.content}


@dataclass
class DeleteFile:
    path: str
    expected: bytes

    kind = "delete_file"

    def apply(self, root: Path) -> None:
        check(read_file(root, self.path) == self.expected,
              f"file changed before delete: {self.path}")
        full_path(root, self.path).unlink()

    def rollback(self, root: Path) -> None:
        check(not full_path(root, self.path).exists(),
              f"path exists during rollback: {self.path}")
        write_file_atomic(root, self.path, self.expected)

    def to_record(self) -> dict[str, Any]:
        return {"kind": self.kind, "path": self.path, "expected": self.expected}


@dataclass
class ReplaceFile:
    path: str
    expected: bytes
    content: bytes

    kind = "replace_file"

    def apply(self, root: Path) -> None:
        check(read_file(root, self.path) == self.expected,
              f"file changed before replace: {self.path}")
        write_file_atomic(root, self.path, self.content)

    def rollback(self, root: Path) -> None:
        check(read_file(root, self.path) == self.content,
              f"file changed after apply: {self.path}")
        write_file_atomic(root, self.path, self.expected)

    def to_record(self) -> dict[str, Any]:
        return {
            "kind": self.kind,
            "path": self.path,
            "expected": self.expected,
            "content": self.content,
        }


@dataclass
class MoveFile:
    src: str
    dst: str
    expected: bytes

    kind = "move_file"

    def apply(self, root: Path) -> None:
        check(read_file(root, self.src) == self.expected,
              f"file changed before move: {self.src}")
        check(not full_path(root, self.dst).exists(),
              f"move destination exists: {self.dst}")
        full_path(root, self.src).rename(full_path(root, self.dst))

    def rollback(self, root: Path) -> None:
        check(read_file(root, self.dst) == self.expected,
              f"file changed after move: {self.dst}")
        check(not full_path(root, self.src).exists(),
              f"move source exists during rollback: {self.src}")
        full_path(root, self.dst).rename(full_path(root, self.src))

    def to_record(self) -> dict[str, Any]:
        return {
            "kind": self.kind,
            "src": self.src,
            "dst": self.dst,
            "expected": self.expected,
        }


Action = CreateDirectories | CreateFile | DeleteFile | ReplaceFile | MoveFile


def action_from_record(record: dict[str, Any]) -> Action:
    kind = record["kind"]
    if kind == "create_directories":
        return CreateDirectories([safe_path(path) for path in record["paths"]])
    if kind == "create_file":
        return CreateFile(safe_path(record["path"]), record["content"])
    if kind == "delete_file":
        return DeleteFile(safe_path(record["path"]), record["expected"])
    if kind == "replace_file":
        return ReplaceFile(
            safe_path(record["path"]),
            record["expected"],
            record["content"],
        )
    if kind == "move_file":
        return MoveFile(
            safe_path(record["src"]),
            safe_path(record["dst"]),
            record["expected"],
        )
    raise PatchError(f"unknown action kind: {kind}")


def parse_patch(data: bytes) -> list[PatchOp]:
    lines = data.splitlines(keepends=True)
    check(lines, "empty patch")

    ops = []
    i = 0
    while i < len(lines) and is_comment(lines[i]):
        i += 1
    check(i < len(lines) and strip_eol(lines[i]) == b"*** Begin Patch",
          "missing begin sentinel")
    i += 1

    while i < len(lines):
        if is_comment(lines[i]):
            i += 1
            continue

        line = strip_eol(lines[i])
        if line == b"*** End Patch":
            i += 1
            while i < len(lines) and is_comment(lines[i]):
                i += 1
            check(i == len(lines), "content after end sentinel")
            return ops

        if line.startswith(b"*** Add File: "):
            path = safe_path(line[len(b"*** Add File: "):])
            i += 1
            add_lines = []
            while i < len(lines):
                if is_comment(lines[i]):
                    i += 1
                    continue
                if is_operation_boundary(lines[i]):
                    break
                check(lines[i].startswith(b"+"),
                      f"add-file line must start with '+': {path}")
                add_lines.append(lines[i][1:])
                i += 1
            ops.append(PatchOp("add_file", path, add_lines=add_lines))
            continue

        if line.startswith(b"*** Delete File: "):
            path = safe_path(line[len(b"*** Delete File: "):])
            ops.append(PatchOp("delete_file", path))
            i += 1
            continue

        if line.startswith(b"*** Update File: "):
            path = safe_path(line[len(b"*** Update File: "):])
            i += 1
            move_to = None
            while i < len(lines) and is_comment(lines[i]):
                i += 1
            if i < len(lines) and strip_eol(lines[i]).startswith(b"*** Move to: "):
                move_to = safe_path(strip_eol(lines[i])[len(b"*** Move to: "):])
                i += 1

            hunks = []
            while i < len(lines):
                if is_comment(lines[i]):
                    i += 1
                    continue
                if is_operation_boundary(lines[i]):
                    break
                marker = strip_eol(lines[i])
                check(marker == b"@@", f"expected hunk header for update: {path}")
                i += 1
                hunk_lines = []
                while i < len(lines):
                    if is_comment(lines[i]):
                        i += 1
                        continue
                    marker = strip_eol(lines[i])
                    if marker == b"@@" or is_operation_boundary(lines[i]):
                        break
                    check(lines[i][:1] in (b" ", b"-", b"+"),
                          f"bad hunk line prefix in {path}")
                    prefix = lines[i][:1]
                    kind = {b" ": "context", b"-": "remove", b"+": "add"}[prefix]
                    hunk_lines.append(PatchLine(kind, lines[i][1:]))
                    i += 1
                check(hunk_lines, f"empty hunk in {path}")
                hunks.append(PatchHunk(hunk_lines))

            check(hunks or move_to is not None, f"update has no hunks: {path}")
            ops.append(PatchOp("update_file", path, move_to=move_to, hunks=hunks))
            continue

        raise PatchError(f"unknown patch line: {line.decode('utf-8', 'replace')}")

    raise PatchError("missing end sentinel")


def split_file_lines(content: bytes) -> list[bytes]:
    return content.splitlines(keepends=True)


def find_unique(haystack: list[bytes], needle: list[bytes]) -> int:
    if not needle:
        check(not haystack, "add-only hunk is ambiguous for non-empty file")
        return 0
    matches = []
    limit = len(haystack) - len(needle) + 1
    for i in range(max(limit, 0)):
        if haystack[i:i + len(needle)] == needle:
            matches.append(i)
    check(matches, "hunk does not match")
    check(len(matches) == 1, "hunk matches multiple locations")
    return matches[0]


def apply_hunks(content: bytes, hunks: list[PatchHunk]) -> bytes:
    file_lines = split_file_lines(content)
    for hunk in hunks:
        old_lines = [
            line.text
            for line in hunk.lines
            if line.kind in ("context", "remove")
        ]
        new_lines = [
            line.text
            for line in hunk.lines
            if line.kind in ("context", "add")
        ]
        pos = find_unique(file_lines, old_lines)
        file_lines = file_lines[:pos] + new_lines + file_lines[pos + len(old_lines):]
    return b"".join(file_lines)


class Planner:
    def __init__(self, root: Path):
        self.root = root
        self.files: dict[str, bytes | None] = {}
        self.created_dirs: set[str] = set()
        self.actions: list[Action] = []

    def read_planned(self, path: str) -> bytes | None:
        if path in self.files:
            return self.files[path]
        target = full_path(self.root, path)
        if target.is_file():
            return target.read_bytes()
        if target.exists():
            raise PatchError(f"path is not a file: {path}")
        return None

    def dir_exists(self, path: str) -> bool:
        if path in self.files and self.files[path] is not None:
            return False
        if path in self.created_dirs:
            return True
        target = full_path(self.root, path)
        if target.is_dir():
            return True
        if target.exists():
            raise PatchError(f"path is not a directory: {path}")
        return False

    def ensure_parent_dirs(self, path: str) -> None:
        parent = Path(path).parent.as_posix()
        if parent == ".":
            return

        missing = []
        parts = parent.split("/")
        for i in range(1, len(parts) + 1):
            current = "/".join(parts[:i])
            if not self.dir_exists(current):
                missing.append(current)
                self.created_dirs.add(current)

        if missing:
            self.actions.append(CreateDirectories(missing))

    def add_op(self, op: PatchOp) -> None:
        if op.kind == "add_file":
            check(self.read_planned(op.path) is None,
                  f"add destination exists: {op.path}")
            content = b"".join(op.add_lines or [])
            self.ensure_parent_dirs(op.path)
            self.actions.append(CreateFile(op.path, content))
            self.files[op.path] = content
            return

        if op.kind == "delete_file":
            expected = self.read_planned(op.path)
            check(expected is not None, f"delete target does not exist: {op.path}")
            self.actions.append(DeleteFile(op.path, expected))
            self.files[op.path] = None
            return

        if op.kind == "update_file":
            expected = self.read_planned(op.path)
            check(expected is not None, f"update target does not exist: {op.path}")
            content = apply_hunks(expected, op.hunks or [])

            if op.move_to is None:
                self.actions.append(ReplaceFile(op.path, expected, content))
                self.files[op.path] = content
                return

            check(self.read_planned(op.move_to) is None,
                  f"move destination exists: {op.move_to}")
            self.ensure_parent_dirs(op.move_to)
            if content == expected:
                self.actions.append(MoveFile(op.path, op.move_to, expected))
            else:
                self.actions.append(DeleteFile(op.path, expected))
                self.actions.append(CreateFile(op.move_to, content))
            self.files[op.path] = None
            self.files[op.move_to] = content
            return

        raise PatchError(f"unknown op kind: {op.kind}")


def plan_actions(root: Path, ops: list[PatchOp]) -> list[Action]:
    planner = Planner(root)
    for op in ops:
        planner.add_op(op)
    return planner.actions


def rollback_path(root: Path) -> Path:
    ts = time.strftime("%Y%m%d%H%M%S", time.localtime())
    return root / ".patch" / f"{ts}-{secrets.token_hex(3)}.pkl"


def write_rollback(path: Path, record: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f".{path.name}.tmp")
    with tmp.open("wb") as f:
        pickle.dump(record, f)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def apply_actions(root: Path, actions: list[Action]) -> Path:
    records = [action.to_record() for action in actions]
    rb_path = rollback_path(root)
    rollback = {
        "version": 1,
        "created_at": time.time(),
        "root": str(root),
        "actions": records,
        "applied_count": 0,
    }
    write_rollback(rb_path, rollback)

    for action in actions:
        action.apply(root)
        rollback["applied_count"] += 1
        write_rollback(rb_path, rollback)

    return rb_path


def rollback(record_path: Path) -> None:
    with record_path.open("rb") as f:
        record = pickle.load(f)

    check(record.get("version") == 1, "unsupported rollback version")
    root = Path(record["root"]).resolve()
    actions = [action_from_record(item) for item in record["actions"]]
    applied_count = int(record["applied_count"])
    check(0 <= applied_count <= len(actions), "invalid applied_count")

    for action in reversed(actions[:applied_count]):
        action.rollback(root)
        applied_count -= 1
        record["applied_count"] = applied_count
        write_rollback(record_path, record)


def read_input(path: str | None) -> bytes:
    if path is None or path == "-":
        return sys.stdin.buffer.read()
    return Path(path).read_bytes()


def print_doc() -> None:
    print(
        "# apply_patch accepts and applies a patch file from stdin.\n"
        "# The patch file consists multiple patch operations.\n"
        "# Lines begin with # are comments.\n"
        "# Example:\n"
        "*** Begin Patch\n"
        "*** Add File: notes/todo.txt\n"
        "+write patch docs\n"
        "+add shell coverage\n"
        "+keep examples copyable\n"
        "#\n"
        "*** Update File: src/example.txt\n"
        "@@\n"
        " first line\n"
        "-old line\n"
        "-old line2\n"
        "-old line3\n"
        "+new line\n"
        "+new line2\n"
        " last line\n"
        "*** Update File: old/name.txt\n"
        "*** Move to: new/name.txt\n"
        "*** Update File: docs/draft.txt\n"
        "*** Move to: docs/final.txt\n"
        "@@\n"
        " Title: Release Notes\n"
        "-Status: draft\n"
        "+Status: final\n"
        "*** Delete File: generated/cache.txt\n"
        "*** End Patch"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("patch", nargs="?", help="patch file, or stdin if omitted")
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--check", action="store_true", help="check only")
    parser.add_argument("--doc", action="store_true", help="print patch format examples")
    parser.add_argument("--rollback", help="rollback pickle path")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.doc:
            print_doc()
            return 0

        if args.rollback:
            rollback(Path(args.rollback))
            print(f"Rolled back: {args.rollback}")
            return 0

        root = Path(args.root).resolve()
        patch = parse_patch(read_input(args.patch))
        actions = plan_actions(root, patch)
        if args.check:
            print(f"Patch OK: {len(actions)} action(s)")
            return 0

        rb_path = apply_actions(root, actions)
        print(f"Applied patch. Rollback reference: {rb_path}")
        return 0
    except PatchError as e:
        print(f"patch.py: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
