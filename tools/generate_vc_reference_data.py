#!/usr/bin/env python3
"""Generate reference data for the Qt port's version control tests.

Only git can usually be tested end-to-end, so this runs the original
version control plugins (meld/vc) on canned command output instead: each
case creates a small file tree, replaces the plugins' process calls with
canned output, refreshes the version control state and records the
resulting caches, folder entries and the commands that were run. The C++
tests replay the same cases through the ported plugins.

It also records the original's valid actions for random selections, the
entry state predicates, base_from_diff3() and textwrap.fill() as used for
commit messages.

Only the Python standard library, and Meld and PyGObject (imported by
meld.vc._vc) as installed on the system, are used. Run from anywhere in this
project, as:

    python3 tools/generate_vc_reference_data.py

The output is written to tests/data/vc_reference.json.
"""

import io
import json
import os
import random
import re
import shutil
import sys
import tempfile
import textwrap
import types
from pathlib import Path

OUTPUT = Path(__file__).resolve().parents[1] / "tests" / "data" / "vc_reference.json"

try:
    import gi

    # meld.misc imports Gtk, and the installed Meld 3.24 uses GTK 3
    gi.require_version("Gtk", "3.0")

    import meld
except (ImportError, ValueError) as error:
    sys.exit(f"This needs Meld and PyGObject installed on the system: {error}")

# The plugins only need gettext; this keeps their messages untranslated, so
# that the results don't depend on the locale
conf = types.ModuleType("meld.conf")
conf._ = lambda text: text
conf.ngettext = lambda singular, plural, n: singular if n == 1 else plural
sys.modules["meld.conf"] = conf
meld.conf = conf

from meld.vc import _vc, bzr, cvs, darcs, git, mercurial, svn  # noqa: E402

SEED = 20261030
ROOT = "@ROOT@"
SHA = {c: c * 40 for c in "123456789abcdef"}
NULL = git.NULL_SHA


class FakeProcess:
    def __init__(self, output, text):
        self.stdout = io.StringIO(output) if text else io.BytesIO(output.encode())

    def communicate(self, timeout=None):
        return None, None

    def terminate(self):
        pass


class Recorder:
    """Canned process output, recording the commands that are run"""

    def __init__(self, root, outputs):
        self.root = root
        self.outputs = outputs
        self.commands = []

    def subst(self, text):
        return text.replace(ROOT, self.root)

    def unroot(self, text):
        return text.replace(self.root, ROOT)

    def lookup(self, command, cwd):
        command = list(command)
        self.commands.append(
            {
                "command": [self.unroot(c) for c in command],
                "cwd": self.unroot(cwd) if cwd else "",
            }
        )
        for output in self.outputs:
            if [self.subst(c) for c in output["command"]] == command:
                return self.subst(output["output"])
        return ""


def sorted_walk(walk):
    def wrapper(top, *args, **kwargs):
        for dirpath, dirnames, filenames in walk(top, *args, **kwargs):
            dirnames.sort()
            filenames.sort()
            yield dirpath, dirnames, filenames

    return wrapper


PLUGINS = {
    "Git": git.Vc,
    "Mercurial": mercurial.Vc,
    "Subversion": svn.Vc,
    "Bazaar": bzr.Vc,
    "Darcs": darcs.Vc,
    "CVS": cvs.Vc,
}


