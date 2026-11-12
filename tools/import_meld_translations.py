#!/usr/bin/env python3
"""Fill the translations in translations/ from the original Meld's.

Meld's translations are gettext catalogues, in po/ of its source. For each of
its languages, this gives the messages of translations/meld-qt_<language>.ts
(as lupdate leaves them, updated from the sources) Meld's translation of the
same English text, where they have none yet. It never changes a translation
that's already there (made in Qt Linguist, say), unless --force is given.

The English text is matched after the conversions that the port's messages
have from Meld's:

 * placeholders: Python's %s, %(name)s, {} and {name} are Qt's %1, %2...,
   numbered in the order of the English text, and %d in plural messages is
   Qt's %n;
 * mnemonics: GTK's _File is Qt's &File, or the label has none (a Japanese
   "ファイル(_F)" then loses its "(_F)");
 * the program is called Meld Qt, where Meld calls itself Meld.

Plural messages have their forms put in Qt's order for the language, with
Qt's plural rules as lconvert gives them. Translations that can't be matched
for sure (different translations of the same English text, plural rules that
don't correspond, placeholders that don't match) are kept, but unfinished:
these aren't built into the program until they're reviewed in Qt Linguist.

translations/meld-qt_en.ts, which only has plural forms, gets the English
singular and plural of Meld's plural messages. The desktop file
(data/Meld-Qt.desktop.in) gets its translated generic name, comment and
keywords too.

Usage: import_meld_translations.py [--force] [--languages de,fr] MELD_PO_DIR

Needs lconvert (from Qt Linguist's tools; found in PATH, Qt's host binaries
as qmake6 -query gives them, or LCONVERT), and only the standard library.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
TRANSLATIONS = os.path.join(ROOT, "translations")
DESKTOP_FILE = os.path.join(ROOT, "data", "Meld-Qt.desktop.in")

# Meld's languages (as in its po/LINGUAS) whose files are named otherwise
FILE_LANGUAGES = {"ca@valencia": "ca_valencia", "sr@latin": "sr_Latn"}
# ...and their languages in .ts files, which choose Qt's plural rules
TS_LANGUAGES = {"ca@valencia": "ca", "sr@latin": "sr_Latn"}

MELD = re.compile(r"\bMeld\b")


# ---------------------------------------------------------------------------
# gettext catalogues

PO_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "a": "\a", "b": "\b", "f": "\f", "v": "\v"}


def po_unquote(text):
    return re.sub(r"\\(.)", lambda m: PO_ESCAPES.get(m.group(1), m.group(1)), text[1:-1])


def parse_po(path):
    """The catalogue's header fields and entries, including obsolete ones"""
    entries = []
    entry = None
    key = None

    def flush():
        if entry is not None and "msgid" in entry:
            entries.append(entry)

    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip():
                flush()
                entry, key = None, None
                continue
            if entry is None:
                entry = {"flags": set(), "obsolete": False}
            if line.startswith("#~"):
                entry["obsolete"] = True
                line = line[2:].strip()
                if not line or line.startswith("#"):
                    if line.startswith("#,"):
                        entry["flags"].update(f.strip() for f in line[2:].split(","))
                    continue
            elif line.startswith("#,"):
                entry["flags"].update(f.strip() for f in line[2:].split(","))
                continue
            elif line.startswith("#"):
                continue
            m = re.match(r"(msgctxt|msgid_plural|msgid|msgstr(?:\[\d+\])?)\s+(\".*\")$", line)
            if m:
                key = m.group(1)
                entry[key] = po_unquote(m.group(2))
            elif line.startswith('"') and key:
                entry[key] += po_unquote(line)
    flush()

    header = {}
    for e in entries:
        if e["msgid"] == "" and not e["obsolete"]:
            for field in e.get("msgstr", "").split("\n"):
                name, _, value = field.partition(":")
                if value:
                    header[name.strip()] = value.strip()
    return header, [e for e in entries if e["msgid"] != ""]


def plural_forms(entry):
    forms = []
    while f"msgstr[{len(forms)}]" in entry:
        forms.append(entry[f"msgstr[{len(forms)}]"])
    return forms


# ---------------------------------------------------------------------------
# Plural rules: gettext's C expressions, evaluated

