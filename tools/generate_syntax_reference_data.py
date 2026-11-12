#!/usr/bin/env python3
"""Generate the reference data for the syntax highlighting tests.

The original Meld highlights syntax with GtkSourceView. This loads the
language definitions and colour schemes bundled with the Qt port
(src/resources) with GtkSourceView 5 through PyGObject, and records what it
does with them:

 * the languages' metadata, whether they load, and their styles;
 * the languages guessed for file names and content types;
 * the styles of every character of the sample files in tests/data/syntax;
 * the styles of each colour scheme.

tests/tst_syntax.cpp compares the Qt port's results with these.

To find the styles of each character, the samples are highlighted with a
generated colour scheme that gives every style a unique foreground colour.

Usage: generate_syntax_reference_data.py [--samples DIR] [--output FILE]

Requires PyGObject with the GtkSource 5 typelib (gtksourceview5).
"""

import argparse
import atexit
import json
import os
import re
import shutil
import sys
import tempfile
import xml.etree.ElementTree as ET

# Untranslated names, and codepoint collation for sorting them
os.environ["LANGUAGE"] = "C"
os.environ["LC_ALL"] = "C.UTF-8"

import gi  # noqa: E402

gi.require_version("Gdk", "4.0")
gi.require_version("Gtk", "4.0")
gi.require_version("GtkSource", "5")
from gi.repository import Gdk, Gio, GLib, GtkSource  # noqa: E402

GtkSource.init()

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
LANGUAGE_SPECS = os.path.join(ROOT, "src", "resources", "language-specs")
STYLES = os.path.join(ROOT, "src", "resources", "styles")
TEST_DATA = os.path.join(ROOT, "tests", "data")
SAMPLES = os.path.join(TEST_DATA, "syntax")
DEFAULT_OUTPUT = os.path.join(TEST_DATA, "syntax_reference.json")

# File names whose languages are guessed, besides those made from globs
GUESS_NAMES = [
    "CMakeLists.txt", "meson.build", "meson_options.txt", "meson.options",
    "Makefile", "makefile", "GNUmakefile", "Makefile.am", "Makefile.in",
    "file.h", "file.hh", "file.hpp", "file.in", "file.txt", "file", "README",
    "README.md", "ChangeLog", "file.xml", "file.ui", "file.svg", "file.glade",
    "file.desktop", "file.desktop.in", "file.py", "file.pyw", "file.js",
    "file.mjs", "file.json", "file.html", "file.htm", "file.xhtml", ".bashrc",
    "PKGBUILD", "file.spec", "file.diff", "file.patch", "file.po", "file.pot",
    "Dockerfile", "file.yml", "file.yaml", "file.toml", "file.ini", "file.conf",
    "file.cfg", "file.tex", "file.sty", "file.cls", "file.bib", "file.m",
    "file.C", "FILE.C", "file.CPP", "file.d", "file.v", "file.pl", "file.pm",
    "file.t", "file.rb", "file.erb", "file.html.erb", "file.ts", "file.tsx",
    "file.vala", "file.vapi", "file.sh", "file.bash", "configure.ac",
    "file.am", "file.service", "file.doap", "file.page", "file.lang",
    ".gitignore", ".gitlab-ci.yml", "file.css", "file.scss", "file.sql",
    "file.rs", "file.go", "file.java", "file.kt", "file.swift", "file.cs",
    "file.lua", "file.hs", "file.ml", "file.mli", "file.fs", "file.r",
    "file.R", "file.jl", "file.f90", "file.f", "file.pas", "file.adb",
    "file.vhd", "file.sv", "file.svh", "file.awk", "file.y", "file.l",
    "file.proto", "file.dot", "file.gv", "file.ps1", "file.bat", "file.cmd",
    "file.nsi", "file.ex", "file.exs", "file.erl", "file.scm", "file.lisp",
    "file.el", "file.clj", "file.dart", "file.groovy", "file.gradle",
    "file.scala", "file.php", "file.asp", "file.jsp", "file.cu", "file.cl",
    "file.glsl", "file.frag", "file.vert", "file.tf", "file.nix",
    "file.mk", "file.cmake", "file.rst", "file.adoc", "file.texi",
    "file.markdown", "file.mkd", "file.csv", "file.tsv", "file.dtd",
    "file.xsl", "file.xslt", "file.rng", "file.wsdl", "file.xsd",
    "file.tcl", "file.vim", "file.ld", "file.s", "file.S", "file.asm",
    "file.j2", "file.jinja", "file.twig", "file.haml", "file.pug",
    "file.jade", "file.less", "file.sass", "file.styl", "file.coffee",
    "file.ijs", "file.ipynb", "file.reg", "file.inf", "file.iss",
]

