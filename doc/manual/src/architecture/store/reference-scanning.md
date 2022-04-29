# Reference scanning

In most cases, Nix can automatically come up with the references that store objects ought to have, saving the user from doing additional bookkeeping.

## Why it works

This might be surprising to users coming from elsewhere!
Traditional build systems do not track dependencies between build artifacts, as the problem appears very challenging since file formats are numerous.
It would seem either there would need to be a way to teach the build system how to parse arbitrary formats, or the build system would need to track references via manual annotations the user must write and, worse, keep up to date.
I
On of the best insights in the creation of Nix was how cut this Gordian knot.

Store objects are, as previously mentioned, mounted at `<store-dir>/<hash>-<name>` when they are exposed to the file system.
This is the textual form of a [store path](./paths.md).
When this happens depends on the type of store, but it *must* happen during building, as regular Unix processes are being run.
Those paths contain hashes, and hashes are not guessable.
As such, build results *must* store those hashes if the programs inside them wish to refer to those store objects without the aid of extra information when they are run.

The hashes could be encrypted or compressed, but regular software doesn't need to do this.
The result is that just scanning for hashes works quite well!

## How it works

Hashes are scanned for, not entire store paths.
Thus, nix would look for, e.g.,
```
b6gvzjyb2pg0kjfwrjmg1vfhh54ad73z
```
not
```
/nix/store/b6gvzjyb2pg0kjfwrjmg1vfhh54ad73z-firefox-33.1
```

## When it happens

Scanning happens when store paths are created that could refer to other store paths.
When source code is added, references are prohibited by fiat, and thus no scanning is needed.
Build results can refer to other objects, so scanning does happen at the end of a build.

## Exceptions

Note that just because Nix *can* scan references doesn't mean that it *must* scan for references.

There is no problem with a store object having an "extra" reference that doesn't correspond to a hash inside the dependency.
The store object will have an extra dependency that it doesn't need, but that is fine.
It is a case adjacent to that of a hash occurring in some obscure location within the store object that is never read.

[Drv files](./drvs/drvs.md), discussed next, might also contain store paths that *aren't* references.
The specific reasons for this will be given then, but we can still ask how how could this possibly be safe?
The fundamental answer is that since the drv file format is known to Nix, it can "do better" than plain scanning.
Nix knows how to parse them, and thus meaningfully differentiate between hashes based on *where* they occur.
It can decide which contexts correspond to an assumption that the store path ought to exist and be accessible, and which contexts do not.