TOKEN = re.compile(r"\s*(\d+|n|&&|\|\||==|!=|<=|>=|[<>!?:%()+\-*/])")


def plural_function(expression):
    """A function of n for a C expression of gettext's Plural-Forms"""
    tokens = []
    position = 0
    expression = expression.strip().rstrip(";")
    while position < len(expression):
        m = TOKEN.match(expression, position)
        if not m:
            raise ValueError(f"Can't read the plural expression {expression!r}")
        tokens.append(m.group(1))
        position = m.end()
    tokens.append(None)
    index = [0]

    def peek():
        return tokens[index[0]]

    def take(expected=None):
        token = tokens[index[0]]
        if expected is not None and token != expected:
            raise ValueError(f"Expected {expected!r} in the plural expression {expression!r}")
        index[0] += 1
        return token

    binary = [["||"], ["&&"], ["==", "!="], ["<", ">", "<=", ">="], ["+", "-"], ["*", "/", "%"]]
    operations = {
        "||": lambda a, b: int(bool(a) or bool(b)), "&&": lambda a, b: int(bool(a) and bool(b)),
        "==": lambda a, b: int(a == b), "!=": lambda a, b: int(a != b),
        "<": lambda a, b: int(a < b), ">": lambda a, b: int(a > b),
        "<=": lambda a, b: int(a <= b), ">=": lambda a, b: int(a >= b),
        "+": lambda a, b: a + b, "-": lambda a, b: a - b,
        "*": lambda a, b: a * b, "/": lambda a, b: a // b, "%": lambda a, b: a % b,
    }

    def conditional():
        condition = level(0)
        if peek() != "?":
            return condition
        take("?")
        then = conditional()
        take(":")
        otherwise = conditional()
        return lambda n: then(n) if condition(n) else otherwise(n)

    def level(i):
        if i == len(binary):
            return unary()
        left = level(i + 1)
        while peek() in binary[i]:
            op = operations[take()]
            right = level(i + 1)
            left = (lambda op, l, r: lambda n: op(l(n), r(n)))(op, left, right)
        return left

    def unary():
        token = take()
        if token == "!":
            operand = unary()
            return lambda n: int(not operand(n))
        if token == "(":
            inner = conditional()
            take(")")
            return inner
        if token == "n":
            return lambda n: n
        if token is not None and token.isdigit():
            value = int(token)
            return lambda n: value
        raise ValueError(f"Unexpected {token!r} in the plural expression {expression!r}")

    result = conditional()
    take(None)
    return result


def parse_plural_forms(header_value):
    m = re.search(r"nplurals\s*=\s*(\d+)\s*;\s*plural\s*=\s*(.+)", header_value or "")
    if not m:
        return 2, plural_function("n != 1")
    return int(m.group(1)), plural_function(m.group(2))


def qt_plural_forms(lconvert, ts_language, cache={}):
    """Qt's plural rule for a .ts language, as lconvert writes it for gettext"""
    if ts_language not in cache:
        with tempfile.TemporaryDirectory() as folder:
            ts = os.path.join(folder, "plurals.ts")
            po = os.path.join(folder, "plurals.po")
            with open(ts, "w", encoding="utf-8") as f:
                f.write('<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n'
                        f'<TS version="2.1" language="{ts_language}">\n<context><name>c</name>'
                        '<message numerus="yes"><source>%n</source><translation>'
                        '<numerusform>x</numerusform></translation></message></context>\n</TS>\n')
            subprocess.run([lconvert, "-i", ts, "-o", po], check=True)
            header, _ = parse_po(po)
            cache[ts_language] = parse_plural_forms(header.get("Plural-Forms"))
    return cache[ts_language]


def form_mapping(qt_rule, po_rule):
    """For each of Qt's forms, how often the catalogue's forms are used for
    the same numbers (as {form: count})"""
    qt_count, qt_form = qt_rule
    _po_count, po_form = po_rule
    seen = [dict() for _ in range(qt_count)]
    for n in range(0, 2000):
        q = qt_form(n)
        if q < qt_count:
            seen[q][po_form(n)] = seen[q].get(po_form(n), 0) + 1
    return seen


