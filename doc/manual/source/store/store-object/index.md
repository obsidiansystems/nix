# Store Object

A Nix store is a collection of *store objects* with *references* between them.
A store object consists of

  - A [file system object](../file-system-object.md) as data

  - references, a map of [store paths](../store-path.md) to store objects

Both parts of the store object are hashed into a *closure digest*.
Store objects are equal if and only if their closure digests are equal (excluding hash collisions).

## References

The reference map indicates which store objects the given store object depends on, and at what file name within the store directly they are expected to be located at.

Store objects can refer to both other store objects and themselves.
References from a store object to itself are called *self-references*.

To reference another store object, a store path is mapped to the closure digest for that store object.
To self-reference, a store path is mapped to a [sentinel value](https://en.wikipedia.org/wiki/Sentinel_value), indicating the current self-reference.

A store object can be at most one self-reference.
Informally, the store path used for that self-reference identifies where the store object must be located.
Formally this is enforced as follows: a (non-self) store object referenced by closure digest with a self reference must be mapped from the same store path as its self reference.

Conversely, any store object without a self-reference is *relocatable* --- it may be mounted at any store path.
It is highly recommended to use a [content address store path](./content-address.md) in this case, but this is not required.

### As a graph

Store objects and their references form a directed graph, where the store paths of store objects are the vertices, and the references are the edges.
In particular, the edge corresponding to a reference is from the store object that contains the reference, and to the store object that the store path (which is the reference) refers to.

> **Remark**
>
> A store object *without* a self-reference can be referenced at multiple store paths.
> In the above graph, that store object corresponds to multiple vertices, one for each of the store paths mapped to it.
> Likewise, every outgoing reference of that store object corresponds to multiple edges in the graph: one for each source vertex.

References other than a self-reference must not form a cycle.
The graph of references excluding self-references thus forms a [directed acyclic graph].

[directed acyclic graph]: @docroot@/glossary.md#gloss-directed-acyclic-graph

> **Remark**
>
> The embedding of closure digests in the preimage of closure digests means that the references graph is encoded in a [Merkle DAG](https://en.wikipedia.org/wiki/Merkle_tree).
> This prevent cycles by construction (assuming the hash function used is secure).

We can take the [transitive closure] of the references graph, in which any pair of store objects have an edge if a *path* of one or more references exists from the first to the second object.
(A single reference always forms a path which is one reference long, but longer paths may connect objects which have no direct reference between them.)
The *requisites* of a store object are all store objects reachable by paths of references which start with given store object's references.

[transitive closure]: https://en.wikipedia.org/wiki/Transitive_closure

We can also take the [transpose graph] of the references graph, where we reverse the orientation of all edges.
The *referrers* of a store object are the store objects that reference it.

[transpose graph]: https://en.wikipedia.org/wiki/Transpose_graph

One can also combine both concepts: taking the transitive closure of the transposed references graph.
The *referrers closure* of a store object are the store objects that can reach the given store object via paths of references.

> **Note**
>
> Care must be taken to distinguish between the intrinsic and extrinsic properties of store objects.
> We can create graphs from the store objects in a store, but the contents of the store is not, in general fixed, and may instead change over time.
>
> - The references of a store object --- the set of store paths called the references --- is a field of a store object, and thus intrinsic by definition.
    Regardless of what store contains the store object in question, and what else that store may or may not contain, the references are the same.
>
> - The requisites of a store object are almost intrinsic --- some store paths do not precisely refer to a unique single store object.
> Exactly what store object is being referenced, and what in turn *its* references are, depends on the store in question.
>   Different stores may disagree on what a given store path refers to.
>
> - The referrers of a store object are completely extrinsic, and depends solely on the store which contains that store object, not the store object itself.
>   Other store objects which refer to the store object in question may be added or removed from the store.

## Immutability

Store objects are [immutable](https://en.wikipedia.org/wiki/Immutable_object):
Once created, they do not change nor can any store object they reference be changed.

> **Note**
>
> Stores which support atomically deleting multiple store objects allow more flexibility while still upholding this property.

## Closure property and coherence

As was already mentioned above, non-self references and self-references need to agree on where a given store object is mounted.
Also, all references in a closure must agree as to what store object is mounted at any store object.
These two conditions are the *coherency* conditions of a valid store object.
We formalize this below:

Let \\(\\overline{C}\\) be references relation collected from all the references of all the objects in a closure.
Precisely, given an arbitrary collection of store objects \\(X\\), define \\(X\_x\\) for all \\(x \\in X\\) as the least fixed point of the following two rules:

\\[
\\begin{prooftree}
\\AxiomC{}
\\UnaryInfC{$x \\in X\_x$}
\\end{prooftree}
\\]

\\[
\\begin{prooftree}
\\AxiomC{$y \in X\_x$}
\\UnaryInfC{$\text{range}(\text{refs}(y)) \subseteq X\_x$}
\\end{prooftree}
\\]

And the define \\(\overline{C\_x}\\) for all \\(x \\in X\\) directly in terms of \\(X\_x\\):

\\[
\overline{C\_x} = \\bigcup\_{y \in X\_x} \\left( \text{refs}(y) \\right)
\\]

\\(\overline{C\_x}\\) is a relation, because even if each object's references are functional (a "map", per the above), their union may not be.
The first coherence rule is that the union in fact corresponds to a function, \\(C\_x\\):

\\[
\\forall s \in \text{domain}(\overline{C\_x}). \overline{C\_x}[s] = \\{ C[s] \\}
\\]

Every \\(C\_x\\) is the reference transitively visible from a given store object \\(x\\).
Then, for any such \\(C = C\_x\\), we can express the second coherence rules above:

\\[
\\forall s t. \text{self-ref}(C[s]) = \text{Some(t)} \Rightarrow s = t
\\]

### Locality of coherence

Because these laws only govern each \\(C\_x\\), they are a *local* coherence criterion.
For example, imagine a \\(X = Y \\cup Z\\), where none of the store objects in \\(Y\\) know about any of the store objects in \\(Y\\), and vise versa.
In this case, the coherence constraints on any \\(C\_y\\) do not impact any \\(C\_z\\), and vise versa, likewise.
This is true *even though* some \\(C\_y\\) and \\(C\_z\\) may well map the same store path to different object files (to choose one coherence violation).
Rather, it is only one one one extends the collection with a new store object, \\(X' = X \\cup \{x'\}\\), viewing the conflict, \\(\{x, y\} \subseteq \\text{refs}(x')\\), that a coherence violation occurs.

The upshot of this is that locally-coherent stores can *always* be combined together, irrespective of what each store contains.

### Global store coherence

Stores that are just locally coherent are not what must Nix users are used to, nor what the implementation currently implements.
Instead of we stores which are *globally* coherent.
This is as if the store had one additional store object that references every other object.
That "all-seeing" fictitious \\(\\text{root}\\) store object would have a \\(X_\\text{root}\\) containing all the other store objects itself, and a \\(C_\\text{root}\\) containing all the references of all the store objects.
And then, applied to that \\(C_\\text{root}\\), the local coherence conditional become global ones.

A rather basic consequence of these coherence conditions in their global form is the "classic" closure property:
A (globally coherent) store can only contain a store object if it also contains all the store objects it refers to.

> **Note**
>
> The "closure property" isn't meant to prohibit, for example, [lazy loading](https://en.wikipedia.org/wiki/Lazy_loading) of store objects.
> However, the "closure property" and immutability in conjunction imply that any such lazy loading ought to be deterministic.

## "Franken-closures" and integrity model { #integrity }

Suppose that one does want to combine two store object closures that disagree on references.
Can the store objects be modified so that coherence is preserved?
The answer is "yes".
The deepest (i.e. transitively referencing on no such other) store object that disagrees in its assignment of store paths can be modified to change the closure digest the store path is mapped to.
This will change its own closure digest, and that change can likewise be percolated up through the closure in the usual many for a (non-destructive) Merkle DAG update.
The resulting store objects are completely valid.

Historically with Nix, this could happen all the time, without any user sign-off.
Now the use of closure digests prevents this from happening by accident, but as the feasibility of the above rewriting show, it does not prevent intentional tampering.
That is the responsibility of a different data structure, the [build trace], that is conceptually layered atop this.

> **Remark**
>
> Is the tamperable nature of store objects themselves a good thing?
> Arguably, yes: combining e.g. disagreeing disagreeing cache's build products when one is in a rush can be useful.
> And it would be regression to make it impossible.
>
> The current design strikes a middle ground of making it possible, but opt-in (since the recursive closure digest rewriting won't happen by chance).
> This is hopefully the best of both worlds.

A final consequence of the above design is that there is no such thing as "invalid store object", except for having an invalid closure.
Any file system object, paired with any references, so long as the closure is coherent), can be assigned a closure digest.
Again, more practical notions of invalidity like "wasn't actually produced by XYZ" are the responsibility of other layers of the system.

## Store Object Metadata {#metadata}

[Store implementations](@docroot@/store/types/index.md) currently associate more information than described above with a store object.
Quite arguably some of this information doesn't belong here, because it conflates concerns.
For details see the [store object info](@docroot@/protocols/json/store-object-info.md) JSON format or the [narinfo](@docroot@/protocols/binary-cache/narinfo.md) format.

[build trace]: @docroot@/store/build-trace.md
