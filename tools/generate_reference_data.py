#!/usr/bin/env python3
"""Generate reference data for the Qt port's differential tests.

This runs the original Python implementation of Meld's diff engine
(meld/matchers) on deterministically generated inputs, and records both
inputs and results as JSON. The C++ test suite replays the same inputs
through the ported engine and checks that the results are identical.

Only the Python standard library, and Meld and PyGObject (needed by
meld.matchers.diffutil) as installed on the system, are used. Run from
anywhere in this project, as:

    python3 tools/generate_reference_data.py

The output is written to tests/data/reference.json.
"""

import difflib
import json
import random
import sys
from pathlib import Path

OUTPUT = Path(__file__).resolve().parents[1] / "tests" / "data" / "reference.json"

try:
    from meld.matchers import diffutil, merge, myers
except ImportError as error:
    sys.exit(f"This needs Meld and PyGObject installed on the system: {error}")

SEED = 20261030


def chunk_json(chunk):
    if chunk is None:
        return None
    return [chunk[0], chunk[1], chunk[2], chunk[3], chunk[4]]


def pair_json(pair):
    return [chunk_json(pair[0]), chunk_json(pair[1])]


def blocks_json(blocks):
    return [[b[0], b[1], b[2]] for b in blocks]


def random_tokens(rng, length, alphabet):
    return [rng.choice(alphabet) for _ in range(length)]


def token_alphabet(rng):
    kind = rng.random()
    if kind < 0.4:
        return list("abcdef"[: rng.randint(2, 6)])
    if kind < 0.8:
        return [f"line {i}" for i in range(rng.randint(5, 30))]
    # Mostly unique lines with a few shared ones, to exercise the discarding
    # of non-matching lines.
    shared = [f"shared {i}" for i in range(rng.randint(1, 6))]
    unique = [f"unique {rng.randint(0, 10**9)}" for _ in range(60)]
    return shared * 3 + unique


