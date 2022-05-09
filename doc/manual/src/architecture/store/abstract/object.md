# Abstract store object

Nix organizes the data it manages into *store objects*.
An store object can be thought of as a black box that can reference other store objects.
- FEEDBACK: I would consider listing out that store objects can be arbitrary scripts, derivations, ect. Since "store object" sounds like it must be some special nix-binary blob or something as written. 

In pseudo-code:

```idris
data StoreObject
data StoreObjectRef

getReferences : StoreObject -> Set StoreObjectRef
```

References must not "dangle": in all contexts, if an object equipped with a reference exists then the referenced object to must also exist.
This existential invariant implies that store objects and their references form a directed graph of symlinks (are there any counterexamples to this?).
Furthermore, this invariant combined with the immutability of store objects implies that the graph is acyclic:
to avoid both dangling references and mutation, all referenced objects must exist prior to the creation of an object that references it.
It follows from induction that, store objects with references must be constructed by store objects without references.
Thus the dependency graph acyclic.

Other operations impacted by these invariant are:

**Copying***

- Store objects can be copied between stores.
- When an objects is copied, its references must refer to objects already in the destination store.
- Recursive copying must either proceed in dependency order or be atomic.
    - FEEDBACK: I'm not sure what the intent of this sentence is, can you add an example?

**Deleting**

- Only unreferenced objects may be safely deleted.
- Recursive deleting (garbage collection) must either proceed in dependency order, or be atomic.
    - FEEDBACK: Same feedback as the Copying section.
