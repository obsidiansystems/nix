# Reference scanning

While references could be arbitrary paths, Nix requires them to be store paths to ensure correctness.
Anything outside a given store is not under control of Nix, and therefore cannot be guaranteed to be present when needed.

However, having references match store paths in files is not enforced by the data model:
Store objects could have excess or incomplete references with respect to store paths found in their file contents.

Scanning files therefore allows reliably capturing run time dependencies without declaring them explicitly.
Doing it at build time and persisting references in the store object avoids repeating this time-consuming operation.