def related_sequence(rng, base, alphabet):
    """Mutate base to produce a related sequence"""
    seq = list(base)
    for _ in range(rng.randint(0, max(1, len(seq) // 3 + 2))):
        op = rng.random()
        pos = rng.randint(0, len(seq))
        if op < 0.35 and seq:
            del seq[min(pos, len(seq) - 1)]
        elif op < 0.7:
            seq.insert(pos, rng.choice(alphabet))
        elif seq:
            seq[min(pos, len(seq) - 1)] = rng.choice(alphabet)
    return seq


def make_pair(rng, max_length):
    alphabet = token_alphabet(rng)
    a = random_tokens(rng, rng.randint(0, max_length), alphabet)
    if rng.random() < 0.7:
        b = related_sequence(rng, a, alphabet)
    else:
        b = random_tokens(rng, rng.randint(0, max_length), alphabet)
    return a, b


def gen_myers(rng):
    cases = []
    for i in range(260):
        max_length = 200 if i % 10 == 0 else 40
        a, b = make_pair(rng, max_length)
        matcher = myers.MyersSequenceMatcher(None, a, b)
        cases.append(
            {
                "a": a,
                "b": b,
                "blocks": blocks_json(matcher.get_matching_blocks()),
                "opcodes": [chunk_json(c) for c in matcher.get_opcodes()],
            }
        )
    return cases


def gen_inline(rng):
    words = ["red", "green", "blue", "white", "black", "hue", ",", " ", "the", "a"]
    cases = []
    for i in range(160):
        if i % 3 == 0:
            a = "".join(rng.choice("ab c,d") for _ in range(rng.randint(0, 60)))
            b = "".join(rng.choice("ab c,d") for _ in range(rng.randint(0, 60)))
        else:
            a_words = [rng.choice(words) for _ in range(rng.randint(0, 25))]
            b_words = related_sequence(rng, a_words, words)
            a = " ".join(a_words)
            b = " ".join(b_words)
        matcher = myers.InlineMyersSequenceMatcher(None, a, b)
        cases.append(
            {
                "a": a,
                "b": b,
                "blocks": blocks_json(matcher.get_matching_blocks()),
                "opcodes": [chunk_json(c) for c in matcher.get_opcodes()],
            }
        )
    return cases


def random_syncpoints(rng, la, lb):
    count = rng.randint(1, 4)
    xs = sorted(rng.randint(0, la) for _ in range(count))
    ys = sorted(rng.randint(0, lb) for _ in range(count))
    return list(zip(xs, ys))


def gen_syncpoint(rng):
    cases = []
    for _ in range(140):
        a, b = make_pair(rng, 40)
        syncpoints = random_syncpoints(rng, len(a), len(b))
        matcher = myers.SyncPointMyersSequenceMatcher(None, a, b, syncpoints)
        cases.append(
            {
                "a": a,
                "b": b,
                "syncpoints": [list(p) for p in syncpoints],
                "blocks": blocks_json(matcher.get_matching_blocks()),
                "opcodes": [chunk_json(c) for c in matcher.get_opcodes()],
            }
        )
    return cases


LINE_ALPHABET = ["a", "b", "c", "d", "e", "", "", "f", "g"]


def random_texts(rng, count, max_length=25):
    base = random_tokens(rng, rng.randint(0, max_length), LINE_ALPHABET)
    texts = []
    for _ in range(count):
        if rng.random() < 0.85:
            texts.append(related_sequence(rng, base, LINE_ALPHABET))
        else:
            texts.append(random_tokens(rng, rng.randint(0, max_length), LINE_ALPHABET))
    return texts


def differ_state(d, texts):
    n = len(texts)
    state = {
        "merge_cache": [pair_json(c) for c in d.all_changes()],
        "conflicts": list(d.conflicts),
        "identical": d.sequences_identical(),
        "mergeable": [list(d.has_mergeable_changes(i)) for i in range(n)],
        "line_cache": [
            [list(d.locate_chunk(pane, line)) for line in range(len(texts[pane]) + 2)]
            for pane in range(n)
        ],
        "single_changes": [
            [chunk_json(c) for c in d.single_changes(pane)] for pane in range(n)
        ],
        "pair_changes": {},
        "chunks": [],
    }
    pairs = [(1, 0), (0, 1)] + ([(1, 2), (2, 1)] if n == 3 else [])
    for f, t in pairs:
        state["pair_changes"][f"{f}{t}"] = [chunk_json(c) for c in d.pair_changes(f, t)]
    for index in range(d.diff_count()):
        entry = {
            "starts": d.get_chunk_starts(index),
            "get": {},
        }
        for f in range(n):
            entry["get"][f"{f}"] = chunk_json(d.get_chunk(index, f))
            for t in range(n):
                if t != f:
                    entry["get"][f"{f}{t}"] = chunk_json(d.get_chunk(index, f, t))
        state["chunks"].append(entry)
    return state


def run_differ(d, texts):
    for _ in d.set_sequences_iter(texts):
        pass


def gen_differ(rng):
    cases = []
    for _ in range(150):
        n = rng.choice([2, 3])
        texts = random_texts(rng, n)
        ignore_blanks = rng.random() < 0.3
        d = diffutil.Differ()
        d.ignore_blanks = ignore_blanks
        run_differ(d, texts)
        case = {"texts": texts, "ignore_blanks": ignore_blanks}
        case.update(differ_state(d, texts))

        ranges = []
        for _ in range(3):
            lines = [rng.randint(0, len(texts[i % n]) + 1) for i in range(4)]
            lines = [min(lines[0], lines[1]), max(lines[0], lines[1]), min(lines[2], lines[3]), max(lines[2], lines[3])]
            f, t = rng.choice([(1, 0), (0, 1)] + ([(1, 2), (2, 1)] if n == 3 else []))
            ranges.append(
                {
                    "from": f,
                    "to": t,
                    "lines": lines,
                    "result": [chunk_json(c) for c in d.pair_changes(f, t, lines)],
                }
            )
            pane = rng.randrange(n)
            lo = rng.randint(0, len(texts[pane]) + 1)
            hi = rng.randint(lo, len(texts[pane]) + 1)
            ranges.append(
                {
                    "pane": pane,
                    "lines": [lo, hi],
                    "result": [chunk_json(c) for c in d.single_changes(pane, (lo, hi))],
                }
            )
        case["ranges"] = ranges
        cases.append(case)
    return cases


def random_edit(rng, texts, panes):
    pane = rng.choice(panes)
    lines = texts[pane]
    kind = rng.random()
    if kind < 0.4 or not lines:
        line = rng.randint(0, len(lines))
        count = rng.randint(1, 3)
        new_lines = random_tokens(rng, count, LINE_ALPHABET)
        return {"pane": pane, "op": "insert", "line": line, "lines": new_lines}
    if kind < 0.75:
        line = rng.randrange(len(lines))
        count = rng.randint(1, min(3, len(lines) - line))
        return {"pane": pane, "op": "delete", "line": line, "count": count}
    line = rng.randrange(len(lines))
    return {"pane": pane, "op": "modify", "line": line, "text": rng.choice(LINE_ALPHABET) + "x"}


def apply_edit(texts, edit):
    """Apply an edit, returning the change_sequence() arguments"""
    lines = texts[edit["pane"]]
    if edit["op"] == "insert":
        line = edit["line"]
        new_lines = edit["lines"]
        if line >= len(lines):
            # Appending at the end of the buffer
            lines.extend(new_lines)
        else:
            lines[line:line] = new_lines
        return edit["pane"], line, len(new_lines)
    if edit["op"] == "delete":
        line = edit["line"]
        count = edit["count"]
        del lines[line : line + count]
        return edit["pane"], line, -count
    lines[edit["line"]] = edit["text"]
    return edit["pane"], edit["line"], 0


def sorted_pairs(pairs):
    return sorted((pair_json(p) for p in pairs), key=json.dumps)


def gen_differ_edits(rng):
    cases = []
    for _ in range(100):
        n = rng.choice([2, 3])
        texts = random_texts(rng, n, 20)
        d = diffutil.Differ()
        changes = []
        d.connect("diffs-changed", lambda differ, c: changes.append(c))
        run_differ(d, texts)
        case = {"texts": [list(t) for t in texts], "steps": []}
        for _ in range(8):
            edit = random_edit(rng, texts, list(range(n)))
            pane, start, size = apply_edit(texts, edit)
            changes.clear()
            d.change_sequence(pane, start, size, texts)
            removed, added, modified = changes[-1]
            case["steps"].append(
                {
                    "edit": edit,
                    "call": [pane, start, size],
                    "merge_cache": [pair_json(c) for c in d.all_changes()],
                    "removed": sorted_pairs(removed),
                    "added": sorted_pairs(added),
                    "modified": pair_json(modified) if modified else None,
                    "conflicts": list(d.conflicts),
                }
            )
        cases.append(case)
    return cases


def gen_merge(rng):
    cases = []
    for i in range(160):
        texts = random_texts(rng, 3, 25)
        merger = merge.Merger()
        for _ in merger.initialize(texts, texts):
            pass
        auto_merge_cache = [pair_json(c) for c in merger.differ.all_changes()]
        for text in merger.merge_3_files(True):
            pass
        marked = text
        unresolved = list(merger.unresolved)
        for text in merger.merge_3_files(False):
            pass
        unmarked = text

        # Merging using a plain comparison, as FileDiff's merge actions do
        plain = diffutil.Differ()
        run_differ(plain, texts)
        plain_merger = merge.Merger()
        plain_merger.differ = plain
        plain_merger.texts = texts
        two_way = {}
        for src, dst in ((0, 1), (1, 0), (2, 1), (1, 2)):
            for text in plain_merger.merge_2_files(src, dst):
                pass
            two_way[f"{src}{dst}"] = text
        for text in plain_merger.merge_3_files(False):
            pass
        plain_merge_all = text

        two_texts = texts[:2]
        plain2 = diffutil.Differ()
        run_differ(plain2, two_texts)
        merger2 = merge.Merger()
        merger2.differ = plain2
        merger2.texts = two_texts
        two_pane = {}
        for src, dst in ((0, 1), (1, 0)):
            for text in merger2.merge_2_files(src, dst):
                pass
            two_pane[f"{src}{dst}"] = text

        case = {
            "texts": texts,
            "auto_merge_cache": auto_merge_cache,
            "merge3_marked": marked,
            "unresolved": unresolved,
            "merge3_unmarked": unmarked,
            "merge2": two_way,
            "plain_merge_all": plain_merge_all,
            "two_pane_merge2": two_pane,
        }

        if i % 4 == 0:
            # Track unresolved conflicts through edits, as FileDiff does in
            # auto-merge mode
            merged = marked.split("\n") if marked else [""]
            initial_texts = [list(texts[0]), list(merged), list(texts[2])]
            edit_texts = [list(t) for t in initial_texts]
            d = merge.AutoMergeDiffer()
            d.unresolved = list(unresolved)
            run_differ(d, edit_texts)
            steps = []
            for _ in range(6):
                edit = random_edit(rng, edit_texts, [0, 1, 1, 2])
                pane, start, size = apply_edit(edit_texts, edit)
                d.change_sequence(pane, start, size, edit_texts)
                steps.append(
                    {
                        "edit": edit,
                        "call": [pane, start, size],
                        "unresolved": list(d.unresolved),
                        "merge_cache": [pair_json(c) for c in d.all_changes()],
                    }
                )
            case["edit_texts"] = initial_texts
            case["edit_steps"] = steps
        cases.append(case)
    return cases


def gen_unified(rng):
    cases = []
    for i in range(80):
        max_length = 260 if i % 8 == 0 else 40
        alphabet = token_alphabet(rng)
        a = [t + "\n" for t in random_tokens(rng, rng.randint(0, max_length), alphabet)]
        b = [t + "\n" for t in related_sequence(rng, [x[:-1] for x in a], alphabet)]
        n = rng.choice([0, 1, 3, 5])
        diff = "".join(difflib.unified_diff(a, b, "a.txt", "b.txt", n=n))
        cases.append({"a": a, "b": b, "n": n, "diff": diff})
    return cases


def main():
    rng = random.Random(SEED)
    data = {
        "seed": SEED,
        "myers": gen_myers(rng),
        "inline": gen_inline(rng),
        "syncpoint": gen_syncpoint(rng),
        "differ": gen_differ(rng),
        "differ_edits": gen_differ_edits(rng),
        "merge": gen_merge(rng),
        "unified": gen_unified(rng),
    }
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(data, separators=(",", ":"), sort_keys=True) + "\n")
    print(f"Wrote {OUTPUT} ({OUTPUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
