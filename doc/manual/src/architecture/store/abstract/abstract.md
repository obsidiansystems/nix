# The abstract model

The abstract model is inspired by functional programming in the following key ways:

- Store objects are immutable

- References to store objects are not under direct user control.

- References are *capabilities* in that objects unreachable by the reference to any operation cannot matter for that operation.

- Building always creates "fresh" store objects, never conflicting with other builds'

These properties have many positive ramifications, which we will go over in detail in the sections on the abstract model.