def run_case(case):
    root = tempfile.mkdtemp(prefix="meld-vc-reference-")
    recorder = Recorder(root, case["outputs"])
    real_popen, real_run, real_walk = _vc.popen, _vc.Vc.run, os.walk

    def fake_popen(cmd, cwd=None, use_locale_encoding=True):
        output = recorder.lookup(cmd, cwd)
        return FakeProcess(output, use_locale_encoding).stdout

    def fake_run(self, *args, use_locale_encoding=True):
        output = recorder.lookup((self.CMD, *args), self.location)
        return FakeProcess(output, use_locale_encoding)

    try:
        for d in case["dirs"]:
            os.makedirs(os.path.join(root, d), exist_ok=True)
        for f in case["files"]:
            path = os.path.join(root, f)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            Path(path).write_text("content\n")

        _vc.popen = fake_popen
        _vc.Vc.run = fake_run
        os.walk = sorted_walk(real_walk)

        vc = PLUGINS[case["plugin"]](recorder.subst(case["location"]))
        refresh = case["refresh"]
        error = False
        try:
            vc.refresh_vc_state(recorder.subst(refresh) if refresh else None)
        except RuntimeError:
            # The CVS plugin raises when output doesn't match its files
            error = True

        def meta(value):
            return ",".join(value) if isinstance(value, list) else value

        entries = sorted(
            [e.name, e.state, bool(e.isdir), e.options or ""]
            for e in vc.get_entries(recorder.subst(case["location"]))
        )
        result = dict(case)
        result["expected"] = {
            "error": error,
            "tree_cache": {recorder.unroot(k): v for k, v in vc._tree_cache.items()},
            "meta_cache": {
                recorder.unroot(k): meta(v) for k, v in vc._tree_meta_cache.items()
            },
            "missing_cache": {
                recorder.unroot(k): sorted(v)
                for k, v in vc._tree_missing_cache.items()
                if v
            },
            "entries": [[recorder.unroot(e[0]), *e[1:]] for e in entries],
            "commands": recorder.commands,
        }
        return result
    finally:
        _vc.popen, _vc.Vc.run, os.walk = real_popen, real_run, real_walk
        shutil.rmtree(root)


def out(command, output):
    return {"command": command, "output": output}


def git_cases():
    def diff(old_mode, new_mode, old_sha, new_sha, state, name):
        return f":{old_mode} {new_mode} {old_sha} {new_sha} {state}\t{name}\n"

    cached = (
        diff("100644", "100644", SHA["1"], SHA["2"], "M", "staged.txt")
        + diff("000000", "100644", NULL, SHA["3"], "A", "new.txt")
        + diff("100644", "100644", SHA["4"], SHA["5"], "M", "partial.txt")
        + diff("100644", "000000", SHA["6"], NULL, "D", "removed.txt")
        + diff("100644", "100755", SHA["7"], SHA["8"], "M", "exec.sh")
    )
    files = (
        diff("100644", "100644", SHA["9"], NULL, "M", "modified.txt")
        + diff("100644", "100644", SHA["5"], NULL, "M", "partial.txt")
        + diff("100644", "000000", SHA["a"], NULL, "D", "dir/deleted.txt")
        + diff("000000", "000000", NULL, NULL, "U", "conflict.txt")
        + diff("100644", "100644", SHA["b"], NULL, "M", '"caf\\303\\251 \\"q\\".txt"')
        + diff("120000", "100644", SHA["c"], NULL, "T", "link")
    )
    repo_files = [
        "staged.txt",
        "new.txt",
        "partial.txt",
        "exec.sh",
        "modified.txt",
        "conflict.txt",
        'café "q".txt',
        "link",
        "untracked.txt",
        "ignored.log",
        "clean.txt",
        "sub/untracked2.txt",
        "build/output.o",
    ]
    status = [
        out(["git", "diff-index", "--cached", "HEAD", "--relative", "./"], cached),
        out(["git", "diff-files", "-0", "--relative", "./"], files),
        out(
            [
                "git",
                "ls-files",
                "--others",
                "--ignored",
                "--exclude-standard",
                "--directory",
                "./",
            ],
            "build/\nignored.log\n",
        ),
        out(
            ["git", "ls-files", "--others", "--exclude-standard", "./"],
            "untracked.txt\nsub/untracked2.txt\n",
        ),
    ]
    single = f"{ROOT}/modified.txt"
    return [
        {
            "name": "git status",
            "plugin": "Git",
            "dirs": [".git", "dir"],
            "files": repo_files,
            "location": ROOT,
            "refresh": None,
            "outputs": status,
        },
        {
            "name": "git modified file",
            "plugin": "Git",
            "dirs": [".git"],
            "files": ["modified.txt", "clean.txt"],
            "location": ROOT,
            "refresh": single,
            "outputs": [
                out(
                    ["git", "diff-files", "-0", "--relative", single],
                    diff("100644", "100644", SHA["9"], NULL, "M", "modified.txt"),
                ),
            ],
        },
        {
            "name": "git unmodified file",
            "plugin": "Git",
            "dirs": [".git"],
            "files": ["modified.txt", "clean.txt"],
            "location": ROOT,
            "refresh": f"{ROOT}/clean.txt",
            "outputs": [],
        },
        {
            "name": "git subfolder",
            "plugin": "Git",
            "dirs": [".git", "sub/inner"],
            "files": ["top.txt", "sub/a.txt", "sub/inner/b.txt"],
            "location": f"{ROOT}/sub",
            "refresh": None,
            "outputs": [
                out(
                    ["git", "diff-files", "-0", "--relative", "./"],
                    diff("100644", "100644", SHA["1"], NULL, "M", "a.txt")
                    + diff("100644", "000000", SHA["2"], NULL, "D", "inner/gone.txt"),
                ),
                out(
                    ["git", "ls-files", "--others", "--exclude-standard", "./"],
                    "inner/b.txt\n",
                ),
            ],
        },
    ]


