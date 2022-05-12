# Store Object

Concrete Store objects comes in a few variations of Nix, but the basic model of a store object is the triple of

  - a [file system object](#file-system-object)
  - a set of [references](#references) to store objects.
  - a name

While the abstract model allows references and other data to be intermingled, the concrete model strictly separates them.

```idris
data StoreObjectRef

record StoreObject where
  root       : FileSystemObject
  references : Set StoreObjectRef
  name       : String

getReferences so = so.references
```

We call a store object's outermost file system object the *root*.

- FEEDBACK: What does "outer" mean here? Is this closer to root, or closer to a leaf? An example would be good here.

The string name is subject to this condition (taken from an error message in the implementation):

> names are alphanumeric and can include the symbols +-._?= and must not begin with a period.

- FEEDBACK: The description here is crystal clear, but I think a concrete (pun... intended?) example (even if it's a toy) like gnu hello from the nix pills would make this section stronger.

## File system object {#file-system-object}

The Nix store uses a simple file system model.

Every file system object is one of the following:
 - File: an executable flag, and arbitrary data for contents
 - Directory: mapping of names to child file system objects
 - [Symbolic link](https://en.m.wikipedia.org/wiki/Symbolic_link): may point anywhere.

```idris
data FileSystemObject
  = File {
      isExecutable : Bool,
      contents     : Bytes,
    }
  | Directory { entries : Map FileName FileSystemObject }
  | SymLink { target : Path }
```

A bare file or symlink can be a root file system object.

- FEEDBACK: Are there any restrictions on directories?

Symbolic links pointing outside of their own root, or to a store object without a matching reference, are allowed, but might not function as intended.

## References {#references}

The reference's independence from the file system reflects the fact that Unix has no built-in concept of a file system reference that enforce Nix's functional laws:

- Symbolic links are allowed to dangle or be acyclic.
  Nix does not impose those restrictions for compatibility with existing software that may use symlinks in these ways.

- Hard links are only permitted to files to trivially avoid cycles (since files do not have children/outgoing links.)
  Local stores allow hard links as a space optimization (which a safe one since store objects are immutable) but has no semantic content.

- FEEDBACK: Can you add an exampleof how a hard link would "trivially avoid a cycle"? This is not obvious to me.

- FEEDBACK: "which a safe one" is the intent: "this optimization is safe because store objects are immutable"? Another improvement on the precision of this sentence would be including what kind of safety this provides.

Exactly what form the references take depends on the type of store object.
We will provide more details in the following sections, leaving "store path reference" abstract for now.
