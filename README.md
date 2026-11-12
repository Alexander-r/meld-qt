# Meld Qt

A C++/Qt Widgets port of [Meld](https://meld.app/), the visual diff and merge
tool. It supports two- and three-way comparison and merging of files and
folders, and version control views of Git, Mercurial, Subversion, Bazaar,
Darcs and CVS working copies, and highlights the syntax of the files it
compares. It mirrors the original's interface: header bar, comparison tabs,
link maps with curved chunk connectors, merge action gutters, overview chunk
maps and the "New comparison" page.

## Requirements

- CMake 3.21 or newer, or qmake from Qt 6 (see "Building with qmake")
- A C++23 compiler and standard library with `std::generator`; this means
  GCC 14 or newer (libstdc++), or MSVC 17.10 or newer
- Qt 6.8 or newer: Core, Gui, Widgets and Concurrent, plus Test for the test
  suite, and Network with `MELD_SINGLE_INSTANCE_NETWORK` (see below)
- Qt Linguist's tools (`lrelease` and `lupdate`, and with CMake, Qt's
  LinguistTools package), which build the translations in: e.g., `qt6-tools`
  on Arch, and `qt6-l10n-tools` and `qt6-tools-dev` on Debian and Ubuntu
- To open archives (optional, at run time): `bsdtar` (from libarchive), or
  7-Zip (`7z`), GNU tar or `unzip` for the formats they read (see below)

## Building and running

```sh
cmake -S . -B build
cmake --build build -j
build/src/app/meld-qt                      # empty window with a "New comparison" tab
build/src/app/meld-qt left.txt right.txt   # 2-way file comparison
build/src/app/meld-qt a.txt base.txt b.txt # 3-way file comparison
build/src/app/meld-qt dirA dirB [dirC]     # 2- or 3-way folder comparison
build/src/app/meld-qt --auto-merge -o out.txt local.txt base.txt remote.txt
build/src/app/meld-qt path/to/checkout     # version control view
build/src/app/meld-qt path/to/checkout/file.txt  # compare with the repository version
build/src/app/meld-qt --help
```

Build options (`cmake -D<option>=ON|OFF`):

| Option | Default | |
|--------|---------|-|
| `MELD_SINGLE_INSTANCE` | ON | Build single-instance mode (see below) |
| `MELD_SINGLE_INSTANCE_NETWORK` | OFF | Implement it with Qt Network's local sockets, rather than with shared memory |
| `MELD_BUILD_TESTS` | ON | Build the test suite |
| `MELD_INSTALL_DESKTOP_INTEGRATION` | ON | Install the desktop file, icons and MIME type with the program (Linux and BSDs; see "Installing") |

`-DMELD_VERSION=<version>` sets the version that the program reports, in its
About window and with `--version`, rather than the project's. For example, for
a build of a git commit:

```sh
cmake -S . -B build -DMELD_VERSION="$(git describe --always --dirty)"
```

The command line matches the original's options: `-L/--label`,
`-a/--auto-compare`, `-o/--output`, `--auto-merge`, `--diff` (repeatable)
and `--comparison-file`. `@blank` gives an empty pane. `-u/--unified` is
accepted for compatibility and ignored.

### Building with qmake

`meld-qt.pro` builds the same program with qmake, for opening in Qt Creator
(which also opens `CMakeLists.txt`). From the command line, use Qt 6's qmake,
in a build folder:

```sh
mkdir build-qmake && cd build-qmake
qmake6 ../meld-qt.pro
make -j$(nproc)
src/app/meld-qt
make -k check                      # the tests, as ctest does
make install                       # to PREFIX, /usr/local by default
make install INSTALL_ROOT=staging  # staged for a package, as DESTDIR
```

The build options are given to qmake, or under "Additional arguments" of the
qmake step in Qt Creator:

| Argument | As CMake's |
|----------|------------|
| `CONFIG+=meld_no_single_instance` | `MELD_SINGLE_INSTANCE=OFF` |
| `CONFIG+=meld_single_instance_network` | `MELD_SINGLE_INSTANCE_NETWORK=ON` |
| `CONFIG+=meld_no_tests` | `MELD_BUILD_TESTS=OFF` |
| `CONFIG+=meld_no_desktop_integration` | `MELD_INSTALL_DESKTOP_INTEGRATION=OFF` |
| `MELD_VERSION=<version>` | `MELD_VERSION=<version>` |
| `PREFIX=<path>` | `CMAKE_INSTALL_PREFIX` |

