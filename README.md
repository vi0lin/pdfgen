# pdfgen — C++ port of pdfgen.py

Generates `DIR/Bewerbung.pdf` from `DIR/{sender.txt, receiver.txt, text.txt}`,
writing the PDF file format directly. No PDF library needed; dependencies are
**zlib** (PNG decoding / Flate compression, preinstalled virtually everywhere)
and **libwebp** for WebP images (`sudo apt install libwebp-dev`; build with
`make NO_WEBP=1` to drop this dependency — WebP files then fail with a clear
message instead).

## Build & run

CMake (primary, cross-platform):

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/pdfgen --path /path/to/bewerbung
./build/pdfgen dir1 dir2 dir3    # multiple paths now actually work
```

Dependency resolution is tiered and automatic — for both zlib and libwebp it
tries the system's CMake config package, then pkg-config (libwebp only), then
falls back to building the library from source via FetchContent. Useful knobs:

```
-DPDFGEN_FORCE_FETCH=ON     # skip system libs, build zlib+libwebp from source
                            # (recommended for mingw-w64 / Android NDK cross
                            # builds whose sysroot ships neither)
-DPDFGEN_WITH_WEBP=OFF      # drop the libwebp dependency entirely; .webp
                            # files then fail with a clear message
```

Why targets instead of a plain `-lwebp`: since libwebp 1.3 the static library
needs `-lsharpyuv -lm` after it in exactly that order, which is the usual
cause of "undefined reference to SharpYuv..." link failures. The imported /
FetchContent CMake targets carry those transitive dependencies automatically
on every platform.

### Embedding into another CMake project

`pdfgen_core` is a proper library target (the CLI is a thin wrapper), so from
a parent project it's just:

```cmake
add_subdirectory(external/pdfgen EXCLUDE_FROM_ALL)   # or FetchContent
target_link_libraries(my_app PRIVATE pdfgen::core)
```

and in code: include `markup.h` / `flowables.h`, build a `pdf::Writer` +
`pdf::Document`, add flowables, `save()` — see `src/main.cpp` for the
complete 40-line recipe.

### Sending the PDF by mail

`src/gmail_send.c` sends mail through libcurl (`sudo apt install
libcurl4-openssl-dev`; CMake links it via `find_package(CURL)` +
`CURL::libcurl`). Being a C module used from C++, its functions are declared
`extern "C"` in `gmail_send.h` — include that header from C++ code, never the
`.c` file (including the `.c` compiles a second, name-mangled copy).

Credentials are compile-time constants — no environment variables. Either
edit `FROM_ADDR` / `APP_PASSWORD` at the top of `gmail_send.c`, or inject
them at configure time:

```
cmake -B build -DPDFGEN_MAIL_FROM=me@gmail.com \
               -DPDFGEN_MAIL_APP_PASSWORD=xxxxxxxxxxxxxxxx
./build/pdfgen --mail /pfad/zur/bewerbung    # generates + mails the PDF
```

`send_test_mail(path)` takes the attachment path as a parameter (NULL = no
attachment); edit its `to[]` / `cc[]` arrays for your recipients — the counts
passed to `send_mail` must match the array sizes. From C++, the header also
offers a `std::string`/`std::vector` wrapper:

```cpp
pdfgen_mail::send("me@gmail.com", "apppassword",
                  {"a@x.de", "b@y.de"}, {},          // to, cc
                  "Bewerbung", "Anbei mein PDF.",
                  {dir + "/Bewerbung.pdf"});
```

Gmail requires an *app password* (Google account → Security → 2-step
verification → App passwords); regular passwords are rejected for SMTP.

A plain `Makefile` is also kept for quick Linux builds
(`make`, optionally `make NO_WEBP=1`).

## Architecture — why the parser is no longer nested

The Python version chained `parse_images → parse_headings → parse_datum`
*inside each other*, so every tag type had to know about every other one
(and `parse_datum` accidentally returned a variable from the wrong scope).
The C++ version replaces that with a flat pipeline of layers that only ever
call **downward**:

```
main.cpp        CLI, reads the three txt files, assembles the document
   │
markup.h/.cpp   STAGE 0  expandIncludes():  [datei.txt] → file contents
                STAGE 1  tokenize():        text → flat vector<Token>
                STAGE 2  buildFlowables():  tokens → layout objects
   │                     (Text | Heading | Image | ParagraphEnd — no recursion;
   │                      a new tag = one new Token kind + one case per stage)
   │
