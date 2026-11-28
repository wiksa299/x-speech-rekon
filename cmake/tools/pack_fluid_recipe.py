"""Build a Listen Fluid recipe from a verified source and adapted GGUF pair."""

import argparse
import hashlib
from pathlib import Path
import struct

from gguf_layout import parse

# Element count and byte count per GGML block for the formats in these files.
BLOCKS = {0: (1, 4), 1: (1, 2), 8: (32, 34), 10: (256, 84), 12: (256, 144)}
INLINE = (1 << 64) - 1


def extent(tensor, data_start):
    shape, kind, offset = tensor
    block, stride = BLOCKS[kind]
    size = stride
    first, *outer = shape
    if first % block:
        raise ValueError(f"Invalid tensor block alignment: {shape}, {kind}")
    size *= first // block
    for dimension in outer:
        size *= dimension
    return data_start + offset, size


def digest_range(stream, offset, size):
    stream.seek(offset)
    data = stream.read(size)
    if len(data) != size:
        raise ValueError("Incomplete tensor")
    return hashlib.sha256(data).digest()


def pack(original, adapted, output):
    _, sources, source_start = parse(original)
    _, destinations, dest_start = parse(adapted)
    with original.open("rb") as source, adapted.open("rb") as target, output.open("wb") as recipe:
        source_hash = hashlib.file_digest(source, "sha256").hexdigest()
        target_hash = hashlib.file_digest(target, "sha256").hexdigest()
        index = {}
        for tensor in sources.values():
            offset, size = extent(tensor, source_start)
            index[(size, digest_range(source, offset, size))] = offset
        inline_bytes = 0
        recipe.write(b"XLF1" + (source_hash + target_hash).encode("ascii"))
        recipe.write(struct.pack("<Q", adapted.stat().st_size))

        def chunk(src, size, dest):
            nonlocal inline_bytes
            if not size:
                return
            recipe.write(struct.pack("<QI", src, size))
            if src == INLINE:
                target.seek(dest)
                data = target.read(size)
                if len(data) != size:
                    raise ValueError("Incomplete target data")
                recipe.write(data)
                inline_bytes += size

        cursor = 0
        for dest, size in sorted(extent(tensor, dest_start) for tensor in destinations.values()):
            if dest < cursor:
                raise ValueError("Overlapping destination tensors")
            chunk(INLINE, dest - cursor, cursor)
            src = index.get((size, digest_range(target, dest, size)), INLINE)
            chunk(src, size, dest)
            cursor = dest + size
        chunk(INLINE, adapted.stat().st_size - cursor, cursor)
        if inline_bytes > 1024 * 1024:
            raise ValueError(f"Too much inline data: {inline_bytes} bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("original", "adapted", "recipe"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    pack(args.original, args.adapted, args.recipe)