def qt_forms(mapping, forms):
    """A plural message's forms in Qt's order, and whether they're exactly
    the catalogue's: each of Qt's forms is used for numbers whose forms in the
    catalogue are all the same text. None if a form is missing."""
    texts = []
    exact = True
    for counts in mapping:
        if not counts or any(g >= len(forms) for g in counts):
            return None
        texts.append(forms[max(counts, key=counts.get)])
        exact = exact and len({forms[g] for g in counts}) == 1
    return texts, exact


# ---------------------------------------------------------------------------
# Converting Meld's messages to the port's

def convert_placeholders(text, flags, numbers):
    """Python placeholders as Qt's %1, %2..., numbered by `numbers` (a dict
    of placeholder names, filled in for the English text), and %d as %n in
    plural messages (numbers is None for these)"""
    plural = numbers is None
    positional = [0]

    def number(name):
        if plural:
            return "%n"
        if name is None:
            positional[0] += 1
            name = positional[0]
        if name not in numbers:
            numbers[name] = len(numbers) + 1
        return f"%{numbers[name]}"

    if "python-brace-format" in flags:
        def brace(m):
            if m.group(0) in ("{{", "}}"):
                return m.group(0)[0]
            name = m.group(1)
            return number(int(name) + 1 if name.isdigit() else (name or None))
        return re.sub(r"\{\{|\}\}|\{(\w*)\}", brace, text)
    if "python-format" in flags:
        def percent(m):
            if m.group(0) == "%%":
                return "%"
            return number(m.group(1))
        return re.sub(r"%%|%(?:\((\w+)\))?[-#0 +]*\d*(?:\.\d+)?[sdirfxXoeEgGc]", percent, text)
    return text


MNEMONIC = re.compile(r"_(?=[^_\s])")
CJK_MNEMONIC = re.compile(r"\s*[(（]_[^)）]{1,2}[)）]")


def has_mnemonic(text):
    return bool(MNEMONIC.search(text.replace("__", "")))


def qt_mnemonic(text):
    """GTK's _File as Qt's &File (& itself is &&, __ is _)"""
    parts = text.split("__")
    return "_".join(MNEMONIC.sub("&", part.replace("&", "&&"), count=1) for part in parts)


def without_mnemonic(text):
    text = CJK_MNEMONIC.sub("", text)
    return "_".join(MNEMONIC.sub("", part, count=1) for part in text.split("__"))


def variants(source, translation):
    """(English, translation, exact) pairs that the port's messages may have
    for a message of Meld's; exact is False for those that are less likely
    to be what the port's message means"""
    pairs = [(source, translation, True)]
    if has_mnemonic(source):
        # Without the mnemonic, the English may be another message's too,
        # whose translation is then the better one
        pairs = [(qt_mnemonic(source), qt_mnemonic(translation), True),
                 (without_mnemonic(source), without_mnemonic(translation), False)]
    result = []
    for s, t, exact in pairs:
        result.append((s, t, exact))
        # Meld Qt, where Meld calls itself Meld, if the translation names it
        # as often
        meld = len(MELD.findall(s))
        if meld and len(MELD.findall(t)) == meld:
            result.append((MELD.sub("Meld Qt", s), MELD.sub("Meld Qt", t), exact))
    return result


def format_flags(entry):
    """The flags to read an entry's placeholders with: its own, or, as some
    of Meld's formatted messages aren't marked as such, the formats its text
    looks like it has too"""
    flags = [entry["flags"]]
    if not entry["flags"] & {"python-format", "python-brace-format"}:
        if re.search(r"\{\w*\}", entry["msgid"]):
            flags.append(entry["flags"] | {"python-brace-format"})
        if re.search(r"%(?:\(\w+\))?[sdi]", entry["msgid"]):
            flags.append(entry["flags"] | {"python-format"})
    return flags


def filter_names(text):
    """The names in a list of filters, as the default of the original's
    filename-filters and text-filters settings has them"""
    return re.findall(r'\(\s*"((?:[^"\\]|\\.)*)"\s*,\s*(?:true|false)\s*,', text)