flowables.h/.cpp  layout engine (≈ platypus): Paragraph (word wrap,
   │              justification, <b>/<i>/<u>/<br/>), Spacer, ImageFlow,
   │              TwoColumnTable, Document (page breaking + paragraph
   │              splitting across pages)
   │
metrics.h/.cpp    Helvetica advance widths (Adobe AFM) for measuring text
image.h/.cpp      JPEG (embedded verbatim), PNG (full decode incl. palette
   │              & alpha → /SMask), WebP (via libwebp: lossy VP8, lossless
   │              VP8L, alpha → /SMask), BMP
   │
pdfwriter.h/.cpp  raw PDF 1.4: objects, xref, pages, content streams, fonts
```

A complete, self-demonstrating syntax reference ships in `Syntax/Syntax.txt`
(with its demo assets): every tag is explained AND shown live — render it
with `pdfgen Syntax.txt` and read the resulting `Syntax.pdf` side by side
with the source. Literal square brackets in text are written `\[` `\]`.

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
| `\c embedded` … `\\c` | conditional block: the lines in between render only when the condition holds (`embedded` / `!embedded`, nestable). `condition=` stays available on every tag. |

**Styles / mementos / forward** (works for the parameters of EVERY element —
tables incl. legacy `[table]`, images, `\r`, `\m`, `\e`, `\p`, project tags):
`name=Stil1` stores the element's EFFECTIVE parameter set; `style=Stil1`
loads it at exactly that position in the list (parameters before `style=`
are overridden by the style, parameters after it override the style);
`forward` additionally merges the effective values into a per-element-type
cache so every FOLLOWING element of that type inherits them automatically
(accumulative); `clean` ignores that cache and clears it. Merge order:
forward cache → params before `style=` → style values → params after
(later wins). A document-leading `[margins]`/`\m` line still sets the base
page layout; a margins tag after any content now always acts as an in-flow
change from that point (this also fixed a subtle bug where a mid-document
margins tag was hoisted to the whole document). Styles live per source file;
embedded/included files have their own scope.

**Loops over data files:** `\loop werte.txt, D=\n\n` … `\\loop` repeats the
body once per record. Records are separated by the delimiter `D` (escapes
`\n` `\t` `\\`; default `\n\n` = blank line — with `D=\n` every line is its
own record). Within a record the LINES are the fields: `:0` inserts line 0,
`:1:3` lines 1–3 (as real lines), `:4:` line 4 up to the record's end. A `:`
only counts as a placeholder when preceded by start-of-line, whitespace or
`|`, so `widths=2:1` stays untouched. Multi-line values drop naturally into
block-table cells and full-width rows; `@` inside the body gives one
keep-together group per record. Data files are read verbatim; out-of-range
fields warn once per loop and insert nothing. JSON input (`\jloop`) is
planned but deliberately postponed.

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
literal, so ordinary colons in prose are safe.

**Block tables (`\t` … `\\t`):** a `|`-row may span several physical lines —
it ends at the first line that ENDS with `|`, and line breaks inside a cell
become real breaks (`Test / Adresse / Telefonnummer` in one cell). Any text
WITHOUT a leading `|` becomes a row with one cell spanning ALL columns (line
breaks allowed); the next `|`-line starts the next ordinary row
automatically. `@` between rows groups them (never split across pages),
`[row...]`, separators and explicit `|||` colspans work as usual.

## Projects: several PDFs and mails from one source

A source file may define multiple documents and e-mails. Control tags stand
on their own line (a tag may wrap across lines until its closing bracket):

```
[pdfgen, file=Bewerbung.pdf]        <- starts document 1, content follows
...content...
[pdfgen=Lebenslauf.txt, file=Lebenslauf.pdf]   <- document 2 from another file
[mail=Haupt, to=a@x.de b@y.de, cc=c@z.de,
attachment=Bewerbung.pdf, test=ich@x.de]
Betreffzeile                        <- first line after the tag = Subject
Mailtext ...                        <- rest until the next tag = body
[attach, mail=Haupt, file=Lebenslauf.pdf]
```

Address lists are space-separated. Without any control tags the whole file is
one document: directories keep producing `DIR/Bewerbung.pdf`, and
`pdfgen Example.txt` produces `Example.pdf` next to the source.

```
pdfgen DIR                          # reads DIR/text.txt
pdfgen a.txt b.txt                  # several sources
pdfgen ... --mail                   # send ALL defined mails
pdfgen ... --mail Haupt Zweite      # send only these
pdfgen ... --mail --mail-test       # send to each mail's test= address
                                    # (subject prefixed "[TEST] ")
