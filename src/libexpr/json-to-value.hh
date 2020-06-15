#pragma once

#include "eval.hh"

#include <string>

namespace nix {

MakeError(JSONParseError, EvalError);

void parseJSON(EvalState & state, std::string_view s, Value & v);

}
