# pdfgen — C++

Turns plain `.txt` (pdfgen syntax + GitHub markdown) and `.md` files into
PDFs, writing the PDF file format directly — no PDF library. Dependencies:
**zlib** (preinstalled virtually everywhere), **libwebp** for WebP images,
**libcurl** for mail. One source file can produce several PDFs, merge
existing ones and e-mail the results.

---

## Quick start

### Get the source

```sh
git clone --recurse-submodules https://github.com/vi0lin/pdfgen
cd pdfgen
# already cloned without submodules? fetch them afterwards:
git submodule update --init
```

The only submodule is `external/curl`, a fallback for systems without a
libcurl dev package (Windows / cross builds) — with
`libcurl4-openssl-dev` installed, a plain `git clone` works too.

### Build

```sh
# CMake (primary, cross-platform)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# plain Makefile (quick Linux builds; make NO_WEBP=1 drops libwebp)
make
```

With [build.sh](https://github.com/vi0lin/build.sh) (multi-machine builds,
this repo ships a ready `build.sh.conf`):

```sh
git clone https://github.com/vi0lin/build.sh && cd build.sh
./build.sh --add-to-path          # one-time install
cd /path/to/pdfgen                # the clone from above
build.sh deb@local                # configure + build here
build.sh --run deb@local          # build and start
build.sh exe@windows deb@local    # several targets/machines in parallel
```

Smoke test (builds if needed, renders the syntax reference and a few probes):

```sh
scripts/test_pdfgen.sh
```

Dependency resolution is tiered and automatic — for zlib and libwebp CMake
tries the system config package, then pkg-config, then builds from source
via FetchContent. Knobs: `-DPDFGEN_FORCE_FETCH=ON` (skip system libs —
recommended for mingw-w64 / Android NDK cross builds), `-DPDFGEN_WITH_WEBP=OFF`
(drop libwebp; .webp files then fail with a clear message). Why targets
instead of a plain `-lwebp`: since libwebp 1.3 the static library needs
`-lsharpyuv -lm` after it in exactly that order — the imported/FetchContent
targets carry those transitive dependencies on every platform.

### Use from the command line

```sh
pdfgen brief.txt                 # -> brief.pdf (next to the source)
pdfgen notes.md                  # markdown file -> notes.pdf
pdfgen bewerbung/                # directory: reads text.txt (or text.md)
pdfgen a.txt b/ c.md             # several sources in one run
pdfgen . --mail                  # generate, then send the [mail=...] definitions
pdfgen . --mail Haupt            # ... only the mail named "Haupt"
pdfgen . --mail-test             # send to each mail's test address instead
```

| Flag | Meaning |
|---|---|
| `-c, --config FILE` | sender config (default: `pdfgen.conf` next to the binary) |
| `--sender ADDR TOKEN [PROV\|HOST:PORT[:ssl]]` | add a sender account; the provider is inferred from the mail domain (gmail, outlook, gmx, web.de, t-online, yahoo, icloud) or given explicitly |
| `--mail-accounts` | list the resolved sender accounts (host/port/TLS/auth) and exit |
| `--default-sender ADDR` / `--test-to ADDR` | pick the default account / add test recipients |
| `-v, --verbose` | debug messages |