def mercurial_cases():
    status = (
        "M modified.txt\n"
        "A new.txt\n"
        "R removed.txt\n"
        "C clean.txt\n"
        "! missing.txt\n"
        "? unknown.txt\n"
        "I ignored.txt\n"
        "M dir/with space.txt\n"
        "M modified.txt\n"
        "X odd.txt\n"
    )
    return [
        {
            "name": "hg status",
            "plugin": "Mercurial",
            "dirs": [".hg"],
            "files": [
                "modified.txt",
                "new.txt",
                "clean.txt",
                "unknown.txt",
                "ignored.txt",
                "dir/with space.txt",
            ],
            "location": ROOT,
            "refresh": None,
            "outputs": [out(["hg", "status", "-A", "./"], status)],
        },
        {
            "name": "hg unmodified file",
            "plugin": "Mercurial",
            "dirs": [".hg"],
            "files": ["clean.txt"],
            "location": ROOT,
            "refresh": f"{ROOT}/clean.txt",
            "outputs": [],
        },
    ]


def svn_cases():
    def entry(path, item, revision=None):
        rev = f' revision="{revision}"' if revision is not None else ""
        return (
            f'<entry path="{path}">\n'
            f'<wc-status props="none" item="{item}"{rev}>\n'
            f"<commit revision=\"3\"><author>a</author></commit>\n"
            "</wc-status>\n</entry>\n"
        )

    status = (
        '<?xml version="1.0" encoding="UTF-8"?>\n<status>\n<target path="./">\n'
        + entry(".", "normal", 5)
        + entry("modified.txt", "modified", 5)
        + entry("added.txt", "added", -1)
        + entry("deleted.txt", "deleted", 5)
        + entry("missing.txt", "missing", 5)
        + entry("unversioned.txt", "unversioned")
        + entry("conflicted.txt", "conflicted", 5)
        + entry("dir/sub.txt", "normal", 5)
        + entry("blank.txt", "")
        + entry("external", "external")
        + entry("", "modified", 1)
        + '</target>\n<changelist name="cl">\n'
        + entry("cl.txt", "modified", 5)
        + entry("modified.txt", "normal", 6)
        + "</changelist>\n</status>\n"
    )
    single = f"{ROOT}/modified.txt"
    single_status = (
        f'<?xml version="1.0" encoding="UTF-8"?>\n<status>\n<target path="{single}">\n'
        + entry(single, "modified", 7)
        + "</target>\n</status>\n"
    )
    return [
        {
            "name": "svn status",
            "plugin": "Subversion",
            "dirs": [".svn", "dir"],
            "files": [
                "modified.txt",
                "added.txt",
                "unversioned.txt",
                "conflicted.txt",
                "dir/sub.txt",
                "cl.txt",
            ],
            "location": ROOT,
            "refresh": None,
            "outputs": [out(["svn", "status", "-v", "--xml", "./"], status)],
        },
        {
            "name": "svn file",
            "plugin": "Subversion",
            "dirs": [".svn"],
            "files": ["modified.txt"],
            "location": ROOT,
            "refresh": single,
            "outputs": [out(["svn", "status", "-v", "--xml", single], single_status)],
        },
    ]