The two projects are maintained separately, so a new source file needs adding
to both: to `src/core/core.pro` or `src/ui/ui.pro`, and a new test needs a
`tests/tst_<name>.pro`, listed in `tests/tests.pro`. New language definitions
and colour schemes are found when qmake is next run.

### Single-instance mode

By default every invocation starts its own Meld Qt. In single-instance mode,
comparisons open in a Meld Qt that's already running instead: each invocation
gets a new window there, and `-n/--newtab` a new tab in its most recently
used window. The invocation prints its errors and, as with the original
(e.g., as a Git merge tool), ends when its comparison is closed.

Single-instance mode is the "Open comparisons in the running Meld Qt"
preference. `--single-instance` and `--no-single-instance` override it for
one invocation, without changing it. If no Meld Qt in single-instance mode is
running, the invocation starts one.

There are two implementations:

- Shared memory (the default), which only needs Qt Core. A Meld Qt in
  single-instance mode creates a shared memory segment, which it checks for
  the command lines of later invocations ten times a second. Meld Qt started
  without single-instance mode doesn't use shared memory at all, so
  `--newtab` is ignored then: there's no running Meld Qt it could reach.
- Local sockets from Qt Network (`MELD_SINGLE_INSTANCE_NETWORK`). Every
  Meld Qt listens on a socket in the user's runtime folder, without polling,
  so `--newtab` reaches a running Meld Qt even without single-instance mode
  (unless `--no-single-instance` is given).

Without `MELD_SINGLE_INSTANCE`, every invocation starts its own Meld Qt,
`--newtab` is accepted and ignored, and the options and preference for
single-instance mode don't exist.

### Version control

A single folder opens the version control view. It lists the working copy's
changes (by default modified files, with folders flattened; see "Version
Filters" in the header bar), and runs version control commands from its
action bar and context menu: commit, pull, push, add, unstage, remove,
revert, resolve and delete. The commands and their output are shown in the
console (F9). Activating a file compares it with its repository version;
activating a conflicted file opens a three-way merge, which offers to mark
the conflict as resolved once the merge is saved and closed. A single file
opens that comparison directly.

The version control programs (`git`, `hg`, `svn`, `bzr`, `darcs`, `cvs`) are
run as in the original; folders that aren't in a repository are shown
without version control. Paths follow the original's `os.path` rules for
the platform: on Windows, those of `ntpath` (drive letters, UNC shares,
paths compared without case), which are tested on any platform.

#### Comparing revisions (git)

This isn't in the original. For git repositories, a bar below the action
bar chooses what's compared: "Changes from [HEAD] to [working copy]" by
default, as in the original. Either side can be any branch, tag or recent
commit from its list, or anything typed that git can read as a commit, tag
or tree (e.g., `HEAD~3`, `v1.0^{tree}` or a commit id). "Swap" swaps two
revisions, and "Reset" goes back to the default.

- **A revision and the working copy**: the view lists the working copy's
  changes from that revision. A file's comparison shows the revision's text,
  read-only, with the working file, which can be edited and saved.
- **Two revisions**: the view lists the files that differ between them,
  including files and folders that are only in one of them, whether or not
  they're on disk. Comparisons are read-only.

Revisions' files are read into memory (`git cat-file`), never written to
temporary files or checked out. Version control actions (commit, add,
revert, …) are disabled while another revision than the current one is
compared, as the listed changes aren't what they'd act on. The chosen
revisions are kept when the view is refreshed. Other version control
systems don't offer this yet, and don't show the bar.

### Archives

Archives (e.g., `.zip`, `.tar.gz`, `.7z`, `.rar`, `.iso`, `.deb`, `.rpm`)
given on the command line, dropped on the window or reopened from the
recent comparisons are compared as folders, as in the original:

```sh
build/src/app/meld-qt old.tar.gz new.zip   # folder comparison of two archives
build/src/app/meld-qt release.zip src/     # an archive against a folder
build/src/app/meld-qt release.zip          # an archive's files, without version control
```

The original mounts archives with gvfs. Here they're extracted to a
temporary folder (`meld-qt-archive-*` in the system's, or `TMPDIR`) with an
installed program, the first of:

- `bsdtar`, or `tar` if it's bsdtar (as on BSDs, macOS and Windows 10 and
  newer), which read every format, as gvfs does with libarchive;
- 7-Zip (`7z`, `7zz` or `7za`) for zip, 7z, rar, cab, ISO, lha, cpio, deb, ar
  and xar archives;
- GNU tar for tar archives, compressed or not;
- `unzip` for zip archives (including `.jar` and `.apk`).

Large archives show a progress dialog after half a second, which can cancel
them. As with gvfs mounts, the extracted archives can't be changed: their
folders are read-only, and their files open read-only (they can be saved
elsewhere). Folder comparisons, tab titles and path labels show the archives
rather than the temporary folders, and recent comparisons record the
archives. Extracted archives are removed when the folder comparison that
opened them closes, or when Meld Qt quits.

### Resolving conflict markers

This isn't in the original. A file with unresolved merge conflicts, as
version control leaves them, is opened as a 3-way merge of its local and
remote versions, made in memory from its conflict markers:

- **A single file** (e.g., `meld-qt conflicted.txt`) with conflict markers opens
  as a merge. If version control still has the conflict's sides as whole
  files (as git does until the file is added), those are used, as before.
- **Any file comparison** that loads a file with conflict markers offers to
  merge it, above that file's pane.
- **The version control view** merges conflicted files from their markers
  for systems that can't give the conflict's sides.

Files are only checked for markers as they're opened, never while scanning
folders. Both git's default `merge` style and the `diff3` style (with the
common ancestor) are read; markers start their lines, their length is the
first one's (git's `conflict-marker-size`), and incomplete markers are
reported rather than guessed at.

