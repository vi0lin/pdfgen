// jsondata.h -- minimal JSON for mail-merge data (no dependencies).
//
// Parses the subset every mailing list needs: objects, arrays, strings
// (with \" \\ \/ \n \t \r \b \f \uXXXX), numbers, true/false/null.
// Also offers a tiny BUILDER so a host program can assemble recipient
// data in C++ and hand pdfgen the JSON text (or write the file):
//
//   using namespace jsondata;
//   Value list = Value::array();
//   Value k = Value::object();
//   k.set("name", "Anna Muster");
//   k.set("email", "anna@example.com");
//   Value posten = Value::array();
//   posten.push(Value::object().set("text", "Beratung").set("preis", 120));
//   k.set("posten", posten);
//   list.push(k);
//   std::string json = list.toJson();   // -> kunden.json / processSource
//
// Plain-C hosts simply snprintf the JSON text -- the format IS the API.
#pragma once
#define PDFGEN_JSONDATA_API 1

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jsondata {

class Value {
 public:
  enum class Kind { Null, Bool, Number, String, Array, Object };

  Value() = default;
  static Value object();
  static Value array();
  Value(bool b);
  Value(double n);
  Value(int n);
  Value(const char* s);
  Value(const std::string& s);

  Kind kind() const { return kind_; }
  bool isNull() const { return kind_ == Kind::Null; }

  // ---- reading ----
  // Path lookup: "name", "kunde.ort", "posten[0].preis". On an array,
  // a bare path step that is a number indexes it. Missing -> Null value.
  const Value& at(const std::string& path) const;
  size_t size() const;                         // array/object element count
  const Value& index(size_t i) const;          // array element (Null if OOR)
  std::vector<std::string> keys() const;       // object keys in file order
  // Text for placeholder substitution: strings verbatim; numbers without
  // trailing zeros; true/false; null -> ""; arrays/objects -> compact JSON.
  std::string toText() const;
  std::string toJson() const;                  // compact serialization
  // "truthy" for conditions: not null/false/0/""/empty array|object.
  bool truthy() const;

  // ---- building (host side) ----
  Value& set(const std::string& key, Value v);     // object; returns *this
  Value& push(Value v);                            // array;  returns *this

  // ---- parsing ----
  // Returns a Null value and fills err on failure (line info included).
  static Value parse(const std::string& text, std::string& err);

 private:
  Kind kind_ = Kind::Null;
  bool b_ = false;
  double num_ = 0;
  std::string str_;
  std::vector<Value> arr_;
  std::vector<std::pair<std::string, Value>> obj_;   // keeps file order
  static const Value& nullValue();
};

}  // namespace jsondata