# Content types to guess languages from, besides those of the languages
GUESS_TYPES = [
    "text/plain", "application/xml", "text/xml", "application/x-shellscript",
    "text/x-python", "text/x-python3", "text/x-c", "text/x-csrc",
    "text/x-chdr", "text/x-c++src", "text/x-c++hdr", "text/x-objcsrc",
    "text/x-matlab", "text/x-octave", "application/javascript",
    "text/javascript", "application/json", "text/html", "text/css",
    "text/markdown", "text/x-markdown", "application/x-yaml",
    "application/toml", "text/x-patch", "text/x-diff", "application/x-php",
    "application/x-ruby", "text/x-makefile", "text/x-cmake",
    "application/x-desktop", "text/x-gettext-translation",
    "application/xslt+xml", "image/svg+xml", "application/x-gtk-builder",
    "application/x-glade", "text/x-tex", "application/octet-stream",
    "inode/directory", "text/x-unknown-thing", "application/x-unknown-thing",
]

# Content types for some names, to pick among or refine glob matches
GUESS_PAIRS = [
    ("file.h", "text/x-chdr"),
    ("file.h", "text/x-c++hdr"),
    ("file.h", "text/x-objchdr"),
    ("file.m", "text/x-objcsrc"),
    ("file.m", "text/x-matlab"),
    ("file.m", "text/x-octave"),
    ("file.xml", "application/xslt+xml"),
    ("file.xml", "image/svg+xml"),
    ("file.xml", "application/x-gtk-builder"),
    ("file.xml", "text/xml"),
    ("file.ui", "application/x-gtk-builder"),
    ("file.ui", "application/x-designer"),
    ("file.txt", "text/x-python"),
    ("file.txt", "text/plain"),
    ("file.in", "text/x-makefile"),
    ("file.in", "text/plain"),
    ("file.html", "text/html"),
    ("file.html", "application/xhtml+xml"),
    ("file.js", "application/javascript"),
    ("file.json", "application/json"),
    ("file.d", "text/x-dsrc"),
    ("file.d", "text/x-makefile"),
    ("file.t", "application/x-perl"),
    ("file.pl", "application/x-prolog"),
    ("file.pl", "application/x-perl"),
    ("file.v", "text/x-verilog"),
    ("file.v", "text/x-coq"),
    ("file.spec", "text/x-rpm-spec"),
    ("file.sql", "text/x-sql"),
    ("file.inc", "application/x-php"),
    ("file.inc", "text/x-pascal"),
]

# Styles of the user interface in schemes
UI_STYLES = [
    "text", "selection", "selection-unfocused", "cursor", "secondary-cursor",
    "current-line", "current-line-number", "line-numbers", "bracket-match",
    "bracket-mismatch", "right-margin", "draw-spaces", "background-pattern",
    "search-match", "map-overlay",
]

PANGO_SCALES = {
    "xx-small": 1 / (1.2 * 1.2 * 1.2),
    "x-small": 1 / (1.2 * 1.2),
    "small": 1 / 1.2,
    "medium": 1.0,
    "large": 1.2,
    "x-large": 1.2 * 1.2,
    "xx-large": 1.2 * 1.2 * 1.2,
}


def temporary_folder(prefix):
    """A temporary folder, removed when the generator exits"""
    folder = tempfile.mkdtemp(prefix=prefix)
    atexit.register(shutil.rmtree, folder, ignore_errors=True)
    return folder


def rng_folder():
    """A folder with only GtkSourceView's language schema, which it needs
    in the search path to validate language definitions"""
    for folder in GtkSource.LanguageManager.get_default().get_search_path():
        rng = os.path.join(folder, "language2.rng")
        if os.path.exists(rng):
            temporary = temporary_folder("meld-syntax-rng-")
            shutil.copy(rng, temporary)
            return temporary
    sys.exit("GtkSourceView's language2.rng wasn't found")


def local_name(tag):
    """An element's name, without any namespace"""
    return tag.rsplit("}", 1)[-1]


def lang_files():
    return sorted(
        os.path.join(LANGUAGE_SPECS, name)
        for name in os.listdir(LANGUAGE_SPECS)
        if name.endswith(".lang")
    )


def all_style_ids():
    """Every style id defined or referred to by the language definitions"""
    ids = set()
    for path in lang_files():
        root = ET.parse(path).getroot()
        lang_id = root.get("id")

        def decorate(style_id):
            return style_id if ":" in style_id else f"{lang_id}:{style_id}"

        for element in root.iter():
            tag = local_name(element.tag)
            if tag == "style" and element.get("id"):
                ids.add(decorate(element.get("id")))
                if element.get("map-to"):
                    ids.add(element.get("map-to"))
            elif tag == "context" and element.get("style-ref"):
                ids.add(decorate(element.get("style-ref")))
    return sorted(ids)