The local and remote versions are read-only, ordered as "Order when merging
files" says. The middle pane is the merge, which is saved to the file: the
text outside conflicts, with conflicts empty, to be filled from the local and
remote versions. With `diff3`-style markers, the conflicts can show the
common ancestor's text instead ("Show the common ancestor in conflicts from
conflict markers" in the version control preferences, on by default).
Saving the merge with conflicts left asks first, as it would drop them. In a
repository, saving offers to mark the file as resolved, as for version
control conflicts.

### Comparing with a patch

This isn't in the original. On the "New comparison" page, "Compare with a
patch" (for files or folders) makes the second chooser select a patch file.
The file or folder is then compared with what the patch would make of it.
On the command line, `--patch` does the same:

```sh
build/src/app/meld-qt --patch fix.patch src/main.c   # a file with the patch applied
build/src/app/meld-qt --patch fix.patch project/     # a folder with the patch applied
build/src/app/meld-qt -a --patch fix.patch project/  # ...and compare each changed file
build/src/app/meld-qt --patch fix.patch release.tar.gz  # an archive, extracted first
```

`--patch` takes exactly one file or folder. It can be combined with `-L`
labels, `--auto-compare` and `--newtab`, but not with `--auto-merge` or
`--comparison-file`; `--diff` comparisons alongside it open without the patch.


- **A file** is compared with its patched text. If the patch changes several
  files, the one whose path best matches the file's is used.
- **A folder** is compared with the patched folder, which lists the files
  the patch changes, adds and deletes (including renamed files). Changes
  that can't be applied, such as to files that aren't in the folder, are
  shown as errors in the patched folder. A file's patched text is only made
  when it's compared.

Patches are unified diffs, as made by `diff -u`, `git diff` and `git
format-patch`, including git's new, deleted, renamed and copied files.
Patches from Windows tools whose paths use backslashes (and no `/`) are
read too.
Context diffs and binary changes can't be applied. For folders, the number
of leading path components to strip (as `patch -p`) is the one that finds
the most files to change, e.g. 1 for git's `a/` and `b/` prefixes.

Patches are applied in memory, without the `patch` program or temporary
files, as GNU patch applies them by default: where the patch says, or else
at the nearest place the hunk matches (an offset), or else ignoring up to
two context lines (fuzz). Patched text is also accepted where line endings
differ from the patch's (CRLF or LF); new lines then get the file's line
endings. How hunks were applied, if not exactly, is shown above the patched
text. If a patch can't be applied, the patched pane shows which hunks don't
match, or that the patch seems to be applied already.

