# Abstract Derivation and Derived Reference

Abstract derivations and derived references are a mutually recursive concepts.

Derivations are recipes to create store objects.

Derivations are the heart of Nix.
Other system (like Git or IPFS) also store and transfer immutable data, but they don't concern themselves with *how* that data was created.
This is where Nix comes in.

Derived references are a generalization of store object references to allow referring to existing or yet-to-be-built store objects.

In pseudo-code:

```idris
data Derivation
data DerivationRef

type OutputName = String

inputs : Derivation -> Set DerivedRef

outputs : Derivation -> Set OutputName

data DerivedRef
  = OpaqueRef { ref : StoreObjectRef }
  | BuiltRef {
      drv    : DerivationRef,
      output : OutputName
    }
```

### Abstract Derivations

**Inputs**

**Multiple named outputs**

### Abstract Derived References

Two cases:

**Opaque references**
These are just store object references.
Per the *no dangling* rule already discussed, one can only refer directly like this to store objects that already exists.

**Built reference**
These are a pair of a derivation reference and an output.
They indirectly refer to the store output with the given name that will be produced by the given derivation.
Since the store output is not directly referenced, the *no dangling* rule is impacted by this.
Thus, store objects that are not yet built can be referred to this way.

## Extending the model to be higher-order

[RFC 92](https://github.com/NixOS/rfcs/pull/92)

```idris
parse : StoreObject -> Maybe Derivation
print : Derivation -> StoreObject
```

```idris
castRef0 : StoreObjectRef -> DerivationRef
```

```idris
castRef : DerivedRef -> DerivationRef
```
