# Content-Addressing Store Objects

Just [like][fso-ca] [File System Objects][File System Object],
[Store Objects][Store Object] can also be [content-addressed](@docroot@/glossary.md#gloss-content-address),
unless they are [input-addressed].

Actually, in some sense, store objects are *always* content-addressed.
For a very long time, the file system objects of all store objects were all given "[NAR][Nix Archive] Hashes".
More recently, they are also all secured with "closure digests", which, unlike the "NAR Hash", accounts for the entire store object.
That does fit the technical definition of a content address.

However, in most contexts, store objects are *addressed* not via these hashes, but with the [Store Path] that has been assigned to them.
We thus term store object content-addresed if the store path is determined from its content.
In particular, the content-addressing scheme will ensure that the digest of the store path is solely computed from the

- file system object graph (the root one and its children, if it has any)
- references
- [store directory path](../store-path.md#store-directory-path)
- name

of the store object, and not any other information, which would not be an intrinsic property of that store object.

For the full specification of the algorithms involved, see the [specification of store path digests][sp-spec].

[File System Object]: ../file-system-object.md
[Store Object]: ../store-object/index.md
[Store Path]: ../store-path.md

## Content addressing each part of a store object

### File System Objects

With all currently-supported store object content-addressing methods, the file system object is always [content-addressed][fso-ca] first, and then that hash is incorporated into content address computation for the store object.

### References

#### References to other store objects

With all currently supported store object content addressing methods,
other objects are referred to by their regular (string-encoded-) [store paths][Store Path].

#### Self-references

Self-references however cannot be referred to by their path, because we are in the midst of describing how to compute that path!

> The alternative would require finding as hash function fixed point, i.e. the solution to an equation in the form
> ```
> digest = hash(..... || digest || ....)
> ```
> which is computationally infeasible.
> As far as we know, this is equivalent to finding a hash collision.

Instead we have a "has self-reference" boolean, which ends up affecting the digest:
In all currently-supported store object content-addressing methods, when hashing the file system object data, any occurrence of store object's own store path in the digested data is replaced with a [sentinel value](https://en.wikipedia.org/wiki/Sentinel_value).
The hashes of these modified input streams are used instead.

When validating the content address of a store object after the fact, the above process works as written.
However, when first creating the store object we don't know the store object's store path, as explained just above.
We therefore, strictly speaking, do not know what value we will be replacing with the sentinel value in the inputs to hash functions.
What instead happens is that the provisional store object --- the data from which we wish to create a store object --- is paired with a provisional "scratch" store path (that presumably was chosen when the data was created).
That provisional store path is instead what is replaced with the sentinel value, rather than the final store object which we do not yet know.

> **Design note**
>
> It is an informal property of content-addressed store objects that the choice of provisional store path should not matter.
> In other words, if a provisional store object is prepared in the same way except for the choice of provision store path, the provisional data need not be identical.
> But, after the sentinel value is substituted in place of each provisional store object's provision store path, the final so-normalized data *should* be identical.
>
> If, conversely, the data after this normalization process is still different, we'll compute a different content-address.
> The method of preparing the provisional self-referenced data has *failed* to be deterministic in the sense of not *leaking* the choice of provisional store path --- a choice which is supposed to be arbitrary --- into the final store object.
>
> This property is informal at this stage, because we are just describing store objects, which have no formal notion of their origin.
> Without that, we cannot rigorously define what it would mean for the process to be insufficiently deterministic.
> When we cover [derivations](@docroot@/store/derivation/index.md), we will be able to make this a formal property — not of content-addressed store objects themselves, but of the derivations that *produce* them.

### Name and Store Directory

These two items affect the digest in a way that is standard for store path digest computations and not specific to content-addressing.
Consult the [specification of store path digests][sp-spec] for further details.

## Content addressing Methods

For historical reasons, we don't support all features in all combinations.
Each currently supported method of content addressing chooses a single method of file system object hashing, and may offer some restrictions on references.
The names and store directories are unrestricted however.

### Flat { #method-flat }

This uses the corresponding [Flat](../file-system-object/content-address.md#serial-flat) method of file system object content addressing.

References are not supported: store objects with flat hashing *and* references can not be created.

### Text { #method-text }

This also uses the corresponding [Flat](../file-system-object/content-address.md#serial-flat) method of file system object content addressing.

References to other store objects are supported, but self-references are not.

This is the only store-object content-addressing method that is not named identically with a corresponding file system object method.
It is somewhat obscure, mainly used for "drv files"
(derivations serialized as store objects in their ["ATerm" file format](@docroot@/protocols/derivation-aterm.md)).
Prefer another method if possible.

### Nix Archive { #method-nix-archive }

This uses the corresponding [Nix Archive] method of file system object content addressing.

References (to other store objects and self-references alike) are supported so long as the hash algorithm is SHA-256, but not (neither kind) otherwise.

### Git { #method-git }

> **Warning**
>
> This method is part of the [`git-hashing`][xp-feature-git-hashing] experimental feature.

This uses the corresponding [Git](../file-system-object/content-address.md#git) method of file system object content addressing.

References are not supported.

Only SHA-1 is supported at this time.
If [SHA-256-based Git](https://git-scm.com/docs/hash-function-transition)
becomes more widespread, this restriction will be revisited.

## Shallowness of store object content-addressing

Note that none of these methods currently account for the closure digest parts of the store object's references.
They instead just treat the references as a set of store objects, taking the keys and discarding the values of the map.
For a store object closure which is entirely content-addressed, this makes no difference, as all information in the closure will affect the store path content-address of the root store object.
However, when the closure contains an [input-addressed] store object, it starts to matter.

Such non-content-address store paths may be associated with many possible store objects --- the content of the mapped store object is *not* secured by the non-content-address store path.
As such, the content-address store path of the root object does determine that there is *some* store object at such a store object, and which other (content-address-reachable) store objects in the closure reference it, but it does *not* determine what thatstore path is mapped to.

Following the discussion on [store object closure integrity](#integrity), this is considered actually a feature, and not a bug.
The expectation for input-addressed store objects is that it is at least possible to mix-and-match them in unsafe ways.
If changing the contents of a store object mapped to an input address store path affected downstream ([referrer](@docroot@/glossary.md#gloss-referrer) store objects, this would severely curtail --- if not remove outright --- the ability to mix and max input-addressed store objects.

The *closure digest* still always exists is a supplementary content hash which, contrary to the above, *will* be sensative to the mappings of input-addressed store objects.
It can therefore be used instead to pin the the exact mapping of input-addressed store paths, and thus pin down exact closures, compensating for the ambiguity of content address store paths described above.

[input-addressed]: @docroot@/glossary.md#gloss-input-addressed-store-object
[fso-ca]: ../file-system-object/content-address.md
[Nix Archive]: ../file-system-object/content-address.md#serial-nix-archive
[sp-spec]: @docroot@/protocols/store-path.md
[xp-feature-git-hashing]: @docroot@/development/experimental-features.md#xp-feature-git-hashing
