import os
import subprocess
from pathlib import Path


def _postline_home():
    if os.environ.get("POSTLINE_HOME"):
        return Path(os.environ["POSTLINE_HOME"])

    return Path(__file__).resolve().parents[2]


def _tool_dir():
    return _postline_home() / "bin" / "shell"


def _shell_instruction_records():
    tool_dir = _tool_dir()
    if not tool_dir.exists():
        return []

    records = []
    for path in sorted(tool_dir.glob("*")):
        if not path.is_file() or path.name.startswith("."):
            continue

        tool_name = path.name
        subject = f"{tool_name} --doc"
        doc = subprocess.check_output(
            [str(path), "--doc"],
            stderr=subprocess.STDOUT,
            text=True,
        )

        records.append(("ai", "shell", subject, ""))
        records.append(("shell", "ai", subject, doc))

    return records


def _plain_message(From, To, Subject, body=""):
    return f"""From: {From}
To: {To}
Subject: {Subject}

{body}"""


def generate_shell_instruction(plain=True):
    records = _shell_instruction_records()
    if plain:
        return [_plain_message(*record) for record in records]

    from ._postline import Message

    return [
        Message({
            "From": From,
            "To": To,
            "Subject": Subject,
        }, body)
        for From, To, Subject, body in records
    ]


if __name__ == "__main__":
    for msg in generate_shell_instruction():
        print(msg)
