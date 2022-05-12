# The concrete model

The concrete model reconciles this [abstract vision](../abstract/abstract.md) with the reality of Unix as it exists today.
Store objects own file system data, references can be encoded as file system paths, and build steps run arbitrary processes.
- FEEDBACK: The choice of "own" in "Store objects own" can be interpreted as "Store objects are a file permissions mechanism"
- FEEDBACK: By "references can be" does this mean references can be encoded as something other than file system paths?
As none of these three Unix abstractions were designed with the functional properties enforced by nix, it is up to Nix to equip Unix abstractions with those properties.

## One interface, many implementations

There exist different types of stores, all of which follow this model.

Examples:
- a store on the local file system
- a remote store accessible via SSH
- a binary cache store accessible via HTTP

We see in the latter two that there is room for flexibility.
- FEEDBACK: Flexibility in what sense? Can these networked stores somehow escape the functional properties enforced by nix (what kinds of "distributed system" transparency are on offer)?
Builds can be distributed to multiple machines, and data must only be "exposed" via conventional OS interfaces during build steps, being free to be stored "at rest" in other less conventional ways.
- FEEDBACK: I'm confused by what "at rest" means in this sentence.