```

## Page breaks, comments, row spacing

`[newpage]` (or `[neueseite]`) starts a new page; two in a row produce a
deliberate blank page. `[newpage, no_blank]` skips the break when the current
page is still empty, so it never creates a fully blank page. Lines beginning with `//`
(after optional indentation) are comments and disappear entirely — mid-line
`//` (e.g. in URLs) is left alone. Table row spacing:
`[table, rowspacing=2*mm, ...]` sets the default gap between all rows, and a
`[row, spacing=8*mm]` line between two rows overrides the gap at exactly that
position (e.g. extra air above a sum row). Grid lines close both rows around
a gap; page splitting accounts for the gaps.

**Controlling table page breaks:** `[row, pagebreak]` between two rows forces
the table onto a new page exactly there (header repeats), even when the rest
would still have fit — combinable with `spacing=`. `[group] ... [/group]`
around several rows keeps them together: if the natural break would fall
inside, the whole group moves to the next page ([gruppe] works too).

## Header and footer on every page

```
[header]
[table, widths=1:1]
| <b>[document]</b> | [pages] |
| :--- | ---: |
[/header]
[bottom]
...any markup...
[/bottom]
```

Both blocks render on every page of the document (not on merged foreign
pages), may contain any markup with flexible height, and shrink the content
area: the header starts at the top margin, the footer ends at the bottom
margin, so the visual page margins stay intact. Without the blocks nothing
changes. `[document]` is the output file name, `[pages]` becomes
`current/total` — the total is computed with an automatic two-pass render.

## Embedding sources and conditional rendering

`[embed=Lebenslauf.txt]` inlines the file into the current document's flow
with the `embedded` condition active; `[embed=..., newpage]` starts it on a
fresh page, `[embed=..., newpage_no_blank]` does the same but skips the break
when the current page is still empty (never a blank page), `[embed=..., keepmargins]` drops the file's own `[margins]` tags
(default: they apply from that point and the parent margins are restored
after the block). `[margins, ...]` may generally appear mid-document and
changes the margins from that point on.

Every `[...]` tag accepts `condition=embedded` or `condition=!embedded` —
tables (a failing `[table...]` hides the whole following table), images,
includes, `[pdf=...]`, `[embed]`, and the project tags `[pdfgen]` / `[mail]`
(a failing one skips its whole section) / `[attach]`. So
`[sender.txt, condition=!embedded]` shows the sender only in the standalone
`[pdfgen=Lebenslauf.txt, file=Lebenslauf.pdf]` rendering and hides it when
the same file is embedded. `[pdfgen=..., include]` child contexts also count
as embedded.

## Page margins

Defaults are 1.4 cm left, 1.0 cm right, 0.5 cm top/bottom. Override them per
document — unset values keep their defaults:

```
[pdfgen, file=X.pdf, margins=3*cm:3*cm:2*cm:1*cm]   # left:right:top:bottom
[pdfgen, file=Y.pdf, left=2*cm, top=1*cm]           # single values
```

or with a standalone line inside the document content (also works for the
implicit document without a [pdfgen] tag, and inside [pdfgen, include]
sources; it wins over the tag options):

```
[margins, left=2*cm, top=3*cm]
[margins=2*cm:2*cm:1*cm:1*cm]
```

German aliases: `links rechts oben unten raender`. Units: cm mm inch pica pt.

## Merging PDF pages

Inside document content:

```
[pdf=Site3.pdf]                     # insert all pages of an existing PDF here
[pdfgen=Site3.txt, include]         # render Site3.txt and insert its pages
```

The current page ends, the foreign pages follow, and the text continues on a
fresh page — which is created lazily, so chained merges or a merge at the end
of the document never leave stray pages carrying only header/footer or
background. A deliberately created empty page directly before a merge (e.g.
via `[newpage]`) is kept by default; `[pdf=X.pdf, newpage_no_blank]` and
`[pdfgen=X.txt, include, newpage_no_blank]` discard it, exactly like the
option of the same name on `[embed]`. The importer reads classic xref tables AND modern files with
cross-reference streams + object streams (incl. their predictors); page
attributes inherited through the page tree are resolved. Encrypted PDFs are
rejected with a clear message. `include` recursion is depth-limited.

