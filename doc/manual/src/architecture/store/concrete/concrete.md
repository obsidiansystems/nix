# The concrete model

The concrete model reconciles this abstract vision with the reality of Unix as it exists today.
Store objects own file system data, reference can be encoded as file system paths, and build steps run arbitrary processes.
As none of these three Unix abstractions are inherently "functional" per the properties above, it is up to Nix to enforce those properties.

## One interface, many implementations

There exists different types of stores, which all follow this model.
Examples:
- store on the local file system
- remote store accessible via SSH
- binary cache store accessible via HTTP

We see in the latter two that there is room for flexibility.
Builds can be distributed to multiple machines, and data must only be "exposed" via conventional OS interfaces during build steps, being free to be stored "at rest" in other less conventional ways.
