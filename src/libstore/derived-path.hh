#pragma once

#include "util.hh"
#include "path.hh"
#include "realisation.hh"

#include <variant>

#include <nlohmann/json_fwd.hpp>

namespace nix {

class Store;

/**
 * An opaque derived path.
 *
 * Opaque derived paths are just store paths, and fully evaluated. They
 * cannot be simplified further. Since they are opaque, they cannot be
 * built, but they can fetched.
 */
struct DerivedPathOpaque {
    StorePath path;

    nlohmann::json toJSON(const Store & store) const;
    std::string to_string(const Store & store) const;
    static DerivedPathOpaque parse(const Store & store, std::string_view);
};

struct SingleDerivedPath;

/**
 * A single derived path that is built from a derivation
 *
 * Built derived paths are pair of a derivation and an output name. They are
 * evaluated by building the derivation, and then taking the resulting output
 * path of the given output name.
 */
struct SingleDerivedPathBuilt {
    std::shared_ptr<SingleDerivedPath> drvPath;
    std::string outputs; // FIXME rename "output" no "s"

    std::string to_string(const Store & store) const;
    static SingleDerivedPathBuilt parse(const Store & store, std::string_view, std::string_view);
};

using _SingleDerivedPathRaw = std::variant<
    DerivedPathOpaque,
    SingleDerivedPathBuilt
>;

/**
 * A "derived path" is a very simple sort of expression that evaluates
 * to (concrete) store path. It is either:
 *
 * - opaque, in which case it is just a concrete store path with
 *   possibly no known derivation
 *
 * - built, in which case it is a pair of a derivation path and an
 *   output name.
 */
struct SingleDerivedPath : _SingleDerivedPathRaw {
    using Raw = _SingleDerivedPathRaw;
    using Raw::Raw;

    using Opaque = DerivedPathOpaque;
    using Built = SingleDerivedPathBuilt;

    inline const Raw & raw() const {
        return static_cast<const Raw &>(*this);
    }

    std::string to_string(const Store & store) const;
    static SingleDerivedPath parse(const Store & store, std::string_view);
};

static inline std::shared_ptr<SingleDerivedPath> staticDrvReq(StorePath drvPath)
{
    return std::make_shared<SingleDerivedPath>(SingleDerivedPath::Opaque { drvPath });
}

/**
 * A set of derived paths that are built from a derivation
 *
 * Built derived paths are pair of a derivation and some output names.
 * They are evaluated by building the derivation, and then replacing the
 * output names with the resulting outputs.
 *
 * Note that does mean a derived store paths evaluates to multiple
 * opaque paths, which is sort of icky as expressions are supposed to
 * evaluate to single values. Perhaps this should have just a single
 * output name.
 */
struct DerivedPathBuilt {
    std::shared_ptr<SingleDerivedPath> drvPath;
    std::set<std::string> outputs;

    std::string to_string(const Store & store) const;
    static DerivedPathBuilt parse(const Store & store, std::string_view, std::string_view);
};

using _DerivedPathRaw = std::variant<
    DerivedPathOpaque,
    DerivedPathBuilt
>;

/**
 * A "derived path" is a very simple sort of expression that evaluates
 * to one or more (concrete) store paths. It is either:
 *
 * - opaque, in which case it is just a single concrete store path with
 *   possibly no known derivation
 *
 * - built, in which case it is a pair of a derivation path and some
 *   output names.
 */
struct DerivedPath : _DerivedPathRaw {
    using Raw = _DerivedPathRaw;
    using Raw::Raw;

    using Opaque = DerivedPathOpaque;
    using Built = DerivedPathBuilt;

    inline const Raw & raw() const {
        return static_cast<const Raw &>(*this);
    }

    std::string to_string(const Store & store) const;
    static DerivedPath parse(const Store & store, std::string_view);
};

struct SingleBuiltPath;

struct SingleBuiltPathBuilt {
    std::shared_ptr<SingleBuiltPath> drvPath;
    std::pair<std::string, StorePath> outputs; // FIXME rename "output" no "s"

    nlohmann::json toJSON(const Store & store) const;
    static SingleBuiltPathBuilt parse(const Store & store, std::string_view, std::string_view);
};

using _SingleBuiltPathRaw = std::variant<
    DerivedPathOpaque,
    SingleBuiltPathBuilt
>;

struct SingleBuiltPath : _SingleBuiltPathRaw {
    using Raw = _SingleBuiltPathRaw;
    using Raw::Raw;

    using Opaque = DerivedPathOpaque;
    using Built = SingleBuiltPathBuilt;

    inline const Raw & raw() const {
        return static_cast<const Raw &>(*this);
    }

    StorePath outPath() const;

    nlohmann::json toJSON(const Store & store) const;
    static SingleBuiltPath parse(const Store & store, std::string_view);
};

static inline std::shared_ptr<SingleBuiltPath> staticDrv(StorePath drvPath)
{
    return std::make_shared<SingleBuiltPath>(SingleBuiltPath::Opaque { drvPath });
}

/**
 * A built derived path with hints in the form of optional concrete output paths.
 *
 * See 'BuiltPath' for more an explanation.
 */
struct BuiltPathBuilt {
    std::shared_ptr<SingleBuiltPath> drvPath;
    std::map<std::string, StorePath> outputs;

    nlohmann::json toJSON(const Store & store) const;
    static BuiltPathBuilt parse(const Store & store, std::string_view);
};

using _BuiltPathRaw = std::variant<
    DerivedPath::Opaque,
    BuiltPathBuilt
>;

/**
 * A built path. Similar to a `DerivedPath`, but enriched with the corresponding
 * output path(s).
 */
struct BuiltPath : _BuiltPathRaw {
    using Raw = _BuiltPathRaw;
    using Raw::Raw;

    using Opaque = DerivedPathOpaque;
    using Built = BuiltPathBuilt;

    inline const Raw & raw() const {
        return static_cast<const Raw &>(*this);
    }

    StorePathSet outPaths() const;
    RealisedPath::Set toRealisedPaths(Store & store) const;

    nlohmann::json toJSON(const Store & store) const;
};

typedef std::vector<DerivedPath> DerivedPaths;
typedef std::vector<BuiltPath> BuiltPaths;

nlohmann::json derivedPathsWithHintsToJSON(const BuiltPaths & buildables, const Store & store);

}
