#pragma once

#include "ref.hh"

#include <list>
#include <set>
#include <map>
#include <vector>

namespace nix {

using std::list;
using std::set;
using std::vector;
using std::string;

typedef list<string> Strings;
typedef set<string, std::less<>> StringSet;
typedef std::map<string, string, std::less<>> StringMap;

/* Paths are just strings. */

typedef string Path;
typedef std::string_view PathView;
typedef list<Path> Paths;
typedef set<Path, std::less<>> PathSet;

/* Helper class to run code at startup. */
template<typename T>
struct OnStartup
{
    OnStartup(T && t) { t(); }
};

}
