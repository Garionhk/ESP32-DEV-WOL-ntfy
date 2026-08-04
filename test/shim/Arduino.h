// Minimal Arduino compatibility shim so store.cpp can be compiled and tested
// on the host. Only the surface store.cpp actually uses.
#pragma once

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <string>

class String {
 public:
  std::string s;

  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& v) : s(v) {}
  String(int v) { char b[24]; snprintf(b, sizeof b, "%d", v); s = b; }
  String(unsigned v) { char b[24]; snprintf(b, sizeof b, "%u", v); s = b; }
  String(long v) { char b[32]; snprintf(b, sizeof b, "%ld", v); s = b; }
  String(unsigned long v) { char b[32]; snprintf(b, sizeof b, "%lu", v); s = b; }

  int indexOf(const char* needle) const {
    size_t p = s.find(needle);
    return p == std::string::npos ? -1 : (int)p;
  }

  unsigned int length() const { return (unsigned)s.size(); }
  bool isEmpty() const { return s.empty(); }
  const char* c_str() const { return s.c_str(); }
  void reserve(unsigned n) { s.reserve(n); }

  char operator[](unsigned i) const { return s[i]; }
  char& operator[](unsigned i) { return s[i]; }

  String& operator+=(char c) { s += c; return *this; }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(const char* o) { s += o; return *this; }

  bool operator==(const String& o) const { return s == o.s; }
  bool operator!=(const String& o) const { return s != o.s; }
  // Real Arduino String has these too; without them the implicit
  // `operator const char*` makes `str == "literal"` ambiguous.
  bool operator==(const char* o) const { return o && s == o; }
  bool operator!=(const char* o) const { return !(*this == o); }

  void trim() {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    s = (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
  }
  void toLowerCase() { for (auto& c : s) c = (char)tolower((unsigned char)c); }
  bool equalsIgnoreCase(const String& o) const {
    if (s.size() != o.s.size()) return false;
    for (size_t i = 0; i < s.size(); i++)
      if (tolower((unsigned char)s[i]) != tolower((unsigned char)o.s[i])) return false;
    return true;
  }

  operator const char*() const { return s.c_str(); }

  // ArduinoJson's ArduinoStringWriter clears via `str = (const char*)0` and
  // appends via concat().
  String& operator=(const char* c) { s = c ? c : ""; return *this; }
  unsigned char concat(const char* c) { if (c) s += c; return 1; }
};

inline String operator+(const String& a, const String& b) { return String(a.s + b.s); }
inline String operator+(const String& a, const char* b) { return String(a.s + b); }
inline String operator+(const char* a, const String& b) { return String(std::string(a) + b.s); }

// ArduinoJson needs to recognise String as a string type.
namespace ArduinoJson {
inline namespace V7HEAD {}
}

struct SerialShim {
  template <typename... A> void printf(const char* f, A... a) { std::printf(f, a...); }
  void println(const char* m) { std::printf("%s\n", m); }
  void println() { std::printf("\n"); }
  void print(const char* m) { std::printf("%s", m); }
};
extern SerialShim Serial;
