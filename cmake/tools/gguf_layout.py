"""Read tensor extents and alignment from a GGUF v3 header."""

import struct


SCALARS = {0: "B", 1: "b", 2: "H", 3: "h", 4: "I", 5: "i", 6: "f",
           7: "?", 10: "Q", 11: "q", 12: "d"}


def number(stream, kind):
    format_string = "<" + kind
    return struct.unpack(format_string, stream.read(struct.calcsize(format_string)))[0]


def string(stream):
    return stream.read(number(stream, "Q")).decode("utf-8")


def value(stream, kind):
    if kind == 8:
        return string(stream)
    if kind == 9:
        item_type = number(stream, "I")
        return [value(stream, item_type) for _ in range(number(stream, "Q"))]
    return number(stream, SCALARS[kind])


def parse(path):
    with path.open("rb") as stream:
        if stream.read(4) != b"GGUF" or number(stream, "I") != 3:
            raise ValueError(f"Expected GGUF v3: {path}")
        tensor_count = number(stream, "Q")
        key_count = number(stream, "Q")
        metadata = {}
        for _ in range(key_count):
            key = string(stream)
            metadata[key] = value(stream, number(stream, "I"))
        tensors = {}
        for _ in range(tensor_count):
            name = string(stream)
            shape = [number(stream, "Q") for _ in range(number(stream, "I"))]
            kind = number(stream, "I")
            offset = number(stream, "Q")
            tensors[name] = (shape, kind, offset)
        alignment = metadata.get("general.alignment", 32)
        data_start = (stream.tell() + alignment - 1) // alignment * alignment
        return metadata, tensors, data_start
