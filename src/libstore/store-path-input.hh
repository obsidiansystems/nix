#pragma once

// The purpose of this module is to create a unified datatype that contains the
// possible inputs that get merged together to create the inputs of the
// makeStorePath function.
//
// In particular since we know that the store path has the form:
//     <store>/<h>-<name>
//
// we will have to pass in the <store>, the <name> and whatever ingredient is
// used for the creation of <h> (which varies on a per-case basis).

namespace nix {

// TODO: relevant entry-points: makeTextPath -> makeType
// Let's start with the fixed case. Our sources of variabity here are
//
// 1) the type that we want to pass in, which is in the form text:<r1>:..:<rn>,
// which will be encoded in a list of paths.
//
// 2) the string written to the resulting store path. This will just be a string

// TODO: relevant entry-points: see addToStore in local-store (could get
// references from ValidPathInfo). Also, in make-content-addressable, there is
// this run function, that's probably for handling the command, and that calls
// makeFixedOutputPath
//
// Now for the source case:
//
// the type encoding doesn't require anything else, while the h2 part requires
// the serialization of the path from which this store path is copied.
// Practically this is the return of hashPath, which is a string

// TODO: Relevant entry-points: makeOutputPaths
// For the output:<id> case, we need the <id> (which seems to be a string in
// both cases), and we need to distinguish outputs created by derivations from
// paths added using addToStore with different options. Also we have to
// distinguish between fixed and non fixed derivations.

// So, for non-fixed derivations, just the derivation
//
// For alternative addToStores and fixed-output: we have to keep in mind the
// rec, which is a FileIngestionMethod, then the Algo, which is a HashType, and
// a path or hash of contents of path (depending on if it's recursive or not)
//

// data StorePathInputs =
// { store :: StorePath
// , name :: Name
// , typeInputs :: TypeInputs
// }

// type Id = String

// data TypeInputs
//   = TypeText [Paths] Text
//   | TypeSource [Paths] SelfReference String
//   | TypeOutputFromNonFixedDerivation Id Derivation
//   | TypeOutputFromFixedDerivation Id (FileIngestionMethod+HashType+Hash = FileSystemHash)

// This matches the additional info that we need for makeTextPath
struct StorePathTextInputs {
    StorePathSet & references; // References for the paths
    std::string resultingPath; // The resulting store path
};

struct StorePathSourceInputs {
    StorePathSet & references; // References for the paths
    bool selfReference;
    std::string resultingPath;
};

struct StorePathNonFixedInputs {
    std::string id;
    Derivation derivation;
};

struct StorePathFixedInputs {
    std::string id;
    FileSystemHash fileSystemHash;
};

struct StorePathInputs {
    StorePath storePath;
    std::string name;
    std::variant<StorePathTextInputs, StorePathSourceInputs, StorePathNonFixedInputs, StorePathFixedInputs> inputs;
};


}
