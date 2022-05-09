# The abstract model

The abstract model is inspired by functional programming in the following key ways:

- Store objects are immutable.

- References to store objects are not under direct user control.
  References cannot be "forged" or "spoofed", nor can they by precisely controlled by fine-tuning the store paths to be referenced.

- References are *capabilities* in that objects unreachable by references of the inputs of any "operation" cannot possibly effect that operation.

    - FEEDBACK: What is the definition of "operation" here? A function call, an operation on a set, a derivation?

      RESPONSE: Totally opaque!

    - FEEDBACK: I think rewording this point in terms of scope or context would clarify it. "A reference cannot pull in context that is not accessible by...". Saying that a reference is a "capability" implies that a reference is a function (or a typeclass?) which is missing the intent here.

      RESPONSE: The intent is to match https://en.wikipedia.org/wiki/Capability-based_security

- Newly store objects creates "fresh" store objects, never conflicting with one another.
  For example, manually added store paths will never conflict with one another or with ones built from a plan.
  Furthermore, two different plans will never "clash" trying produce the same store object.
  New builds will not only avoid messing up old builds, but they can also be confident no future build will mess up them.

    - FEEDBACK: Word choice: By "Building" do you mean the "nix-build" command or any nix command that builds a derivation? I would also consider changing "other" to "previous".

    - RESPONSE: Good points. Actually, I meant both "past" and "future".
      Made that explicit.

These properties have many positive ramifications, which we will go over in detail in the sections on the abstract model.
