# The abstract model

The abstract model is inspired by functional programming in the following key ways:

- Store objects are immutable.

- References to store objects are not under direct user control.

- References are *capabilities* in that objects unreachable by the reference to any operation cannot matter for that operation.
    - FEEDBACK: What is the definition of "operation" here? A function call, an operation on a set, a derivation?
    - FEEDBACK: I think rewording this point in terms of scope or context would clarify it. "A reference cannot pull in context that is not accessible by...". Saying that a reference is a "capability" implies that a reference is a function (or a typeclass?) which is missing the intent here.

- Building always creates "fresh" store objects, never conflicting with other builds.
    - FEEDBACK: Word choice: By "Building" do you mean the "nix-build" command or any nix command that builds a derivation? I would also consider changing "other" to "previous".

These properties have many positive ramifications, which we will go over in detail in the sections on the abstract model.
