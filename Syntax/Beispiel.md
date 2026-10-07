# GFM-Showcase

Dies ist **fett**, *kursiv*, ~~durchgestrichen~~ und `inline code`.
Diese Zeile gehört noch zum selben Absatz (weiche Umbrüche in .md).
Hier endet er mit zwei Spaces  
→ harter Umbruch davor.

## Listen

- Erster Punkt mit längerem Text, der umbrechen muss, damit man den hängenden Einzug der Liste gut erkennen kann, deshalb noch ein paar Worte extra.
- Zweiter Punkt
  - Verschachtelt A
  - Verschachtelt B
1. Nummeriert eins
2. Nummeriert zwei
- [ ] Offene Aufgabe
- [x] Erledigte Aufgabe

## Code

```cpp
int main() {
    printf("<b> bleibt literal, [tags] auch");
    return 0;   // Einrückung bleibt erhalten
}
```

## Zitat und Linie

> Ein Blockzitat mit **Markup** darin.

---

## Links und Tabelle

Ein [Link zur Doku](https://example.com/doku) und ein Autolink <https://example.org>.

| Spalte | Wert |
| :--- | ---: |
| GFM-Tabelle | 42 |

![Logo](logo.webp)