class Catalogue:
    """Meld's translations of one language, by the port's English text"""

    def __init__(self, path):
        self.header, entries = parse_po(path)
        self.plural_rule = parse_plural_forms(self.header.get("Plural-Forms"))
        self.messages = {}   # English -> [(rank, translation)]
        self.plurals = {}    # English plural -> (singular, plural, forms)
        for e in entries:
            if "fuzzy" in e["flags"]:
                continue
            # Current translations are better than obsolete ones
            rank = 1 if e["obsolete"] else 0
            if "msgctxt" in e:
                rank += 2
            if "msgid_plural" in e:
                forms = [convert_placeholders(f, e["flags"], None) for f in plural_forms(e)]
                if forms and all(forms):
                    singular = convert_placeholders(e["msgid"], e["flags"], None)
                    plural = convert_placeholders(e["msgid_plural"], e["flags"], None)
                    if plural not in self.plurals or rank < self.plurals[plural][3]:
                        self.plurals[plural] = (singular, plural, forms, rank)
                continue
            translation = e.get("msgstr", "")
            if not translation:
                continue
            for flags in format_flags(e):
                numbers = {}
                source = convert_placeholders(e["msgid"], flags, numbers)
                translated = convert_placeholders(translation, flags, dict(numbers))
                for s, t, exact in variants(source, translated):
                    self.add(s, t, rank + (0 if exact else 4))
            # The names of the default filters, from the settings' default
            names = filter_names(e["msgid"])
            translated_names = filter_names(translation)
            if names and len(names) == len(translated_names):
                for name, translated_name in zip(names, translated_names):
                    self.add(name, translated_name, rank + 8)

    def add(self, source, translation, rank):
        self.messages.setdefault(source, []).append((rank, translation))

    def lookup(self, source):
        """The translation, and whether it's certain; None if there's none"""
        candidates = self.messages.get(source)
        if not candidates:
            return None
        best = min(rank for rank, _ in candidates)
        translations = {t for rank, t in candidates if rank == best}
        translation = sorted(translations)[0]
        return translation, len(translations) == 1


# ---------------------------------------------------------------------------
# Checks of translations against their English text

def placeholders(text):
    return sorted(set(re.findall(r"%(\d+|n)", text)))


def accelerators(text):
    return len(re.findall(r"&(?=[^&\s])", text.replace("&&", "")))


def consistent(source, translation, numerus=False):
    expected = [p for p in placeholders(source) if p != "n"] if numerus else placeholders(source)
    found = [p for p in placeholders(translation) if p != "n"] if numerus else placeholders(translation)
    if expected != found:
        return False
    # A translation may leave out the accelerator, as gettext's may leave
    # out the mnemonic, but has at most one
    return accelerators(translation) <= max(1, accelerators(source))


# ---------------------------------------------------------------------------
# .ts files

def translation_is_empty(translation):
    if translation is None:
        return True
    if translation.get("type") in ("vanished", "obsolete"):
        return False
    if len(translation):
        return not any((form.text or "") for form in translation)
    return not (translation.text or "")


def set_translation(message, texts, finished, numerus):
    translation = message.find("translation")
    if translation is None:
        translation = ET.SubElement(message, "translation")
    translation.attrib.pop("type", None)
    if not finished:
        translation.set("type", "unfinished")
    for child in list(translation):
        translation.remove(child)
    if numerus:
        translation.text = None
        for text in texts:
            ET.SubElement(translation, "numerusform").text = text
    else:
        translation.text = texts[0]


def fill(ts_path, catalogue, qt_rule, force, english=False):
    tree = ET.parse(ts_path)
    counts = {"finished": 0, "unfinished": 0, "messages": 0}
    for message in tree.getroot().iter("message"):
        translation = message.find("translation")
        if translation is not None and translation.get("type") in ("vanished", "obsolete"):
            continue
        counts["messages"] += 1
        if not force and not translation_is_empty(translation):
            continue
        source = message.find("source").text or ""
        numerus = message.get("numerus") == "yes"
        result = None
        if numerus:
            plural = catalogue.plurals.get(source) if catalogue else None
            if english and plural:
                singular, plural_text, _forms, _rank = plural
                result = ([singular, plural_text], True)
            elif plural:
                converted = qt_forms(form_mapping(qt_rule, catalogue.plural_rule), plural[2])
                if converted:
                    texts, exact = converted
                    result = (texts, exact and all(consistent(source, t, numerus=True) for t in texts))
        elif catalogue and not english:
            found = catalogue.lookup(source)
            if found:
                text, certain = found
                result = ([text], certain and consistent(source, text))
        if result:
            set_translation(message, result[0], result[1], numerus)
            counts["finished" if result[1] else "unfinished"] += 1
    tree.write(ts_path, encoding="utf-8", xml_declaration=True)
    return counts


