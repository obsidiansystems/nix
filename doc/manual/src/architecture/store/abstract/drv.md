# Abstract Derivation and Derived Reference

Abstract derivations and derived references are a mutually recursive concepts.
  - FEEDBACK: What does "mutually recursive" mean? This currently could be interpreted as "derivations can only depend on derived references and derived references only on derivations".
Derivations are recipes to create store objects.

Derivations are the heart of Nix.
Other system (like Git or IPFS) also store and transfer immutable data, but they don't concern themselves with *how* that data was created.
This is where Nix comes in.
- FEEDBACK: Consider rewriting to something like: "The critical difference between these systems and nix is that this history of data construction is preserved"

Derived references are a generalization of store object references, by weakening the restriction that objects must exist and and allowing reference to yet-to-be-built store objects.

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

We elaborate on the two types (previously existing, yet-to-exist) of references above:

**Opaque references**
An opaque reference is simply a store object reference.
Per the *no dangling* rule already discussed, only references to store objects that already exist are permitted.

**Built reference**
A built reference is a pair of a derivation reference and an output.
- FEEDBACK: Avoiding pronouns in technical documentation may seem redundant, but it improves clarity for confused readers who are not comfortable or used to the terminology.
- FEEDBACK: What are the types of these objects? This may seem obvious, but this is a good place to have a "sanity check".
They indirectly refer to the store output with the given name that will be produced by the given derivation.
- FEEDBACK: When writing about operations on ordered elements, preserving the order of the structure in the explanation improves precision. This would be easier to read if written like "A built reference is like a contract, stating that: given a derivation, a store output with the specified name will be produced."  
Since the store output is not directly referenced, the *no dangling* rule is impacted by this.
- FEEDBACK: Wouldn't the output be impacted by the no dangling rule, not the rule by the output?
- FEEDBACK: How exactly is it impacted? This is another good chance to spell it out for another "sanity check".
Since these mechanisms preserve the functional invariants of the nix store, store objects that are not yet built can be referenced using built references.

## Extending the model to be higher-order

- FEEDBACK: I am assuming this is a stub?
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
