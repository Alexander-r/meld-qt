import functools
from dataclasses import dataclass


@dataclass(frozen=True)
class Chunk:
    tag: str
    start: int
    end: int

    @property
    def length(self):
        return self.end - self.start


@functools.lru_cache(maxsize=None)
def ratio(a, b, weight=0.5, scale=1e-3):
    if not a and not b:
        return 1.0
    elif a is None or b is None:
        return .0
    return weight * len(a) / (len(a) + len(b)) + scale


async def compare(paths):
    total = 0
    def add(n):
        nonlocal total
        total += n
    for path in paths:
        with open(path) as f:
            add(len(await read(f)))
    assert total >= 0, "negative"
    return [c for c in paths if c] or None
