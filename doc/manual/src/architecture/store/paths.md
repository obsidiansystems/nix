# Store Path

A store path is a pair of a 20-byte digest and a name.

## String representation

A store path is rendered as the concatenation of

  - a store directory

  - a path-separator (`/`)

  - the digest rendered as Base-32 (20 arbitrary bytes becomes 32 ASCII chars)

  - a hyphen (`-`)

  - the name

- FEEDBACK: Thank you for providing the parenthesized detail on the digest!

Let's take the store path from the very beginning of this manual as an example:

    /nix/store/b6gvzjyb2pg0kjfwrjmg1vfhh54ad73z-firefox-33.1

This parses like so:

    /nix/store/b6gvzjyb2pg0kjfwrjmg1vfhh54ad73z-firefox-33.1
    ^^^^^^^^^^ ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ ^^^^^^^^^^^^
    store dir  digest                           name

We then can discard the store dir to recover the conceptual pair that is a store path:

    {
      digest: "b6gvzjyb2pg0kjfwrjmg1vfhh54ad73z",
      name:   "firefox-33.1",
    }

- FEEDBACK: Perfect example.

### Where did the "store directory" come from?

If you notice, the above references a "store directory", but that is *not* part of the definition of a store path.
We can discard it when parsing, but what about when printing?
We need to get a store directory from *somewhere*.

The answer is, the store directory is a property of the store that contains the store path.
The explanation for this is simple enough: a store is notionally mounted as a directory below `/nix/store/`, and the store object's root file system is likewise mounted at this path within that directory.

- FEEDBACK: Expanding on "likewise mounted at this path": This would be good place to show (maybe 2 sufficiently different) examples of a nix store directory, and an object's root file system. 

This does, however, mean the string representation of a store path is not derived just from the store path itself, but is in fact "context dependent".

- FEEDBACK: I think this would be a good place to link to a definition within nix (source code) for a reader that wants to get into the gritty details of path and root file system determination.

- FEEDBACK: I'm not sure what else I'd like to know, but I'd like more details on what exactly "context dependent" means.

## The digest

The calculation of the digest is quite complicated for historical reasons.
The details of the algorithms will be discussed later once more concepts have been introduced.
For now, we just concern ourselves with the *key properties* of those algorithms.

::: {.note}
**Historical note** The 20 byte restriction is because originally a digests were SHA-1 hashes.
This is no longer true, but longer hashes and other information are still boiled down to 20 bytes.
:::

Store paths are either *content-addressed* or *input-addressed*.

::: {.note}
The former is a standard term used elsewhere.
The later is our own creation to evoke a contrast with content addressing.
:::

*Content addressing* means that the store path digest ultimately derives from referred store object's contents, namely its file system objects and references.
There is more than one *method* of content-addressing, however.
Still, if one knows which content addressing schema was used,
(or guesses, there aren't that many yet!)
the store path can be recalculated, verifying the store object.

*Input addressing* means that the store path digest derives from how the store path was produced, namely the "inputs" and plan that it was built from.
Store paths of this sort can *not* be validated from the content of the store object.
Rather, the store object might come with the store path it expects to be referred to by, and a signature of that path, the contents of the store path, and other metadata.
The signature indicates that someone is vouching for the store object really being the results of a plan with that digest.

- FEEDBACK: "store object might come with": Can an example demonstrating the "how" of "come with" be added here? I looked back at the pseudo-code example given in `/concrete/object.md` and don't see how the "store path it expects to be referred to by" fits into that. Also since we are getting into the details of this anyways, showing that same (or updated) pseudo-code would lessen the working memory for the reader, improving readability.

- FEEDBACK: Depending on how ^^^ is done, the phrasing of "the store path it expects to be referred to by" could be changed, it took 2 passes for me to understand it.

While metadata is included in the digest calculation explaining which method it was calculated by, this only serves to thwart pre-image attacks.

- FEEDBACK: I think the pattern here is every time "it was ... by" is written, the "by" part should be moved to the beginning of the sentence. In this case, "The method of calculation is included in a digest's metadata. However, this only serves as a preventative measure against pre-image attacks."

Additionally, the metadata is scrambled with everything else so that it is difficult to tell how a given store path was produced short of a brute-force search.

- FEEDBACK: How is the metadata scrambled? (An inline link to some other documentation would suffice)

In the parlance of referencing schemes, this means that store paths are not "self-describing".