`pdfgen.exe` on Windows behaves identically (`pdfgen.exe brief.txt`, paths
with `\` or `/`). Exit code 0 = all sources succeeded — script-friendly.

### Use from bash

```sh
#!/usr/bin/env bash
set -e
for dir in kunden/*/; do             # one application folder per customer
  pdfgen "$dir"                      # writes $dir/Bewerbung.pdf
done
pdfgen rundschreiben.md --mail       # render + send in one go
```

### Embed in C / C++

`pdfgen_core` is a proper library target; the CLI is a thin wrapper.

```cmake
add_subdirectory(external/pdfgen EXCLUDE_FROM_ALL)   # or FetchContent
target_link_libraries(my_app PRIVATE pdfgen::core)   # + pdfgen::mail if sending
```

```cpp
#include "project.h"      // high level: source -> PDFs (+ mail definitions)
#include "mail_config.h"  // runtime sender accounts (no compile-time creds)

std::vector<project::MailSpec> mails;
std::vector<std::string> warnings;
project::processSource("bewerbung/", mails, warnings);   // same as the CLI

pdfgen::MailConfig cfg;                                   // e.g. from your UI
pdfgen::parseMailConfig(confText, cfg, warnings);
pdfgen::setMailConfig(cfg);
```

Host integration (logging, file access, paths) goes through `pdfgen_host.h` —
the library never printf()s or opens files behind your back. For low-level
use (own layout, no markup) include `flowables.h`/`pdfwriter.h`: build a
`pdf::Writer` + `pdf::Document`, add flowables, `save()` — `src/main.cpp` is
the complete recipe. `gmail_send.h` is a C module; include the header from
C++ (it is `extern "C"`), never the `.c` file.

**Learn the syntax by example:** `Syntax/Syntax.txt` is a complete,
self-demonstrating reference — every tag explained AND shown live. Render it
(`pdfgen Syntax.txt`) and read `Syntax.pdf` side by side with the source.

---

# Full reference

## Architecture — why the parser is no longer nested

The Python version chained `parse_images → parse_headings → parse_datum`
*inside each other*, so every tag type had to know about every other one
(and `parse_datum` accidentally returned a variable from the wrong scope).
The C++ version replaces that with a flat pipeline of stages that only ever
call **downward** — a new tag is one new Token kind plus one case per stage,
never a change to the other tags:

```
main.cpp          CLI; everything else lives in the library
   │
project.h/.cpp    sources -> documents: [pdfgen]/[mail]/[attach] sections,
   │              header/footer blocks, base margins, [embed=...] with
   │              conditions, two-pass rendering when [pages] is used
   │
markup.h/.cpp     the text pipeline, in order:
   │                processOnOff            \off/\on verbatim regions
   │                translateMarkdown       GitHub markdown -> internal form
   │                translateBackslashTags  \i /\t /\e ... -> bracket tags
   │                escapeLiteralBrackets   \[ \] -> sentinels
   │                applyStyles             style= / name= / forward / clean
   │                preprocess              // comments, \c blocks, condition=
   │                expandLoops             \loop / \file with :ranges
   │                expandIncludes          [datei.txt] / [datei.md]
   │                expandDates             [date, ...]
   │                tokenize                text -> flat vector<Token>
   │                buildFlowables          tokens -> layout objects
   │
flowables.h/.cpp  layout engine (≈ platypus): Paragraph (wrap, justification,
   │              <b>/<i>/<u>/<s>/<code>/<a=url>, hanging indents), Spacer,
   │              ImageFlow (float, absolute, dpi budget, back layer),
   │              TableFlow (spans, keep-groups, header repetition, splits),
   │              StackFlow (keep-together), HRuleFlow, Document (lazy pages,
   │              page breaking, paragraph splitting, header/footer decor)
   │
metrics / ttffont Helvetica+Courier advance widths; DejaVu embedding (Type0/
   │              CID + ToUnicode) for characters outside CP1252
image.h/.cpp      JPEG (verbatim), PNG (palette, alpha -> /SMask), WebP, BMP;
   │              box-filter downscaling for the dpi= budget
pdfimport.h/.cpp  merging foreign PDFs (classic xref AND xref streams/ObjStm)
pdfwriter.h/.cpp  raw PDF 1.4: objects, xref, pages, fonts, images, links
mail_config/      runtime sender accounts (pdfgen.conf, --sender, host app),
mail_dispatch     provider profiles, transport resolution -> gmail_send.c
pdfgen_host       logging/file/path seam between library and host program
```

## Backslash line-tag syntax

Every whole-line `[tag, params]` also exists as `\<letter-or-word>` at the
start of a line; parameters stay comma-separated, and a first parameter
without `=` is the tag's value. Old bracket tags keep working; inline tags
inside a text line (`[date...]`, `[datei.txt]`, images in cells) keep their
bracket form.

| new | equals |
| --- | --- |
| `\i foto.webp, width=3*cm` | `[foto.webp, width=3*cm]` |
| `\t grid=on, widths=2:1` … `\\t` | block-style table (see below) |
| `\r spacing=4*mm, pagebreak` | `[row, ...]` |
| `\g` / `\\g` | `[group]` / `[/group]` |
| `@` | group separator: closes the previous group and opens the next (works between table rows AND between ordinary flowables — keeps e.g. name + signature together on one page). Outside tables a group also ends automatically at the next structural element (heading, table, page break, margins change, merged PDF), so a trailing `@` can never swallow the rest of the document. |
| `\s 3` / `\s 5*mm` | vertical space: bare number = that many body text lines (16 pt each), with a unit = that dimension |
| `\n` / `\n no_blank` | `[newpage]` / `[newpage, no_blank]` |
| `\m left=2*cm` | `[margins, ...]` |
| `\h`…`\\h`, `\b`…`\\b` | header / bottom blocks |
| `\e Quelle.txt, newpage` | `[embed=Quelle.txt, newpage]` |
| `\p Site3.pdf, newpage_no_blank` | `[pdf=Site3.pdf, ...]` |
| `\pdfgen Quelle.txt, file=X.pdf` | `[pdfgen=Quelle.txt, file=X.pdf]` |
| `\mail Name, to=…` / `\attach mail=…, file=…` | `[mail=Name, …]` / `[attach, …]` |
| `\doc Name, format=pdf,html` | `[doc=Name, format=pdf,html]` -- same block as PDF and/or HTML |
| `\html Name` | `[html=Name]` = `[doc=Name, format=html]` |
| `\c embedded` … `\\c` | conditional block: the lines in between render only when the condition holds (`embedded` / `!embedded`, nestable). `condition=` stays available on every tag. |
| `\off` / `\on` | parsing off/on — see *verbatim mode* below |
| `\loop` / `\file` | repeat over data files — see below |
| `\mloop` / `\jloop` / `\jload` | mail merge over text/JSON lists — see *Mail merge* below |
| `\[` `\]` | literal square brackets, never interpreted anywhere |

**Styles / mementos / forward** (works for the parameters of EVERY element —
tables incl. legacy `[table]`, images, `\r`, `\m`, `\e`, `\p`, project tags):
`name=Stil1` stores the element's EFFECTIVE parameter set; `style=Stil1`
loads it at exactly that position in the list (parameters before `style=`
are overridden by the style, parameters after it override the style);
`forward` additionally merges the effective values into a per-element-type
cache so every FOLLOWING element of that type inherits them automatically
(accumulative); `clean` ignores that cache and clears it. Merge order:
forward cache → params before `style=` → style values → params after
(later wins). A document-leading `[margins]`/`\m` line sets the base page
layout; a margins tag after any content acts as an in-flow change from that
point. Styles live per source file; embedded/included files have their own
scope.

**Loops over data files:** `\loop werte.txt, D=\n\n` … `\\loop` repeats the
body once per record. Records are separated by the delimiter `D` (escapes
`\n` `\t` `\\`; default `\n\n` = blank line — with `D=\n` every line is its
own record). Within a record the LINES are the fields. Multi-line values
drop naturally into block-table cells and full-width rows; `@` inside the
body gives one keep-together group per record. Data files are read verbatim.

**`\file` and the range syntax:** `\file datei.txt(, D=...)` … `\\file` is
`\loop` without iteration — one pass, the fields are ALL lines of the file
(global numbering). Both tags share one placeholder grammar
`:START(:END)?` driven by a cursor that sits after the last delivered line:
START = absolute number, `+N`/`-N` cursor-relative (backwards re-delivers
earlier lines), `.` = line 0, `$var`, or empty = cursor. END = absolute
number, `+N` = START+N inclusive (`:10:+4` → 10..14), `$var`, `$` = always
the file/record end, or empty = to the end — or to the next delimiter when
`D=` is set on the tag. `$name='<` / `$name='>` after a placeholder store its
first/last delivered line number for reuse via `:$name`. Parse failures stay
literal, so ordinary colons in prose are safe. Literal placeholder lines
(under `\off`) do not move the cursor.

**Parsing on/off (verbatim mode):** `\off` renders every following line as
literal text (tags, tables, placeholders stay visible) until `\on` at the
same level. As a line prefix the switch applies to exactly that line
(`\on # Titel` renders one real heading inside an off block) — unless the
rest opens a block: `\off \t …` makes the whole table literal until `\\t`.
Inside `\t`/`\loop`/`\file` a standalone switch lasts until the element
ends, then the previous state returns; `\on D=\n\n` lasts until the next
blank line (any delimiter string works, matched against whole lines).
`\on!`/`\off!` force the state through all open levels; later switches
still work.

**GitHub-flavored markdown:** works in .txt sources on top of the pdfgen
syntax, and whole `.md` files render directly (`pdfgen README.md` →
README.pdf; a directory may carry `text.md` instead of text.txt). Headings
and |-tables are shared syntax anyway. Supported: `**bold**`, `*italic*`,
`~~strike~~`, `` `inline code` `` (Courier), fenced ``` code blocks
(verbatim, indentation preserved), `-`/`*`/`1.` lists with 2-space nesting
and hanging indents, `- [ ]`/`- [x]` task boxes, `>` blockquotes, `---`
rules, `[text](url)` and `<https://…>` as CLICKABLE link annotations,
`![alt](img.png)` mapped onto the image tag, and backslash escapes for
markdown metacharacters. Only `.md` files use markdown line semantics
(single newlines are soft; a hard break needs two trailing spaces or a
trailing backslash) — `.txt` keeps pdfgen's rule that every newline is a
hard break. Structural markdown (lists, quotes, rules) is disabled inside
tables; `\off` shows markdown literally. The one behavior change in .txt:
a line of 3+ dashes/stars is now a rule, and `- ` at line start begins a
list — escape with `\-`/`\*` or `\off` when literal text is wanted.

**Block tables (`\t` … `\\t`):** a `|`-row may span several physical lines —
it ends at the first line that ENDS with `|`, and line breaks inside a cell
become real breaks. Any text WITHOUT a leading `|` becomes a row with one
cell spanning ALL columns (line breaks allowed); the next `|`-line starts
the next ordinary row automatically. `@` between rows groups them (never
split across pages), `[row...]`, separators and explicit `|||` colspans work
as usual.

## Projects: several PDFs and mails from one source

`\pdfgen Quelle.txt, file=Name.pdf` ends the current document and starts the
next one (without a source the following lines are its content; `margins=…`
sets its base layout). `\mail Name, to=a@x b@y, cc=…, attachment=…, test=…`
defines an e-mail: the first line after it is the subject, the rest up to
the next tag the body; `\attach mail=Name, file=X.pdf` adds attachments.
Without any project tags: a directory becomes `Bewerbung.pdf`, `pdfgen
Beispiel.txt` becomes `Beispiel.pdf`.

**Mail accounts are runtime configuration** (no compiled-in credentials):
`pdfgen.conf` next to the binary, a file given with `-c`, `--sender` on the
command line, or `setMailConfig()` from a host application. Sending always
speaks SMTP (IMAP/POP3 are *retrieval* protocols); what varies is the
provider profile and the auth mechanism, both built in:

```ini
[gmail default]              ; provider + optional "default"
ich@gmail.com                ; line 1: address
abcd efgh ijkl mnop          ; line 2: app password / token (spaces ignored)
[gmx]                        ; also: outlook/office365, web.de, t-online,
privat@gmx.de                ;   yahoo, icloud — host/port/TLS are built in
gmx-app-passwort
[smtp mail.firma.de:587]     ; any other server (587=STARTTLS; "... 465 ssl")
ich@firma.de
passwort
[gmail xoauth2]              ; flag xoauth2: the token line is an OAuth2
oauth@gmail.com              ;   bearer (XOAUTH2) instead of a password
ya29....
[test]                       ; default recipients for --mail-test
test@example.com
```

A mail picks its account with `from=adresse` (default account otherwise).
`--sender ADDR TOKEN` infers the provider from the mail domain; an optional
third value overrides it (a provider name or `HOST:PORT[:ssl]`).
`--mail-accounts` lists the resolved accounts without sending. In the C API
the auth mechanism is an extensible enum (`smtp_auth`); credentials stay the
same two strings everywhere, so adding a mechanism never changes any input
path, and the old `send_mail()` entry point still works (it forwards to the
transport-parameterized `send_mail_ex()`).

## HTML output

The same token stream that feeds the PDF layout can be written as HTML
(`src/htmlwriter.cpp`): headings, paragraphs with `<b><i><u>`, tables with
spans, alignment, grid/frame and column widths, images (float, align,
width), lists, quotes, code blocks, rules, page breaks. Loops, conditions,
includes, groups, mloop/jloop -- everything from stage 1 -- works unchanged,
because it is resolved before the format is chosen.

```
\doc  Angebot, format=html        Angebot.html
\doc  Angebot, format=pdf,html    Angebot.pdf AND Angebot.html from one block
\html Angebot                     short for format=html
\pdfgen Quelle.txt, file=X.pdf    unchanged (format=pdf)
```

`\doc Name` with a bare name renders the inline content that follows; a
name ending in `.txt`/`.md` is a source file like with `\pdfgen`. `file=`
sets the output base name; the extension comes from the format. In loops
`format={{format}}` can come from the data.

Mails: `\mail Name, to=…, format=html` sends the body as
`multipart/alternative` (HTML + a text version made from the same tokens);
images are embedded as `cid:` parts. Without `format=` (or `format=text`)
the mail is plain text exactly as before. `body=Name` takes the content of
document `Name` as the mail body; if that document is also rendered as PDF,
the PDF is attached automatically:

```
\doc Angebot, format=pdf,html
...
\mail Angebot, to=kunde@example.org, format=html, body=Angebot
Ihr Angebot
```

In HTML, `[pdf=...]` becomes a link and `[pdfgen=..., include]` is skipped
(both with a warning); margins have no meaning there.

## Mail merge: one source, many PDFs and mails

A merge loop runs at PROJECT level (before documents and mails are split),
so its body may contain `\pdfgen`/`\mail`/`\attach` -- one PDF and one mail
per record. A runnable example ships in `examples/mailmerge/`
(`pdfgen examples/mailmerge --mail-list` renders everything and lists the
mails without sending; add real accounts and use `--mail` to send).

**Plain text records** -- `\mloop datei.txt, D=\n\n` … `\\mloop` uses the
SAME engine and `:N` range placeholders as `\loop` (records split by the
delimiter, lines are the fields):

```
\mloop kunden.txt, D=\n\n
\pdfgen, file=Brief-:0.pdf
# Anschreiben
Hallo :1 aus :2!
\mail M:0, to=:3, attachment=Brief-:0.pdf
Ihr Schreiben
Hallo :1, anbei Ihr PDF.
\\mloop
```

**JSON records** -- `\jloop datei.json` … `\\jloop` iterates a top-level
array. Placeholders: `$key`, nested paths `$kunde.ort`, `$posten[0].preis`,
`${name}` when letters follow directly, `$_` = the whole current record
(that is how an EMAIL-ONLY list `["a@x.de","b@y.de"]` works), `$#` = the
1-based position (ideal for unique file and mail names -- prefer `$#` over
`$name` in file names: `attachment=` splits on spaces for multi-attach).
Unknown `$names` stay literal, so `\loop`'s own `$var` mementos and ordinary
dollar signs survive.

```
\jloop kunden.json, filter=$status!=gesperrt
\pdfgen, file=Angebot-$#.pdf
# Angebot für $name
\t grid=on, widths=3:1
| Posten | Preis |
| :--- | ---: |
\jloop $posten
| $_.text | $_.preis € |
\\jloop
\\t
\c $rabatt
Als Stammkunde erhalten Sie $rabatt % Rabatt!
\\c
\mail Kunde$#, to=$email, attachment=Angebot-$#.pdf
Ihr Angebot, $name
Hallo $name, anbei Ihr persönliches Angebot.
\\jloop
```

**Conditions on data** -- `\c $pfad` … `\\c` renders only when the entry
exists and is non-empty (not null/false/0/""/[]); `\c !$pfad` when it is
missing; `\c $pfad=wert` / `\c $pfad!=wert` compare (the right side may hold
placeholders itself). `filter=` on `\jloop` takes the same expressions and
skips non-matching records. Conditions WITHOUT `$` pass through untouched to
the normal stage (`embedded` etc. keep working).

**Nested lists** -- `\jloop $pfad` … `\\jloop` iterates an array INSIDE the
current record (the invoice items above); `$_` is the element, `$_.feld` its
fields.

**Cross-file lookup** -- `\jload andere.json, match=KEY=WERT, as=name` finds
the first record whose KEY equals WERT (placeholders allowed) and exposes it
as `$name.feld`; not found = empty record, so `\c $name` / `\c !$name`
branch on it. The frame ends with the surrounding loop body (or `\\jload`).
The email-only list from above becomes a full campaign:

```
\jloop emails.json
\jload kunden.json, match=email=$_, as=kd
\c $kd
\pdfgen, file=Info-$#.pdf
# Info für $kd.name
\mail Info$#, to=$_
Kurzinfo für $kd.name
Hallo $kd.name!
\\c
\c !$kd
\mail Neu$#, to=$_
Willkommen!
Zu $_ liegt noch kein Kundendatensatz vor.
\\c
\\jloop
```

**Filling the lists from C/C++** -- the data format IS the interface: hand
pdfgen a JSON file (or write one from your UI) and call `processSource` as
usual. `jsondata.h` ships a tiny builder so hosts never hand-format JSON:

```cpp
#include "jsondata.h"
using jsondata::Value;

Value list = Value::array();
Value k = Value::object();
k.set("name", "Anna Muster").set("email", "anna@example.com");
Value posten = Value::array();
posten.push(Value::object().set("text", "Beratung").set("preis", 120));
k.set("posten", posten);
list.push(k);
pdfgen::writeFile(dir + "/kunden.json", list.toJson());   // then processSource
```

Plain-C hosts simply `snprintf` the JSON text. Parsing errors come back as
warnings with a line number; `--mail-list` is the dry run for the whole
campaign, `--mail-test` sends everything to the `[test]` addresses first.
Merge substitution is a PREPROCESSOR: it also fills placeholders inside
later `\off` blocks and data read by `\loop` stays verbatim.

## Page breaks, comments, row spacing

`[newpage]` / `\n` forces a page break; `no_blank` breaks only when the
current page is not empty (never creates a blank page); two in a row create
a deliberate blank page. Lines starting with `//` are comments. Blank lines
accumulate: every additional one adds a gap unit — including at the top of
the file (space above the first heading). `[row, spacing=6*mm]` between two
table rows changes exactly that gap; `[row, pagebreak]` forces the table to
break there (header rows repeat).

## Header and footer on every page

`\h` … `\\h` and `\b` … `\\b` (or `[header]`/`[bottom]` blocks) hold markup
drawn on every own page — tables, images, styling all work. `[document]`
inserts the output file name, `[pages]` the page as `n/total` (total
triggers an automatic second layout pass). Merged foreign pages count but
carry no decor.

## Embedding sources and conditional rendering

`\e Quelle.txt` (or `.md`) embeds another source into THIS document with the
`embedded` condition active. Options: `newpage`, `newpage_no_blank`,
`keepmargins` (ignore the child's own `\m` tags; otherwise they apply and
the parent layout returns afterwards). Conditions: `\c embedded … \\c`
blocks or `condition=embedded/!embedded` on any tag — the same file renders
differently standalone vs. embedded. `[pdfgen=Quelle.txt, include]` instead
RENDERS the source separately and merges its finished pages.

## Page margins

Defaults: left 1.4 cm, right 1.0 cm, top/bottom 0.5 cm. A `\m`/`[margins]`
line before any content sets the document's base layout; anywhere later it
changes the margins from that point on (left/right immediately, top/bottom
from the next page). Keys `left right top bottom` (German `links rechts oben
unten`), short form `margins=l:r:t:b`, units `cm mm inch pica pt`.

## Merging PDF pages

`\p Extern.pdf` inserts all pages of an existing PDF at this position —
classic xref tables AND modern xref streams/object streams are parsed;
encrypted files are rejected with a clear message. Pages after a merge are
created lazily, so chained merges or a merge at the document end never leave
stray header/footer-only pages; `newpage_no_blank` additionally discards a
deliberately created empty page right before the merge.

## Supported text syntax inside a document

| Syntax | Meaning |
|---|---|
| blank line | paragraph break; every *additional* blank line adds one more gap unit — including at the top of the file |
| single newline | hard line break (`.txt`; in `.md` lines flow together) |
| `# Titel` … `##### Titel` | heading level 1–5 |
| `[date]` / `[datum]` | today's date, default `DD.MM.YYYY` |
| `[date, format="WEEKDAY, D. MONTH YYYY", lang=de]` | formatted date. Tokens `YYYY YY MM M DD D MONTH MON WEEKDAY WD`; `lang` = `en de ru pl es` (ru/pl month names in the genitive). Quote formats containing commas |
| `[date, modified=1, ...]` | frozen date: the output PDF's creation time once it exists (Linux `statx` birth time, Windows creation time, fallback mtime), otherwise today |
| `[bild.png]` / `\i bild.png` | image at its NATURAL size (pixels at 96 dpi, the CSS/Word convention); wider than the text area is scaled down proportionally |
| `[bild.png, width=3*cm]` | `width`/`height` (one = proportional, both = exact), units `cm mm inch pica pt` |
| `[bild.png, ..., align=center]` | left/center/right (German `ausrichtung`, `mitte`…) |
| `[bild.png, ..., float=left]` | the following paragraph wraps around the image (also `right`) |
| `[bild.png, ..., x=11*cm, y=3*cm]` | absolute position from the LEFT/TOP page edge; floats over the text, occupies no flow space |
| `[bild.png, ..., dx=3*cm, dy=-5*mm]` | nudge the drawing only; the layout space stays |
| `[bild.webp, ..., dpi=150]` | decoded images (PNG/WebP/BMP) are downscaled to the display size at this resolution — drastically smaller PDFs (150 is fine on screen and normal prints, 300 print-perfect; alias `aufloesung`). Needs `width=` or `height=`. JPEGs are embedded verbatim, never resampled |
| `[bild.png, ..., layer=back]` | painted underneath the text (above a page background) — watermarks, letterhead art |
| `<b> <i> <u> <s> <code> <br/>` | inline styling (strike-through and monospace included) |
| `\| A \| B \|` + `\| :--- \| ---: \|` | table rows and separators — see tables below |
| `[absender.txt]` | include a file (recursively; `.md` includes get markdown line semantics). Alone on a line: block include. Inline: newlines become `<br/>` |
| `[table, widths=4*cm:2:1, padding=2*mm:1*mm, spacing=0.5*cm, rowspacing=1*mm, indent=1*cm, width=12*cm, grid=off, frame=on, linewidth=1.2*pt, linecolor=#aa2222]` | table options (German keys: `tabelle breiten innenabstand abstand zeilenabstand einzug breite gitter rahmen linienbreite linienfarbe`) |

**Tables:** borders are OFF by default. The *first* separator row makes the
rows above it a bold header (repeated across page breaks), sets column
alignment and draws a rule; every *further* separator just draws a rule —
that's how you underline a header and put a line above a sum row. `grid=on`
draws the full grid, `frame=on` only the outer box. `widths` mixes fixed
columns (values with units) and weights (bare numbers). Compose letterheads
yourself: `[table, grid=off, padding=0:0]` with `| [sender.txt] |
[receiver.txt] |`. With spacing/padding/indent/rowspacing at 0, table rows
fall exactly into the 16 pt body rhythm.

**Column spans:** pipes directly after one another extend the cell before
them — `| Titel über zwei || C |`, `| Gesamt |||` spans all columns. Only
zero-width segments count (`| |` stays an empty cell). Spans take the first
column's alignment; grid lines are drawn per row so spans are never cut
through. Headings work inside cells (`| # Titel |`) and keep the column
alignment. Images work inside cells, wrap cell text with `float`, and are
scaled down to fit their column.

**Unicode text (Cyrillic, Polish, …):** the built-in Helvetica/Courier only
cover CP1252; runs outside it are shaped with an embedded DejaVu Sans
(searched in `$PDFGEN_FONT`, `./fonts/`, system paths; Liberation/Arial as
fallbacks), embedded as Type0/CIDFontType2 with ToUnicode — copy-paste and
extraction work. Embedded only when used (~400 KB compressed). Lines with
embedded-font runs are set at natural word spacing instead of justified.

## Differences from the Python version (deliberate)

No letterhead automatism (compose it with a table), explicit image sizes
instead of magic scaling, conditions/styles/loops/markdown did not exist,
and the mail credentials moved from compile-time constants to runtime
configuration. The Python parser nesting is gone — see Architecture.

## Extending

A new whole-line tag: add one `Token::Kind`, parse it in `tokenize()`, build
its flowable in `buildFlowables()` — no other tag changes. A new flowable:
subclass `Flowable` (wrap/draw/clone). A new image format: one decoder in
`image.cpp` filling the same `ImageXObject`. A new mail provider: one line
in the `kProviders` table in `mail_config.cpp`. A new auth mechanism: append
one `smtp_auth` value and one `case` in `gmail_send.c` — no signature or
input format changes. Every header carries a `PDFGEN_*_API` number checked
by `#error` guards, so mixed-version source trees fail loudly at compile
time instead of misbehaving.