## Supported text syntax inside a document

| Syntax | Meaning |
|---|---|
| blank line | paragraph break; every *additional* blank line adds one more gap unit, so stacked blank lines raise the spacing evenly — including at the top of the file (extra space above the first heading) |
| single newline | hard line break (`<br/>`) |
| `# Titel` … `##### Titel` | heading level 1–5 (line must start with `#`) |
| `[date]` / `[datum]` | today's date, default `DD.MM.YYYY` |
| `[date, format="WEEKDAY, D. MONTH YYYY", lang=de]` | formatted date. Tokens: `YYYY YY MM M DD D`, `MONTH`/`MON` (full/short month name), `WEEKDAY`/`WD`. `lang` = `en de ru pl es` (ru/pl month names in the genitive, as used in dates). Quote the format if it contains commas |
| `[date, modified=1, ...]` | fixed date: if the output PDF already exists its file creation date is used (Linux `statx` birth time, Windows creation time, fallback mtime), otherwise today — so the date freezes after the first generation. Same formatting options |
| `[bild.png]` | image, default 5×5 cm |
| `[bild.png, width=3*cm]` | proportional height (units: cm, mm, inch, pica, pt) |
| `[bild.png, height=40*mm]` | proportional width |
| `[bild.png, width=3*cm, height=2*cm]` | exact size (may distort) |
| `<b> <i> <u> </b> </i> </u> <br/>` | inline styling |
| `[bild.png, width=3*cm, float=left]` | the following paragraph wraps around the image (also `float=right`) |
| `[bild.png, width=5*cm, x=11*cm, y=3*cm]` | absolute position from the LEFT/TOP page edge on the current page; the image floats over the text and occupies no flow space |
| `[bild.png, ..., dx=3*cm, dy=-5*mm]` | nudges a flow image visually (dx right, dy down, negatives allowed); the occupied layout space stays unchanged |
| `[bild.webp, width=5*cm, dpi=150]` | decoded images (PNG/WebP/BMP) are downscaled to the display size at this resolution before embedding — drastically smaller PDFs (150 dpi is fine on screen and in normal prints, 300 dpi is print-perfect; alias `aufloesung`). Requires `width=` or `height=`. JPEGs are embedded verbatim and never resampled. |
| `[bild.png, ..., layer=back]` | painted underneath the text (above a page background) — watermarks, letterhead art; default `layer=front` |
| `\| Kopf A \| Kopf B \|` | table row (markdown pipes); cells support inline styling |
| `\| :--- \| :---: \| ---: \|` | separator: makes the row(s) above a bold header and sets column alignment (left/center/right) |
| `[absender.txt]` | include a text file (recursively). Alone on a line: block include, may contain headings/tables/images. Inline (e.g. in a cell): newlines become `<br/>` |
| `[table, widths=4*cm:2:1, padding=2*mm:1*mm, spacing=0.5*cm, indent=1*cm, width=12*cm, grid=off]` | options for the table starting on the next line (German keys work too: `tabelle, breiten, innenabstand, abstand, einzug, breite, gitter`) |

Table options: `widths` mixes fixed columns (values with a unit) and relative
weights (bare numbers); `padding=h:v` is the inner cell padding; `spacing`
the outer gap above and below; `indent` shifts the table right; `width` caps
the total table width; `grid=off` hides all lines. Images work inside cells --
`float=left/right` wraps the cell text around them, an image alone in a cell
with `float=right` is right-aligned, and images wider than their column are
scaled down to fit.

**The letterhead is no longer inserted automatically.** Compose it yourself
at the top of `text.txt`:

```
[table, grid=off, padding=0:0]
| [sender.txt] | [receiver.txt] |
```

**Table borders** are OFF by default. The *first* separator row makes the
rows above it a bold header, sets column alignment, and draws a rule at its
position; every *further* separator row just draws a rule there — that's how
you underline a header and put a line above a sum row with plain markdown.
`grid=on` draws the full grid (the old behavior), `frame=on` only the outer
box, and `linewidth=1.2*pt` / `linecolor=#aa2222` style the lines (German
aliases: `rahmen`, `linienbreite`, `linienfarbe`).