Patched text has no file: it's read-only, and can be saved elsewhere.
Reverting the pane applies the patch again, to the file as it is then.
Comparisons with patches aren't added to the recent comparisons.

### Syntax highlighting

The original highlights syntax with GtkSourceView. This port has its own
highlighting engine (`src/core/syntax/`), a port of GtkSourceView's: it
reads the same language definitions (`.lang` files) and colour schemes,
and gives the same results (see the tests below). The definitions of 179
languages and GtkSourceView's colour schemes are bundled
(`src/resources/language-specs` and `src/resources/styles`, from
GtkSourceView 5.22.0), with Meld's own `meld-base` and `meld-dark` schemes.

As in the original, the language of a comparison is guessed from the
files' names and content types, and if only one language is found, it's
used for all panes. The status bar's language selector changes it. The
"Use Syntax Highlighting" and "Color Scheme" preferences turn highlighting
off and choose the scheme, which also gives the comparison's colours
(changes, inline highlights and so on) where it has them; Meld's own
schemes follow the application's light or dark style. Patches made with
"Format as Patch" are highlighted too.

Highlighting is lazy: lines are highlighted as they're shown, and the rest
of the file in the background, so large files open as quickly as without
highlighting.

### Translations

The program is translated into the original's 51 languages, with its
translations: they're Qt Linguist's `.ts` files, in `translations/`, made
from Meld's gettext ones by `tools/import_meld_translations.py`, and built
into the program. It uses the user's language as gettext would (`LANGUAGE`,
then `LC_ALL`, `LC_MESSAGES` and `LANG`, including Serbian's `@latin` and
Valencian's `@valencia`), with Qt's own translations for its standard dialogs
and buttons, if they're installed. Preferences (Editor, Appearance) has a
language setting, which chooses one of the translations (or English) instead
of the system's language; the language changes when Meld Qt is next started.
`translations/README` has how to translate in Qt Linguist, and how to
update the translations when the messages change.

## Tests

```sh
ctest --test-dir build --output-on-failure
```

The `tests/` directory has these kinds of tests:

- **Ports of the original's Python test suite**:
  matchers, buffer lines, chunk actions, text and filename filters, file
  comparison, I/O helpers, misc utilities, style (font zoom), accelerators
  and the action gutter classification.
