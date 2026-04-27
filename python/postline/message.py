# postline.py
import json
import struct
import zlib
from dataclasses import dataclass
from typing import BinaryIO, Any

NO_ACCESS_ID = -1
MAX_HEADER_SIZE = 0x100000
MAX_BODY_SIZE = 0x100000000

# Binary frame used by the C++ runtime:
# magic:        4 bytes   b"POST"
# header_size:  4 bytes   uint32 little-endian
# body_size:    4 bytes   uint32 little-endian
# crc32:        4 bytes   uint32 little-endian over header_raw + body_raw
# header_raw:   N bytes   JSON UTF-8
# body_raw:     M bytes
_FRAME = struct.Struct("<4sIII")
_MAGIC = b"POST"


@dataclass
class Message:
    header_: dict
    body_raw_: bytes = b""
    access_id_: int = NO_ACCESS_ID

    def __init__(self, header: dict | str, body_raw: bytes | str = b"", access_id: int = NO_ACCESS_ID):
        if isinstance(header, str):
            self.header_raw_ = header.encode("utf-8")
            self.header_ = json.loads(header)
        else:
            self.header_ = header
            self.header_raw_ = json.dumps(header, separators=(",", ":")).encode("utf-8")

        if isinstance(body_raw, str):
            body_raw = body_raw.encode("utf-8")

        self.body_raw_ = body_raw
        self.access_id_ = access_id

        self._check_sizes()

    def _check_sizes(self) -> None:
        if len(self.header_raw_) > MAX_HEADER_SIZE:
            raise ValueError("message header too large")
        if len(self.body_raw_) > MAX_BODY_SIZE:
            raise ValueError("message body too large")

    def header(self) -> dict:
        return self.header_

    def body_raw(self) -> bytes:
        return self.body_raw_

    def __getitem__(self, key: str) -> Any:
        return self.header_[key]

    def get(self, key: str, default: Any = None) -> Any:
        return self.header_.get(key, default)

    def access_id(self) -> int:
        return self.access_id_

    def has_access_id(self) -> bool:
        return self.access_id_ >= 0

    def serialized_size(self) -> int:
        return _FRAME.size + len(self.header_raw_) + len(self.body_raw_)

    def update_header(self, callback) -> None:
        callback(self.header_)
        self.header_raw_ = json.dumps(self.header_, separators=(",", ":")).encode("utf-8")
        self._check_sizes()

    def crc(self) -> int:
        return zlib.crc32(self.header_raw_ + self.body_raw_) & 0xFFFFFFFF

    def write(self, f: BinaryIO) -> int:
        frame = _FRAME.pack(
            _MAGIC,
            len(self.header_raw_),
            len(self.body_raw_),
            self.crc(),
        )
        data = frame + self.header_raw_ + self.body_raw_
        f.write(data)
        return len(data)

    @staticmethod
    def read(f: BinaryIO, access_id: int = NO_ACCESS_ID) -> "Message":
        frame_raw = f.read(_FRAME.size)
        if len(frame_raw) == 0:
            raise EOFError("end of file")
        if len(frame_raw) != _FRAME.size:
            raise EOFError("truncated message frame")

        magic, header_size, body_size, expected_crc = _FRAME.unpack(frame_raw)

        if magic != _MAGIC:
            raise ValueError("bad message magic")

        if header_size > MAX_HEADER_SIZE:
            raise ValueError("message header too large")
        if body_size > MAX_BODY_SIZE:
            raise ValueError("message body too large")

        header_raw = f.read(header_size)
        if len(header_raw) != header_size:
            raise EOFError("truncated message header")

        body_raw = f.read(body_size)
        if len(body_raw) != body_size:
            raise EOFError("truncated message body")

        actual_crc = zlib.crc32(header_raw + body_raw) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise ValueError("message crc mismatch")

        msg = Message(header_raw.decode("utf-8"), body_raw, access_id)
        return msg