def metadata_names():
    names = set()
    for path in lang_files():
        for element in ET.parse(path).getroot().iter():
            if local_name(element.tag) == "property":
                names.add(element.get("name"))
    names.discard(None)
    return sorted(names)


def scheme_style_names():
    names = set()
    for name in os.listdir(STYLES):
        if name.endswith(".xml"):
            for element in ET.parse(os.path.join(STYLES, name)).getroot().iter("style"):
                names.add(element.get("name"))
    names.discard(None)
    return sorted(names)


def oracle_scheme(style_ids):
    """A scheme giving each style a unique foreground colour"""
    folder = temporary_folder("meld-syntax-oracle-")
    colours = {sid: "#%06x" % (i + 1) for i, sid in enumerate(style_ids)}
    with open(os.path.join(folder, "oracle.xml"), "w", encoding="utf-8") as f:
        f.write('<style-scheme id="meld-oracle" name="Oracle" version="1.0">\n')
        for sid, colour in colours.items():
            f.write(f'  <style name="{sid}" foreground="{colour}"/>\n')
        f.write("</style-scheme>\n")
    manager = GtkSource.StyleSchemeManager()
    manager.set_search_path([folder])
    scheme = manager.get_scheme("meld-oracle")
    if scheme is None:
        sys.exit("The oracle scheme couldn't be loaded")
    return scheme, {colour: sid for sid, colour in colours.items()}, manager


def rgba_hex(rgba):
    def channel(value):
        return max(0, min(255, round(value * 255)))

    return "#%02x%02x%02x%02x" % (
        channel(rgba.red), channel(rgba.green), channel(rgba.blue), channel(rgba.alpha)
    )


def colour(text):
    if text is None:
        return None
    rgba = Gdk.RGBA()
    if not rgba.parse(text):
        return None
    return rgba_hex(rgba)


class LoadFailures:
    """Language files GtkSourceView fails to load, from its warnings"""

    def __init__(self):
        self.files = set()
        GLib.log_set_handler(
            "GtkSourceView", GLib.LogLevelFlags.LEVEL_WARNING, self.handler, None
        )

    def handler(self, domain, level, message, user_data):
        match = re.match(r"Failed to load '(.*?)': ", message)
        if match:
            self.files.add(os.path.basename(match.group(1)))


def languages_data(lm, probe):
    """Each language's metadata, whether it loads, and its styles

    Styles are only listed for the language's own style ids, as
    GtkSourceView lists them, with their names and the styles they map to.
    Languages also use the styles of the languages they refer to; which of
    the probed style ids map to other styles is given by indices into the
    probe list, and the styles they map to in "style_fallbacks".
    """
    names = metadata_names()
    files = language_files_by_id()
    failures = LoadFailures()
    fallbacks = {}
    languages = []
    for lang_id in lm.get_language_ids():
        lang = lm.get_language(lang_id)
        # Loads the language, if it hasn't been loaded
        own_styles = lang.get_style_ids() or []
        entry = {
            "id": lang_id,
            "name": lang.get_name(),
            "section": lang.get_section(),
            "hidden": lang.get_hidden(),
            "globs": lang.get_globs() or [],
            "mimetypes": lang.get_mime_types() or [],
            "metadata": {
                name: lang.get_metadata(name)
                for name in names
                if lang.get_metadata(name) is not None
            },
            "loads": files.get(lang_id) not in failures.files,
        }
        if entry["loads"]:
            entry["styles"] = {
                sid: [lang.get_style_name(sid) or "", lang.get_style_fallback(sid) or ""]
                for sid in sorted(own_styles)
            }
            mapped = []
            for index, sid in enumerate(probe):
                fallback = lang.get_style_fallback(sid)
                if fallback is None:
                    continue
                if fallbacks.setdefault(sid, fallback) != fallback:
                    print(f"{sid} maps to {fallbacks[sid]} and {fallback}", file=sys.stderr)
                mapped.append(index)
            entry["mapped_styles"] = mapped
        languages.append(entry)
    return languages, fallbacks


def language_files_by_id():
    """The file names of the languages, which GtkSourceView doesn't expose"""
    files = {}
    for path in lang_files():
        root = ET.parse(path).getroot()
        lang_id = (root.get("id") or root.get("_name") or root.get("name") or "").lower()
        files.setdefault(lang_id, os.path.basename(path))
    return files


