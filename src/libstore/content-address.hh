#pragma once

#include <nlohmann/json.hpp>
#include <variant>

#include "hash.hh"
#include "path.hh"

namespace nix {

/*
 * Mini content address
 */

enum struct FileIngestionMethod : uint8_t {
    Flat,
    Recursive,
    Git,
};


struct TextHash {
    Hash hash;
};

/// Pair of a hash, and how the file system was ingested
struct FixedOutputHash {
    FileIngestionMethod method;
    Hash hash;
    std::string printMethodAlgo() const;
};

// Deref is a phantom parameter that just signifies what the hash should
// dereference too.
template<typename Deref>
struct IPFSHash {
    Hash hash;
};

template<template <typename> class Ref>
struct IPFSGitTreeNodeT;

typedef IPFSHash<IPFSGitTreeNodeT<IPFSHash>> IPFSGitTreeReference;

/*
  We've accumulated several types of content-addressed paths over the years;
  fixed-output derivations support multiple hash algorithms and serialisation
  methods (flat file vs NAR). Thus, ‘ca’ has one of the following forms:

  * ‘text:sha256:<sha256 hash of file contents>’: For paths
    computed by makeTextPath() / addTextToStore().

  * ‘fixed:<r?>:<ht>:<h>’: For paths computed by
    makeFixedOutputPath() / addToStore().
*/
typedef std::variant<
    TextHash, // for paths computed by makeTextPath() / addTextToStore
    FixedOutputHash, // for path computed by makeFixedOutputPath
    IPFSGitTreeReference
> LegacyContentAddress;

/* Compute the prefix to the hash algorithm which indicates how the files were
   ingested. */
std::string makeFileIngestionPrefix(const FileIngestionMethod m);

std::string renderLegacyContentAddress(LegacyContentAddress ca);

std::string renderLegacyContentAddress(std::optional<LegacyContentAddress> ca);

LegacyContentAddress parseLegacyContentAddress(std::string_view rawCa);

std::optional<LegacyContentAddress> parseLegacyContentAddressOpt(std::string_view rawCaOpt);

/*
 * References set
 */

template<typename Ref>
struct PathReferences
{
    std::set<Ref> references;
    bool hasSelfReference = false;

    bool operator == (const PathReferences<Ref> & other) const
    {
        return references == other.references
            && hasSelfReference == other.hasSelfReference;
    }

    /* Functions to view references + hasSelfReference as one set, mainly for
       compatibility's sake. */
    StorePathSet referencesPossiblyToSelf(const Ref & self) const;
    void insertReferencePossiblyToSelf(const Ref & self, Ref && ref);
    void setReferencesPossiblyToSelf(const Ref & self, std::set<Ref> && refs);
};

template<typename Ref>
StorePathSet PathReferences<Ref>::referencesPossiblyToSelf(const Ref & self) const
{
    StorePathSet refs { references };
    if (hasSelfReference)
        refs.insert(self);
    return refs;
}

template<typename Ref>
void PathReferences<Ref>::insertReferencePossiblyToSelf(const Ref & self, Ref && ref)
{
    if (ref == self)
        hasSelfReference = true;
    else
        references.insert(std::move(ref));
}

template<typename Ref>
void PathReferences<Ref>::setReferencesPossiblyToSelf(const Ref & self, std::set<Ref> && refs)
{
    if (refs.count(self))
        hasSelfReference = true;
        refs.erase(self);

    references = refs;
}

void to_json(nlohmann::json& j, const LegacyContentAddress & c);
void from_json(const nlohmann::json& j, LegacyContentAddress & c);

// Needed until https://github.com/nlohmann/json/pull/211

void to_json(nlohmann::json& j, const std::optional<LegacyContentAddress> & c);
void from_json(const nlohmann::json& j, std::optional<LegacyContentAddress> & c);

/*
 * Full content address
 *
 * See the schema for store paths in store-api.cc
 */

// This matches the additional info that we need for makeTextPath
struct TextInfo : TextHash {
    // References for the paths, self references disallowed
    StorePathSet references;
};

struct FixedOutputInfo : FixedOutputHash {
    // References for the paths
    PathReferences<StorePath> references;
};

template<typename Underlying>
struct ContentAddressT;

template<template <typename> class Ref>
struct IPFSGitTreeNodeT {
	Hash gitTree;
    // References for the paths
    PathReferences<ContentAddressT<Ref<IPFSGitTreeNodeT<Ref>>>> references;
};

typedef IPFSGitTreeNodeT<IPFSHash> IPFSGitTreeNode;

// FIXME names
typedef std::variant<
    TextInfo,
    FixedOutputInfo,
    IPFSGitTreeReference
> ContentAddressWithoutName;

template<typename Underlying>
struct ContentAddressT {
    std::string name;
    Underlying info;

    bool operator < (const ContentAddressT<Underlying> & other) const
    {
        return name < other.name;
        // FIXME second field
    }
};

typedef ContentAddressT<ContentAddressWithoutName> ContentAddress;

std::string renderContentAddress(ContentAddress ca);

ContentAddress parseContentAddress(std::string_view rawCa);

void to_json(nlohmann::json& j, const ContentAddress & c);
void from_json(const nlohmann::json& j, ContentAddress & c);

void to_json(nlohmann::json& j, const PathReferences<StorePath> & c);
void from_json(const nlohmann::json& j, PathReferences<StorePath> & c);

}
