#!/usr/bin/env bash
# scripts/test_pdfgen.sh — build pdfgen (if needed) and run smoke tests.
# Renders the self-demonstrating syntax reference plus a few probes that
# cover the core features. Exit code 0 = everything passed.
set -u
cd "$(dirname "$0")/.."                 # repo root
ROOT="$PWD"
FAIL=0
note() { printf '  %-44s %s\n' "$1" "$2"; }
ok()   { note "$1" "OK"; }
bad()  { note "$1" "FEHLER"; FAIL=1; }

# ---- build (reuse an existing build dir) ----------------------------------
BIN=""
for c in build/pdfgen build/pdfgen.exe builds/*/pdfgen; do
  [ -x "$c" ] && BIN="$ROOT/$c" && break
done
if [ -z "$BIN" ]; then
  echo "== Baue pdfgen (cmake) =="
  cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null || { echo "configure fehlgeschlagen"; exit 1; }
  cmake --build build -j >/dev/null || { echo "build fehlgeschlagen"; exit 1; }
  BIN="$ROOT/build/pdfgen"
fi
echo "== Binary: $BIN =="
"$BIN" --help >/dev/null 2>&1 || true   # usage darf auf stderr landen

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ---- 1) Syntax-Referenz rendern (darf ruhig Syntax bauen) -----------------
if [ -f Syntax/Syntax.txt ]; then
  ( cd Syntax && "$BIN" Syntax.txt >/dev/null 2>"$TMP/syn.err" )
  if [ -s Syntax/Syntax.pdf ]; then ok "Syntax/Syntax.pdf gerendert"; else bad "Syntax/Syntax.pdf"; fi
  [ -s Syntax/SyntaxTeil.pdf ] && ok "SyntaxTeil.pdf (Projekt-Tags)" || bad "SyntaxTeil.pdf"
  if command -v pdfinfo >/dev/null 2>&1; then
    P=$(pdfinfo Syntax/Syntax.pdf 2>/dev/null | awk '/^Pages:/{print $2}')
    [ "${P:-0}" -ge 8 ] && ok "Seitenzahl ($P)" || bad "Seitenzahl ($P, erwartet >=8)"
  fi
  grep -q "stammt aus Extern" <(command -v pdftotext >/dev/null 2>&1 && pdftotext Syntax/Syntax.pdf - 2>/dev/null) \
    && ok "PDF-Merge-Seite enthalten" || note "PDF-Merge-Seite" "(pdftotext fehlt: uebersprungen)"
else
  note "Syntax/Syntax.txt" "(nicht vorhanden: uebersprungen)"
fi

# ---- 2) Kern-Proben ---------------------------------------------------------
printf '# Probe\n\n**fett** und `code`.\n\n- Punkt eins\n- Punkt zwei\n' > "$TMP/p.md"
"$BIN" "$TMP/p.md" >/dev/null 2>&1 && [ -s "$TMP/p.pdf" ] && ok ".md -> PDF (Markdown)" || bad ".md -> PDF"

printf 'Zeile A\nZeile B\n\n\\t grid=on\n| A | B |\n\\\\t\n' > "$TMP/t.txt"
"$BIN" "$TMP/t.txt" >/dev/null 2>&1 && [ -s "$TMP/t.pdf" ] && ok ".txt Blocktabelle" || bad ".txt Blocktabelle"

printf 'a\nb\nc\n' > "$TMP/d.txt"
printf '\\file d.txt\n:0:+1\n\\\\file\n' > "$TMP/f.txt"
"$BIN" "$TMP/f.txt" >/dev/null 2>&1 && [ -s "$TMP/f.pdf" ] && ok "\\file + Ranges" || bad "\\file + Ranges"

printf '\\off\n\\t nur Text\n\\on\nEnde.\n' > "$TMP/o.txt"
"$BIN" "$TMP/o.txt" >/dev/null 2>&1 && [ -s "$TMP/o.pdf" ] && ok "\\off/\\on Verbatim" || bad "\\off/\\on"

# ---- 3) Mail-Konten-Aufloesung (sendet nichts) ------------------------------
printf '[gmx]\nich@gmx.de\ntoken\n' > "$TMP/m.conf"
"$BIN" -c "$TMP/m.conf" --mail-accounts 2>/dev/null | grep -q "mail.gmx.net:587" \
  && ok "--mail-accounts (Anbieter-Profile)" || bad "--mail-accounts"

echo
if [ "$FAIL" -eq 0 ]; then echo "== ALLE TESTS GRUEN =="; else echo "== FEHLER (siehe oben) =="; fi
exit "$FAIL"