def guess_data(lm):
    names = set(GUESS_NAMES)
    types = set(GUESS_TYPES)
    for lang_id in lm.get_language_ids():
        lang = lm.get_language(lang_id)
        for glob in lang.get_globs() or []:
            names.add(glob.replace("*", "file").replace("?", "x"))
        types.update(lang.get_mime_types() or [])

    names.discard("")
    cases = set()
    for name in names:
        cases.add((name, None))
        content_type, uncertain = Gio.content_type_guess(name, None)
        if content_type and not uncertain:
            cases.add((name, content_type))
    for content_type in types:
        cases.add((None, content_type))
    cases.update(GUESS_PAIRS)

    result = []
    for name, content_type in sorted(cases, key=lambda c: (c[0] or "", c[1] or "")):
        lang = lm.guess_language(name, content_type)
        result.append([name, content_type, lang.get_id() if lang else None])
    return result


def probe_style_ids():
    return sorted(set(all_style_ids()) | set(scheme_style_names()) | set(UI_STYLES))


def scheme_data(probe):
    manager = GtkSource.StyleSchemeManager()
    manager.set_search_path([STYLES])
    schemes = []
    for scheme_id in manager.get_scheme_ids():
        scheme = manager.get_scheme(scheme_id)
        styles = {}
        for style_id in probe:
            style = scheme.get_style(style_id)
            if style is None:
                continue
            props = style.props
            entry = {}
            for name in ("foreground", "background", "line-background", "underline-color"):
                attribute = name.replace("-", "_")
                if getattr(props, attribute + "_set"):
                    value = colour(getattr(props, attribute))
                    if value is not None:
                        entry[name] = value
            for name in ("bold", "italic", "strikethrough"):
                if getattr(props, name + "_set"):
                    entry[name] = bool(getattr(props, name))
            if props.underline_set:
                entry["underline"] = int(props.pango_underline)
            if props.scale_set:
                value = scale_value(props.scale)
                if value is not None:
                    entry["scale"] = value
            styles[style_id] = entry
        schemes.append(
            {
                "id": scheme_id,
                "name": scheme.get_name(),
                "description": scheme.get_description() or "",
                "authors": scheme.get_authors() or [],
                "metadata": {
                    name: scheme.get_metadata(name)
                    for name in ("variant", "light-variant", "dark-variant")
                    if scheme.get_metadata(name) is not None
                },
                "styles": styles,
            }
        )
    return schemes


def scale_value(text):
    """The scale gtk_source_style_apply() gives text tags"""
    if text is None:
        return None
    if text.lower() in PANGO_SCALES:
        return PANGO_SCALES[text.lower()]
    match = re.match(r"\s*[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?", text)
    if not match:
        return None
    value = float(match.group(0))
    return value if value > 0 else None


def utf16_offsets(line):
    """UTF-16 offsets of each character of a line, and of its end"""
    offsets = [0]
    for ch in line:
        offsets.append(offsets[-1] + (2 if ord(ch) > 0xFFFF else 1))
    return offsets


class Highlighter:
    def __init__(self, lm, scheme, style_of):
        self.lm = lm
        self.scheme = scheme
        self.style_of = style_of
        self.stacks = []
        self.stack_index = {}
        self.unknown = set()

    def stack_id(self, stack):
        key = tuple(stack)
        if key not in self.stack_index:
            self.stack_index[key] = len(self.stacks)
            self.stacks.append(list(stack))
        return self.stack_index[key]

    def styles_at(self, it):
        stack = []
        for tag in it.get_tags():
            # The context engine's tags have no names; named ones are
            # context classes and the like
            if tag.props.name is not None:
                continue
            if not tag.props.foreground_set:
                self.unknown.add("(none)")
                style = "?"
            else:
                key = rgba_hex(tag.props.foreground_rgba)[:7]
                style = self.style_of.get(key)
                if style is None:
                    self.unknown.add(key)
                    style = "?"
            if not stack or stack[-1] != style:
                stack.append(style)
        return stack

    def highlight(self, language_id, text):
        buf = GtkSource.Buffer()
        buf.set_highlight_matching_brackets(False)
        buf.set_style_scheme(self.scheme)
        buf.set_language(self.lm.get_language(language_id))
        buf.set_text(text)
        buf.ensure_highlight(buf.get_start_iter(), buf.get_end_iter())

        lines = text.split("\n")
        offsets = [utf16_offsets(line) for line in lines]
        runs = [[] for _ in lines]

        it = buf.get_start_iter()
        while not it.is_end():
            end = it.copy()
            end.forward_to_tag_toggle(None)
            stack = self.styles_at(it)
            if stack:
                index = self.stack_id(stack)
                first, last = it.get_line(), end.get_line()
                for line in range(first, last + 1):
                    start = it.get_line_offset() if line == first else 0
                    stop = end.get_line_offset() if line == last else len(lines[line])
                    stop = min(stop, len(lines[line]))
                    if start < stop:
                        line_runs = runs[line]
                        u_start, u_stop = offsets[line][start], offsets[line][stop]
                        # Join runs split by toggles of other tags
                        if line_runs and line_runs[-1][1] == u_start and line_runs[-1][2] == index:
                            line_runs[-1][1] = u_stop
                        else:
                            line_runs.append([u_start, u_stop, index])
            it = end
        # Each line's runs as flat lists of start, end, stack index
        return [[value for run in line_runs for value in run] for line_runs in runs]