**Unicode text (Cyrillic, Polish, …):** the built-in Helvetica fonts only
cover CP1252, so runs containing characters outside it (e.g. Russian month
names, ą ć ę ł ń ś ź ż) are automatically shaped with an embedded DejaVu Sans
(searched in `$PDFGEN_FONT`, `./fonts/`, and the usual system paths;
Liberation Sans and Arial are fallbacks). The font is embedded as a
Type0/CIDFontType2 with a ToUnicode CMap, so copy-paste and text extraction
work. It is only embedded when actually used; documents that use it grow by
the compressed font size (~400 KB — subsetting would be the next
optimization). Lines containing embedded-font runs are set at natural word
spacing instead of justified. If no Unicode font is found, those characters
fall back to `?` with a warning.

**Column spans:** pipes directly after one another extend the cell before
them — `| Titel über zwei Spalten || C |` makes the first cell span two
columns, `| Gesamt |||` spans all three. Only zero-width segments count:
`| |` with a space stays an ordinary empty cell. Spanned cells take the
alignment of their first column, grid lines are drawn per row so spans are
never cut through, and header repetition across page breaks keeps the spans.

Headings work inside table cells too: `| # Titel | ... |` renders large and
bold, keeps the column alignment, and the `#` never appears in the PDF —
handy for a letterhead with the title left and `[date...]` right-aligned.

Cell text uses the body metrics (11 pt, 16 pt leading, left-aligned). With
`spacing`, `padding`, `indent` and `rowspacing` set to 0, table rows fall on
exactly the same 16 pt rhythm as paragraph lines — including the transition
from a preceding paragraph, because a table with its own `spacing=` value
suppresses the default gap to the text above it. Dimension values may also
glue number and unit together (`0mm`, `2mm`, `0.5cm` — same as `2*mm`), and
invalid values now produce a warning instead of silently keeping defaults.

Tables span the full text width with column widths proportional to their
content; long tables split across pages and repeat the header row. Floats
break the paragraph's first lines to a narrow box beside the image and
continue full-width below it — justification stays correct in both zones
because every line carries its own box geometry (the Python version only
approximated this by counting characters).

Body text is 11 pt Helvetica, 16 pt leading, **justified** (like the Python
`Body` style). Margins are identical: 1.4 cm left, 1.0 cm right, 0.5 cm
top/bottom, A4. Sender/receiver appear side by side when both files exist.

## Differences from the Python version (deliberate)

* `[datum]` inserts **today's** date inline (the Python code hard-coded
  `01.01.1994` and put it in its own paragraph). Change it in one place:
  `opt.datumText` in `main.cpp`.
* Multiple `--path` arguments are handled (was a TODO in Python).
* Images: JPEG/PNG/WebP/BMP are supported (WebP incl. lossy, lossless and
  alpha; animated WebP is rejected). GIF produces a warning and is skipped —
  convert with e.g. `convert bild.gif bild.png`. Interlaced or 16-bit PNGs are
  also rejected with a clear message.
* Fonts are the built-in Helvetica family with WinAnsi encoding — German
  umlauts, ß, €, „quotes“ and dashes all work; text outside CP1252 becomes `?`.
  Embedding custom TTFs would be the next feature to add in `pdfwriter.cpp`.
* Page background: `doc.setBackground("#f5eee4")` in `main.cpp` replicates the
  commented-out colored background (default is plain white, no rect drawn).

## Extending

* **New tag type** → add a `Token::Kind`, recognize it in `tokenize()`,
  map it in `buildFlowables()`. Two switch cases, no other file changes.
* **Tables & floats** live in `TableFlow` / `FloatFlow` (`flowables.cpp`);
  the per-line box mechanism (`Paragraph::setLineBoxes`) is reusable for any
  future shaped-text feature (e.g. drop caps, two floats).
* **New flowable** (e.g. horizontal rule) → subclass `Flowable`, implement
  `wrap()` + `draw()`; implement `splitTop()` only if it may straddle pages.
* **Text-wrap around images / gradients** → both are canvas-level tricks in
  the Python file; the equivalent here is emitting extra operators in a
  `Flowable::draw()` (see `ImageFlow::draw` for the pattern).