- **Version control tests** (`tst_vc`, `tst_vcview`). The original has no
  tests for its version control support. These test the git plugin and
  the version control view end-to-end on temporary repositories (they're
  skipped if git isn't installed), and test all plugins against the
  original: `tests/data/vc_reference.json` holds the results of the
  original plugins run on canned command output, which the ported plugins
  must reproduce. It also covers the valid actions for selections, entry
  states, merged-file reconstruction and commit message wrapping. To
  regenerate it, which needs Meld and PyGObject installed on the system, run
  `python3 tools/generate_vc_reference_data.py`.
- **Differential tests against the Python implementation.**
  `tests/data/reference.json` holds the output of the original Myers
  matchers (plain, inline and sync point), the `Differ` chunk model
  (including incremental edits), the three-way auto-merge and difflib's
  unified diff, all run on randomly generated inputs. The C++ engine must
  reproduce these exactly. To regenerate the data, which needs Meld and
  PyGObject installed on the system (the data is the same with Meld 3.24.1),
  run:

  ```sh
  python3 tools/generate_reference_data.py
  ```

- **Syntax highlighting tests** (`tst_syntax`, `tst_highlighting`). The
  original relies on GtkSourceView, so these test the port against
  GtkSourceView 5 itself: `tests/data/syntax_reference.json` holds what
  GtkSourceView does with the bundled definitions and schemes, namely each
  language's metadata and styles, the languages guessed for over 700 file names
  and content types, the styles of every character of the samples in
  `tests/data/syntax` (GtkSourceView's own test files, and a few small
  samples of this project's in `extra/`, for languages and styles those
  don't cover), and every style of every scheme. The port must reproduce
  them exactly, apart from here-docs (see "Differences from the original"). `tst_highlighting` tests highlighting in comparisons:
  incremental highlighting after edits, the comparison colours of schemes,
  and choosing languages. To regenerate the data, which needs PyGObject with
  GtkSourceView 5, run:

  ```sh
  python3 tools/generate_syntax_reference_data.py
  ```

  `--samples DIR --output FILE` compares other files: run `tst_syntax` with
  `MELD_SYNTAX_REFERENCE=FILE` to test the port against GtkSourceView on
  them.

- **Integration tests** for file and folder comparisons, the main window and
  command line handling (`tst_filediff`, `tst_dirdiff`, `tst_window`). These
  drive the real widgets and actions on Qt's offscreen platform.
- **Conflict marker tests** (`tst_conflicts`): reading `diff3` and `merge`
  style markers (marker sizes, line endings, incomplete markers), and the
  merges made from them, with git's own conflicts preferred.
- **Revision comparison tests** (`tst_vcrevisions`): git's revisions, the
  states between revisions and with the working copy, the selectors, and the
  comparisons, on real repositories, checking that nothing is written to
  temporary files.
- **Patch tests** (`tst_patch`): reading patches, applying them (offsets,
  fuzz, line endings, new, deleted and renamed files, failures), patched
  folders, and the comparisons in the window. The applier was also checked
  against GNU patch on randomly made patches, with identical results.
- **Archive tests** (`tst_archive`): which files are archives, extracting
  them with each supported program that's installed (others are skipped),
  unsafe and special members, and comparing archives in the window. The
  archives in `tests/data/archives` are made by
  `tools/generate_archive_test_data.py`.
- **Translation tests** (`tst_translations`): choosing the translation for
  the user's languages and locale, or for the language setting, and choosing
  it in Preferences, installing it (with English's plural forms, and Qt's
  own), that every translation is built in and has the
  placeholders and accelerators of its English, and that the text the
  windows show is translatable: with a translator that marks every message
  of the translations, the main window, comparisons, the version control
  view, Preferences, About and the keyboard shortcuts mustn't show text
  that isn't marked (apart from data such as paths).

## Installing

```sh
cmake --install build                      # to CMAKE_INSTALL_PREFIX, /usr/local by default
cmake --install build --prefix ~/.local    # for the current user only
DESTDIR=staging cmake --install build --prefix /usr   # staged for a package
```

The program is installed as `meld-qt`. On Linux and BSDs, desktop integration
is installed with it unless `MELD_INSTALL_DESKTOP_INTEGRATION` is OFF:

| File | Installed to (in the prefix) | |
|------|------------------------------|-|
| `Meld-Qt.desktop` | `share/applications` | Lists Meld Qt in application menus, and to open files with |
| `Meld-Qt.xml` | `share/mime/packages` | The MIME type of saved comparisons (`*.meldcmp`), which open in Meld Qt |
| `Meld-Qt.svg`, `Meld-Qt-symbolic.svg` | `share/icons/hicolor` | The application's icons |

None of these have the original's names, so Meld Qt can be installed
alongside the original Meld (`meld`, `org.gnome.Meld.*`). The MIME type,
`application/x-meld-comparison`, is the original's, as the files are the same
format, so both programs are offered for opening `.meldcmp` files. Unless the
installation is staged with `DESTDIR` (whose package's installation does
this), the caches of desktop files, MIME types and icons in the prefix are
updated with `update-desktop-database`, `update-mime-database` and
`gtk-update-icon-cache`, those that are installed; none are required. The
desktop file's `Exec` is `meld-qt`, so the prefix's `bin` must be in `PATH`
(as `/usr/local/bin` and usually `~/.local/bin` are), and desktops only find
the files in prefixes in `XDG_DATA_DIRS` (or `~/.local/share`). The tests
include `desktop-file-validate` on the desktop file, if it's installed.

## Source layout

| Path | Contents | Python original |
|------|----------|-----------------|
| `src/core/` | Diff engine and document model, and reading and applying patches (`patch.h`, not in the original); no widgets apart from the text layout | `meld/matchers/`, `meldbuffer.py`, `filters.py`, `misc.py`, `iohelpers.py`, `archivehelpers.py`, `task.py`, `dirdiff.py` (file comparison) |
| `src/core/vc/` | Version control plugins; | `meld/vc/` |
| `src/core/syntax/` | Syntax highlighting: language definitions, colour schemes and the highlighting engine; | GtkSourceView |
| `src/resources/` | Icons, and the language definitions and colour schemes, which are GtkSourceView's (see the README there) | `data/styles/` |
| `src/ui/` | Comparison tabs, custom views and the main window | `filediff.py`, `dirdiff.py`, `vcview.py`, `meldwindow.py`, `sourceview.py`, `linkmap.py`, `actiongutter.py`, `chunkmap.py`, … |
| `src/app/` | `main()` | `bin/meld` |
| `data/` | Desktop integration: desktop file, MIME type and icons | `data/` |
| `translations/` | Translations, in Qt Linguist's format; | `po/` |
| `tests/` | Test suite (see above) | `test/` |
| `tools/` | Reference data generators for the differential tests, the archives that `tst_archive` opens, and the import of the original's translations | |

The original's long-running work (comparing, folder scanning) uses Python
generators on a cooperative scheduler, and this port keeps that design with
C++23 `std::generator` coroutines (`src/core/task.h`). Text panes are
`QPlainTextEdit`s with custom painting for chunk backgrounds, inline
highlights and sync points.

## Differences from the original

- Only the git plugin has been run against its real program here. The
  Mercurial, Subversion, Bazaar, Darcs and CVS plugins are complete ports
  whose parsing is tested against the original, but not with the tools
  themselves.
- A change with text on only one side (shown green) also has a button on the
  side without the text, in the action gutter, which removes the text from
  the other side. The change's outline widens around this button. In the
  original, its only button copies the text across.
- Version control actions that a system doesn't implement (e.g., push for
  Mercurial, Subversion and CVS) are disabled, where the original fails
  when they're used. Some faults of the original are fixed: refreshing part
  of the tree forgets stale states (e.g., "Staged" after unstaging), an
  untracked file stays unversioned when it's refreshed after saving, and
  the resolve prompt is shown once instead of repeatedly.
- The previous commit messages are kept in the settings instead of a
  separate `history.ini`.
- Archives are extracted to a temporary folder rather than mounted with
  gvfs (see "Archives"), so opening one takes as long as extracting it, and
  needs an archiving program. Archives are extracted for each comparison
  that opens them, where gvfs mounts each once. If Meld Qt is killed, its
  extracted archives are left in the temporary folder. Archives that a
  program can't extract completely (e.g., because of a member outside the
  archive's folder, which is never written) aren't opened.
- The bundled language definitions and colour schemes are GtkSourceView
  5.22.0's, rather than those installed on the system, and language
  definitions and schemes installed on the system or by the user aren't used.
- The current line highlight in dark schemes is a yellow tint, as in light
  schemes. The original's is near black, which can't be seen on dark backgrounds.
- Here-docs in shell, Perl and Fish always end at their markers. With
  GtkSourceView 5, those whose language definitions use duplicate group names
  for the marker (e.g., `<<'EOF'` in shell, and `<<EOF` in Perl and Fish) don't:
  the marker isn't highlighted, and the here-doc continues until an empty line.
  This keeps GtkSourceView 4's behaviour.
- Patches are highlighted with the comparisons' colour scheme, rather than
  GtkSourceView's default one.
- Colours in schemes can't use X11 colour names that aren't CSS colour
  names (e.g., `#grey50`), and regular expressions with backreferences
  aren't rejected; the bundled files do neither.
- In git repositories, the version control view can compare any two
  revisions, or a revision with the working copy (see "Comparing
  revisions"), which the original can't do.
- Files with merge conflict markers are opened as 3-way merges (see
  "Resolving conflict markers"), which the original can't do.
- Files and folders can be compared with a patch applied (see "Comparing
  with a patch"), which the original can't do.
- The original always opens comparisons in the running Meld; this is
  optional here, and off by default (see "Single-instance mode").
- The language can be chosen in Preferences (see "Translations"); the
  original always uses the system's.
- Settings are stored with `QSettings`, in `~/.config/meld-qt/meld-qt.ini` on
  Linux, instead of GSettings. Recent comparisons are kept there too. Saved
  `.meldcmp` comparison files from the original can still be opened with
  `--comparison-file`.
