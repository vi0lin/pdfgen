#pragma once
// ---------------------------------------------------------------------------
// htmlwriter -- Stufe 2 fuer HTML: dieselbe Token-Liste wie buildFlowables
// (markup::tokenize hat Loops, Bedingungen, Includes, Tabellen, Bilder,
// Gruppen laengst aufgeloest), nur dass hier HTML mit eingebettetem CSS
// herauskommt statt Layout-Objekte fuer den PDF-Writer.
//
//   \doc  Name, format=html      -> Name.html
//   \html Name                   -> dasselbe
//   \doc  Name, format=pdf,html  -> beides aus einem Baustein
//   \mail Name, ..., format=html -> HTML-Mail mit Textalternative
//
// Bilder: in Dateien als data:-URI eingebettet (die HTML ist alleinstehend);
// in Mails als cid:-Verweis, die Dateien kommen als inline-Anhaenge mit.
// ---------------------------------------------------------------------------
#include <string>
#include <utility>
#include <vector>
#include "markup.h"

namespace htmlout {

struct InlineImage { std::string cid; std::string path; };

struct Result {
  std::string html;                       // vollstaendiges Dokument (<!DOCTYPE ...>)
  std::string text;                       // Textfassung derselben Tokens (Mail-Alternative)
  std::vector<InlineImage> images;        // nur forMail: cid -> Datei
};

Result render(const std::vector<markup::Token>& tokens, const std::string& baseDir,
              const std::string& title, bool forMail, std::vector<std::string>& warnings);

// Inline-Markup eines Absatzes (<b> <i> <u> <br/>) nach HTML; alles andere
// wird maskiert. Oeffentlich, weil die Textfassung dieselbe Zerlegung nutzt.
std::string inlineToHtml(const std::string& markupText);
std::string inlineToText(const std::string& markupText);

} // namespace htmlout