def normalise(lconvert, ts_path):
    """Write the .ts file as Qt's tools do"""
    subprocess.run([lconvert, "-locations", "relative", "-i", ts_path, "-o", ts_path], check=True)


# ---------------------------------------------------------------------------
# The desktop file

DESKTOP_KEYS = {"GenericName": "Diff Viewer", "Comment": "Compare and merge your files", "Keywords": "diff;merge;"}


def update_desktop_file(catalogues):
    with open(DESKTOP_FILE, encoding="utf-8") as f:
        lines = [line for line in f.read().split("\n") if not re.match(r"\w+\[[^]]+\]=", line)]
    result = []
    for line in lines:
        result.append(line)
        key = line.partition("=")[0]
        if key in DESKTOP_KEYS:
            for language, catalogue in sorted(catalogues.items()):
                found = catalogue.lookup(DESKTOP_KEYS[key])
                if found and found[1]:
                    value = found[0].replace("\n", " ")
                    if key == "Keywords" and not value.endswith(";"):
                        value += ";"
                    result.append(f"{key}[{language}]={value}")
    with open(DESKTOP_FILE, "w", encoding="utf-8") as f:
        f.write("\n".join(result))


# ---------------------------------------------------------------------------

def find_lconvert():
    if os.environ.get("LCONVERT"):
        return os.environ["LCONVERT"]
    for qmake in ("qmake6", "qmake"):
        if shutil.which(qmake):
            folder = subprocess.run([qmake, "-query", "QT_HOST_BINS"], capture_output=True, text=True).stdout.strip()
            candidate = os.path.join(folder, "lconvert")
            if folder and os.path.exists(candidate):
                return candidate
    for name in ("lconvert6", "lconvert-qt6", "lconvert"):
        if shutil.which(name):
            return shutil.which(name)
    sys.exit("Couldn't find lconvert (Qt Linguist's tools); give its path in LCONVERT")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("po_dir", help="Meld's po folder, with its LINGUAS and .po files")
    parser.add_argument("--force", action="store_true",
                        help="replace translations that are already there")
    parser.add_argument("--languages", help="only these languages (as in LINGUAS), separated by commas")
    args = parser.parse_args()
    lconvert = find_lconvert()

    with open(os.path.join(args.po_dir, "LINGUAS"), encoding="utf-8") as f:
        languages = [w for line in f for w in line.split("#")[0].split()]
    if args.languages:
        wanted = args.languages.split(",")
        languages = [language for language in languages if language in wanted]

    catalogues = {}
    for language in languages:
        ts_path = os.path.join(TRANSLATIONS, f"meld-qt_{FILE_LANGUAGES.get(language, language)}.ts")
        if not os.path.exists(ts_path):
            print(f"Skipping {language}: there's no {os.path.relpath(ts_path, ROOT)} "
                  "(add it, and update it from the sources with lupdate)", file=sys.stderr)
            continue
        catalogue = Catalogue(os.path.join(args.po_dir, f"{language}.po"))
        catalogues[language] = catalogue
        ts_language = ET.parse(ts_path).getroot().get("language") or TS_LANGUAGES.get(language, language)
        counts = fill(ts_path, catalogue, qt_plural_forms(lconvert, ts_language), args.force)
        normalise(lconvert, ts_path)
        print(f"{language}: {counts['finished']} translated, {counts['unfinished']} to review, "
              f"of {counts['messages']} messages")

    # The English plural forms, of every language's plural messages
    english = os.path.join(TRANSLATIONS, "meld-qt_en.ts")
    if catalogues and os.path.exists(english):
        # Any language's catalogue has the English of its plural messages
        plurals = Catalogue.__new__(Catalogue)
        plurals.plurals = {}
        for catalogue in catalogues.values():
            for key, value in catalogue.plurals.items():
                plurals.plurals.setdefault(key, value)
        counts = fill(english, plurals, None, args.force, english=True)
        normalise(lconvert, english)
        print(f"en (plural forms): {counts['finished']} translated, of {counts['messages']} messages")

    if not args.languages:
        update_desktop_file(catalogues)
        print("Updated", os.path.relpath(DESKTOP_FILE, ROOT))


if __name__ == "__main__":
    main()
