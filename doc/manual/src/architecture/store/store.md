# Store

A Nix store is a collection of *store objects* referred to by *store paths*.
Every store also has a "store directory path", which is a path prefix used for various purposes.

There are many types of stores, but all of them at least respect this model.
Some however offer additional functionality.

## A [Rosetta stone](https://en.m.wikipedia.org/wiki/Rosetta_Stone) for build system terminology

The Nix store's design is comparable to other build systems.
Usage of terms is, for historic reasons, not entirely consistent within the Nix ecosystem, and still subject to slow change.

The following translation table points out similarities and equivalent terms, to help clarify their meaning and inform consistent use in the future.

generic build system | Nix | [Bazel](https://bazel.build/start/bazel-intro) | [Build Systems à la Carte](https://www.microsoft.com/en-us/research/uploads/prod/2018/03/build-systems.pdf) | programming language
-- | -- | -- | -- | --
data (build input, build result) | component | [artifact](https://bazel.build/reference/glossary#artifact) | value | value
build plan | derivation | [action](https://bazel.build/reference/glossary#action) | `Task` | [thunk](https://en.m.wikipedia.org/wiki/Thunk)
build graph | derivation graph | [action graph](https://bazel.build/reference/glossary#action-graph), [build graph](https://bazel.build/reference/glossary#build-graph) | `Tasks` | [call graph](https://en.m.wikipedia.org/wiki/Call_graph)
build instructions | builder | ([depends on action type](https://docs.bazel.build/versions/main/skylark/lib/actions.html)) | `Task` | function
build | realisation | build | application of `Build` | evaluation
persistence layer | store | [action cache](https://bazel.build/reference/glossary#action-cache) | `Store` | heap

All of these systems share features of [declarative programming](https://en.m.wikipedia.org/wiki/Declarative_programming) languages, a key insight first put forward by Eelco Dolstra et al. in [Imposing a Memory Management Discipline on Software Deployment](https://edolstra.github.io/pubs/immdsd-icse2004-final.pdf) (2004), elaborated in his PhD thesis [The Purely Functional Software
Deployment Model](https://edolstra.github.io/pubs/phd-thesis.pdf) (2006), and further refined by Andrey Mokhov et al. in [Build Systems à la Carte](https://www.microsoft.com/en-us/research/uploads/prod/2018/03/build-systems.pdf) (2018).