def read_text(path):
    try:
        with open(path, encoding="utf-8", newline="") as f:
            text = f.read()
    except (OSError, UnicodeDecodeError):
        return None
    if any(ch in text for ch in "\r  \x85\x0b\x0c"):
        # Line breaks other than LF split lines differently in GTK
        return None
    return text


def samples(sample_dir):
    """(name, file path relative to tests/data/syntax or None, text)"""
    found = []
    for folder, _dirs, files in os.walk(sample_dir):
        for name in sorted(files):
            if name in ("README", "README.gtksourceview", "languages.txt"):
                continue
            path = os.path.join(folder, name)
            text = read_text(path)
            if text is None:
                print(f"Skipping {path}: not UTF-8 text with LF line breaks", file=sys.stderr)
                continue
            relative = os.path.relpath(path, sample_dir)
            found.append((relative, path, text))
    found.sort()
    return found


def language_overrides(sample_dir):
    """Languages for samples whose names don't identify them, from
    languages.txt: lines of "<sample path> <language id>" """
    overrides = {}
    path = os.path.join(sample_dir, "languages.txt")
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    name, lang_id = line.split()
                    overrides[name] = lang_id
    return overrides


def highlight_data(lm, highlighter, sample_dir, embed_all):
    overrides = language_overrides(sample_dir)
    result = []

    def add(name, lang_id, text, file=None):
        entry = {"name": name, "language": lang_id}
        if file is not None:
            entry["file"] = file
        else:
            entry["text"] = text
        entry["lines"] = highlighter.highlight(lang_id, text)
        result.append(entry)

    for relative, path, text in samples(sample_dir):
        lang_id = overrides.get(relative)
        if lang_id is None:
            lang = lm.guess_language(os.path.basename(relative), None)
            if lang is None:
                content_type, _uncertain = Gio.content_type_guess(path, text.encode("utf-8")[:4096])
                lang = lm.guess_language(None, content_type)
            if lang is None:
                print(f"Skipping {relative}: no language", file=sys.stderr)
                continue
            lang_id = lang.get_id()
        add(relative, lang_id, text, None if embed_all else relative)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--samples", default=SAMPLES, help="folder of sample files to highlight")
    parser.add_argument("--output", default=DEFAULT_OUTPUT, help="the reference data file to write")
    args = parser.parse_args()
    sample_dir = os.path.abspath(args.samples)
    # Samples outside the test data are included in the output
    embed_all = os.path.normpath(sample_dir) != os.path.normpath(SAMPLES)

    lm = GtkSource.LanguageManager()
    lm.set_search_path([LANGUAGE_SPECS, rng_folder()])
    style_ids = all_style_ids()
    scheme, style_of, _manager = oracle_scheme(style_ids)
    highlighter = Highlighter(lm, scheme, style_of)

    probe = probe_style_ids()
    languages, fallbacks = languages_data(lm, probe)
    data = {
        "generator": "tools/generate_syntax_reference_data.py",
        "gtksourceview": "5",
        "probe": probe,
        "languages": languages,
        "style_fallbacks": fallbacks,
        "guess": guess_data(lm),
        "highlight": highlight_data(lm, highlighter, sample_dir, embed_all),
    }
    data["stacks"] = highlighter.stacks
    data["schemes"] = scheme_data(probe)
    if highlighter.unknown:
        print("Unidentified styles:", sorted(highlighter.unknown), file=sys.stderr)

    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, separators=(",", ":"))
        f.write("\n")
    print(
        f"Wrote {args.output}: {len(data['languages'])} languages, "
        f"{len(data['guess'])} guesses, {len(data['highlight'])} samples, "
        f"{len(data['schemes'])} schemes"
    )


if __name__ == "__main__":
    main()