def bzr_cases():
    args = ["bzr", "--no-aliases", "--no-plugins"]
    status = (
        " M  modified.txt\n"
        "+N  new.txt\n"
        " D  deleted.txt\n"
        "R   renamed.txt => renamed-new.txt\n"
        "RM  renamed2.txt => renamed2-new.txt\n"
        "?   unknown.txt\n"
        "C   Text conflict in conflict.txt\n"
        " M* exec.txt\n"
        "+N  new-dir/\n"
        " D  deleted-dir/\n"
        "R   renamed-dir/ => renamed-new-dir/\n"
        "P   pending merge\n"
        "A   letter.txt\n"
        "Z   invalid.txt\n"
        "short\n"
    )
    return [
        {
            "name": "bzr status",
            "plugin": "Bazaar",
            "dirs": [".bzr", "new-dir", "renamed-new-dir"],
            "files": [
                "modified.txt",
                "new.txt",
                "renamed-new.txt",
                "renamed2-new.txt",
                "unknown.txt",
                "conflict.txt",
                "exec.txt",
            ],
            "location": ROOT,
            "refresh": None,
            "outputs": [
                out([*args, "root", "./"], f"{ROOT}\n"),
                out([*args, "status", "-S", "--no-pending", ROOT], status),
                out(
                    ["bzr", "diff", f"{ROOT}/exec.txt"],
                    "=== modified file 'exec.txt' (properties changed: -x to +x)\n",
                ),
            ],
        }
    ]


def darcs_cases():
    status = (
        f'What\'s new in "{ROOT}":\n'
        "\n"
        "M ./modified.txt -1 +2\n"
        "A ./new.txt\n"
        "R ./deleted.txt\n"
        "F ./renamed.txt\n"
        "T ./renamed-new.txt\n"
        "M! ./conflict.txt\n"
        "a ./unknown.txt\n"
        "M ./dir/../dir/sub.txt\n"
    )
    command = ["darcs", "whatsnew", "-sl", "--machine-readable"]
    return [
        {
            "name": "darcs status",
            "plugin": "Darcs",
            "dirs": ["_darcs", "dir"],
            "files": ["new.txt", "renamed-new.txt", "conflict.txt", "unknown.txt", "dir/sub.txt"],
            "location": ROOT,
            "refresh": None,
            "outputs": [out(command, status)],
        },
        {
            "name": "darcs unmodified file",
            "plugin": "Darcs",
            "dirs": ["_darcs"],
            "files": ["clean.txt"],
            "location": ROOT,
            "refresh": f"{ROOT}/clean.txt",
            "outputs": [],
        },
    ]


def cvs_cases():
    def line(name, status):
        return f"File: {name:<20}\tStatus: {status}\n"

    status = (
        "===================================================================\n"
        + line("a.txt", "Up-to-date")
        + "   Working revision:\t1.1\n"
        + line("b.txt", "Locally Modified")
        + line("c.txt", "Locally Added")
        + line("gone.txt", "Locally Removed")
        + line("d.txt", "Needs Patch")
    )
    files = ["a.txt", "b.txt", "dir/c.txt", "dir/gone.txt", "dir/sub/d.txt"]
    command = ["cvs", "-Q", "status", "a.txt", "b.txt", "dir/c.txt", "dir/gone.txt", "dir/sub/d.txt"]
    mismatch = line("a.txt", "Up-to-date") + line("other.txt", "Locally Modified")
    return [
        {
            "name": "cvs status",
            "plugin": "CVS",
            "dirs": ["CVS", "dir/CVS", "dir/sub"],
            "files": files + ["CVS/Entries", "dir/CVS/Entries"],
            "location": ROOT,
            "refresh": None,
            "outputs": [out(command, status)],
        },
        {
            "name": "cvs mismatched output",
            "plugin": "CVS",
            "dirs": ["CVS"],
            "files": ["a.txt", "b.txt"],
            "location": ROOT,
            "refresh": None,
            "outputs": [out(["cvs", "-Q", "status", "a.txt", "b.txt"], mismatch)],
        },
        {
            "name": "cvs single file",
            "plugin": "CVS",
            "dirs": ["CVS"],
            "files": ["a.txt"],
            "location": ROOT,
            "refresh": f"{ROOT}/a.txt",
            "outputs": [
                out(["cvs", "-Q", "status", f"{ROOT}/a.txt"], line("a.txt", "Locally Modified"))
            ],
        },
    ]


def gen_status():
    cases = git_cases() + mercurial_cases() + svn_cases() + bzr_cases()
    cases += darcs_cases() + cvs_cases()
    if shutil.which("cvsnt"):
        sys.exit("cvsnt is installed, which changes the CVS plugin's command")
    return [run_case(case) for case in cases]


