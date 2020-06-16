#pragma once

#include "types.hh"

#include <variant>

#include <nlohmann/json_fwd.hpp>

namespace nix::fetchers {

/* Wrap bools to prevent string literals (i.e. 'char *') from being
   cast to a bool in Attr. */
template<typename T>
struct Explicit {
    T t;
};

typedef std::variant<std::string, int64_t, Explicit<bool>> Attr;
typedef std::map<std::string, Attr, std::less<>> Attrs;

Attrs jsonToAttrs(const nlohmann::json & json);

nlohmann::json attrsToJson(const Attrs & attrs);

std::optional<std::string> maybeGetStrAttr(const Attrs & attrs, std::string_view name);

std::string getStrAttr(const Attrs & attrs, std::string_view name);

std::optional<int64_t> maybeGetIntAttr(const Attrs & attrs, std::string_view name);

int64_t getIntAttr(const Attrs & attrs, std::string_view name);

std::optional<bool> maybeGetBoolAttr(const Attrs & attrs, std::string_view name);

bool getBoolAttr(const Attrs & attrs, std::string_view name);

std::map<std::string, std::string> attrsToQuery(const Attrs & attrs);

}