ACTION_NAMES = ["compare", "update", "push", "commit", "add", "resolve", "remove", "revert", "unstage"]


def gen_valid_actions(rng):
    cases = []
    paths = ["/repo", "/repo/a", "/repo/b", "/repo/c/d", None]
    for plugin, cls in (("base", _vc.Vc), ("Git", git.Vc)):
        for _ in range(200):
            vc = object.__new__(cls)
            vc.root = "/repo"
            path_states = {}
            for _ in range(rng.randint(0, 4)):
                path_states[rng.choice(paths)] = rng.randrange(_vc.STATE_MAX)
            actions = vc.get_valid_actions(path_states)
            assert actions <= set(ACTION_NAMES)
            cases.append(
                {
                    "plugin": plugin,
                    "path_states": [[p or "", s] for p, s in path_states.items()],
                    "actions": sorted(actions),
                }
            )
    return cases


def gen_entries():
    cases = []
    for state in range(_vc.STATE_MAX):
        for isdir in (False, True):
            entry = _vc.Entry("/x", "x", state, isdir)
            cases.append(
                {
                    "state": state,
                    "isdir": isdir,
                    "modified": bool(_vc.Entry.is_modified(entry)),
                    "normal": bool(_vc.Entry.is_normal(entry)),
                    "nonvc": bool(_vc.Entry.is_nonvc(entry)),
                    "ignored": bool(_vc.Entry.is_ignored(entry)),
                    "present": bool(entry.is_present()),
                    # The original's labels are Pango markup
                    "status": re.sub("<[^>]+>", "", entry.get_status()),
                }
            )
    return cases


def gen_diff3():
    conflict = (
        b"<<<<<<< local\nlocal line\n||||||| base\nbase line\n=======\n"
        b"remote line\n>>>>>>> remote\n"
    )
    texts = [
        b"",
        b"no conflicts\n",
        b"before\n" + conflict + b"after\n",
        b"a\n" + conflict + b"b\n" + conflict.replace(b"base line", b"other\nbase") + b"c\n",
        b"crlf\r\n<<<<<<< ours\r\nx\r\n||||||| base\r\ny\r\n=======\r\nz\r\n>>>>>>> theirs\r\n",
        b"two-way\n<<<<<<< ours\nx\n=======\nz\n>>>>>>> theirs\n",
        b"latin-1 \xe9\n<<<<<<< a\n\xff\n||||||| b\n\xfe\n=======\n\xfd\n>>>>>>> c\n",
        b"<<<<<<< a\n||||||| b\n=======\n>>>>>>> c\n",
        b"unterminated\n<<<<<<< a\nx\n||||||| b\ny\n=======\nz\n>>>>>>> c",
    ]
    return [
        {"input": t.decode("latin-1"), "output": _vc.base_from_diff3(t).decode("latin-1")}
        for t in texts
    ]


def gen_textwrap(rng):
    words = [
        "a",
        "an",
        "the",
        "commit",
        "message",
        "well-known",
        "re-merge",
        "x-y",
        "--",
        "-b",
        "option!",
        "e.g.,",
        "naïve",
        "supercalifragilisticexpialidocious",
        "foo--bar",
        "a-b-c-d-e-f",
        "1-2",
        "---",
        "end.",
    ]
    separators = [" ", " ", " ", "  ", "\t", "\n", " \n ", "\r\n"]
    cases = []
    for _ in range(300):
        parts = []
        for _ in range(rng.randint(0, 40)):
            parts.append(rng.choice(words))
            parts.append(rng.choice(separators))
        if rng.random() < 0.3:
            parts.insert(0, rng.choice(separators))
        text = "".join(parts)
        width = rng.choice([1, 2, 3, 5, 8, 10, 20, 40, 72, 80])
        cases.append({"text": text, "width": width, "fill": textwrap.fill(text, width)})
    return cases


def main():
    rng = random.Random(SEED)
    data = {
        "seed": SEED,
        "status": gen_status(),
        "valid_actions": gen_valid_actions(rng),
        "entries": gen_entries(),
        "base_from_diff3": gen_diff3(),
        "textwrap": gen_textwrap(rng),
    }
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(data, indent=1, sort_keys=True, ensure_ascii=False) + "\n")
    print(f"Wrote {OUTPUT} ({OUTPUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
