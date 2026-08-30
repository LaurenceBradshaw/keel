# C++2

## Project Plan, Design Goals, and Language Vision

## 1. What is C++2?

C++2 is a proposed full reboot of what C++ could have been if it had been designed with decades of hindsight.

The goal is **not** to make C++ slightly safer or to add another layer of features on top of the existing language.

The goal is to design a new systems programming language that preserves the best properties of C++ while making its difficult, historically accumulated parts unnecessary.

The central question is:

> What should a systems programming language look like if we designed it today, while retaining C++'s fundamental advantages?

C++2 should be:

- Statically typed.
- Compiled to native machine code.
- Suitable for systems programming.
- Zero-cost in its abstractions.
- Deterministic in resource management.
- Explicit about ownership.
- Safe by default.
- Powerful in generic programming.
- Designed around coherent value semantics.
- Supported by a standard library designed as a whole.
- Capable of interoperating with C and existing systems software.
- Much smaller and more internally consistent than modern C++.

C++2 should be a **new language**, not a new version of C++.

Compatibility with C and C++ should be provided through interoperability layers where practical, rather than allowing historical compatibility requirements to dictate the language itself.

---

# 2. Why C++2?

C++ is extremely capable, but much of its complexity comes from historical evolution rather than deliberate design.

Modern C++ programmers are expected to understand rules such as:

- Prefer RAII.
- Do not use raw owning pointers.
- Use `unique_ptr` for exclusive ownership.
- Use `shared_ptr` only when shared ownership is actually required.
- Avoid dangling references.
- Avoid dangling `string_view`s.
- Understand move semantics.
- Understand the Rule of 0/3/5.
- Understand iterator invalidation.
- Understand exception safety.
- Understand object lifetime.
- Understand undefined behaviour.
- Understand template constraints and substitution.
- Understand data races.
- Understand allocator behaviour.
- Understand when objects are copied, moved, constructed and destroyed.

Many of these are effectively additional language rules that exist primarily in programmers' heads, coding guidelines and static-analysis tools.

C++2 should ask:

> Why does the programmer have to know this?

Where a rule can be made part of the language's type system, ownership model or compiler, it should be.

The aim is not to remove programmer control.

The aim is to make the safe and obvious thing the natural thing to write.

---

# 3. Core Principles

C++2 should have a small set of hard principles that guide the entire design.

## 3.1 Deterministic resource management

Resources have deterministic lifetimes.

RAII is fundamental to the language rather than merely an idiom.

Resources can include:

- Memory.
- Files.
- Sockets.
- Mutexes.
- Database connections.
- GPU resources.
- OS handles.
- Transactions.
- Mapped memory.
- Temporary allocations.

If an object owns a resource, destruction of that object releases the resource.

---

## 3.2 Values are the default

Objects should normally behave as values.

```cpp
Point a = Point(10, 20);
Point b = a;
```

The programmer should not have to think about heap allocation merely because they created an object.

Stack allocation, embedded objects and compiler optimisation should be the normal implementation strategy.

---

## 3.3 Ownership is explicit

The language should distinguish:

- Owned values.
- Exclusive ownership.
- Shared ownership.
- Non-owning references.
- Nullable references/pointers.

The exact syntax is not yet fixed.

A possible model is:

```text
T              owned value
T&             non-owning, non-null reference
T?             optional value
T*             raw/unsafe pointer
Owned<T>       exclusive ownership
Shared<T>      shared ownership
Weak<T>        non-owning reference to shared ownership
```

The important property is that ownership, nullability and borrowing are represented by types rather than by convention.

---

## 3.4 No mandatory garbage collector

C++2 should not require a tracing garbage collector.

The preferred direction is compiler-assisted ownership combined with deterministic destruction.

The compiler should understand object lifetimes where reasonably possible.

The programmer should not normally need to manually allocate and delete objects.

---

## 3.5 Zero-cost abstractions

High-level language constructs should not inherently imply runtime costs.

Generic code, RAII, wrappers, ranges and other abstractions should compile to the same or equivalent machine code as a carefully written low-level implementation when their semantics allow it.

---

## 3.6 Safe by default

Safe C++2 code should eliminate entire classes of bugs where practical.

Examples include:

- Use-after-free.
- Double-free.
- Dangling references.
- Invalid ownership transfers.
- Accidental null dereferences.
- Many data races.
- Invalid variant access.
- Unchecked error propagation.

Unsafe operations should still be possible for systems programming, but they should be explicit.

---

## 3.7 Generic programming is a first-class feature

Generic programming should be designed around constraints from the beginning.

Templates should not be a textual/metaprogramming mechanism that happens to support generic programming.

The language should have proper generic functions, types and constraints.

---

## 3.8 Modules instead of textual inclusion

C++2 should use real modules.

There should be no normal equivalent of:

```cpp
#include "foo.h"
```

Instead:

```cpp
import graphics;
```

Modules should be understood by the compiler rather than implemented as textual preprocessing.

This eliminates:

- Include guards.
- Macro leakage.
- Repeated parsing.
- Declaration duplication.
- Header/implementation synchronisation problems.
- Much of the complexity surrounding C++ build systems.

---

# 4. The Object and Lifetime Model

This is the most important part of the language design.

Syntax should come later.

Before designing keywords and punctuation, C++2 must answer:

- What is an object?
- What is a value?
- What owns an object?
- When is an object constructed?
- When is it destroyed?
- Can objects be copied?
- Can objects be moved?
- What does a reference mean?
- What does a pointer mean?
- What happens when an object escapes a scope?
- Where may objects live?
- How are lifetimes checked?
- How is ownership transferred?
- How can objects be shared?
- How are objects transferred between threads?

If the lifetime model is correct, much of the rest of the language can be derived from it.

---

# 5. Memory Management

## 5.1 Eliminate normal manual allocation

Normal application code should not need:

```cpp
Widget* widget = new Widget();
delete widget;
```

Instead:

```cpp
Widget widget;
```

The object has a deterministic lifetime.

Where dynamic allocation is genuinely required:

```cpp
Owned<Widget> widget = make<Widget>();
```

The allocator still exists underneath the language.

The important difference is that ordinary lifetime management is automatic.

---

## 5.2 Ownership transfer

Ownership should be explicit.

Conceptually:

```cpp
Owned<Widget> create_widget();

fn consume(Owned<Widget> widget) {
    ...
}
```

Passing `widget` to `consume` transfers ownership.

The compiler can therefore reason about the lifetime of the object.

---

## 5.3 Borrowing

Non-owning references should be represented separately.

```cpp
fn inspect(Widget& widget) {
    ...
}
```

The compiler should ensure that the reference cannot outlive its owner.

A reference should not silently become a dangling reference.

---

## 5.4 Shared ownership

Shared ownership should be explicit rather than the default.

```cpp
Shared<Widget> widget;
```

If shared ownership is reference counted, that implementation detail should be associated with the explicit shared ownership type.

Cycles should be handled deliberately through:

```cpp
Weak<Widget>
```

or another appropriate mechanism.

---

# 6. Classes and Structs

Classes remain an important concept, but their semantics should be simpler.

A class should primarily define:

- Data.
- Invariants.
- Methods.
- Construction.
- Destruction.
- Ownership of resources.

For example:

```cpp
class File {
    Path path;

    fn read() -> Result<String, IoError>;
}
```

RAII should be automatic.

```cpp
{
    File file = open_file("data.txt");
    // use file
}
// file is destroyed and the resource is released
```

There should be no need for a separate `close()` call in ordinary code when ownership represents the resource.

---

# 7. Value Semantics

Value semantics should be a central language concept.

For example:

```cpp
Point a = Point(10, 20);
Point b = a;
```

The meaning of the copy should be clear from the type.

If copying a type is expensive or meaningless, the type should be able to prohibit copying.

If a type can be moved but not copied, that should be straightforward to express.

The language should not require programmers to understand the C++ Rule of 0/3/5 merely to implement ordinary resource-owning types.

---

# 8. Algebraic Data Types

C++2 should provide proper sum types and pattern matching.

For example:

```cpp
enum Result<T, E> {
    Ok(T),
    Error(E)
}
```

A shape type could be:

```cpp
enum Shape {
    Circle(float radius),
    Rectangle(float width, float height),
    Triangle(float a, float b, float c)
}
```

Pattern matching:

```cpp
match shape {
    Circle(radius) => ...
    Rectangle(width, height) => ...
    Triangle(a, b, c) => ...
}
```

This removes the need for many combinations of:

- Enums plus unions.
- `std::variant`.
- Visitors.
- Virtual class hierarchies.
- Nullable pointers.
- Manual discriminants.

---

# 9. Error Handling

C++2 should have a coherent error model.

The preferred approach is explicit result types.

For example:

```cpp
fn read_file(path: Path) -> Result<String, IoError>
```

Errors should be easy to propagate:

```cpp
fn load_config(path: Path) -> Result<Config, ConfigError> {
    let text = read_file(path)?;
    return parse_config(text)?;
}
```

The `?` operator is conceptual syntax for propagating an error.

Exceptions could potentially exist for exceptional or unrecoverable situations, but they should not be the fundamental mechanism for ordinary application errors.

---

# 10. Generics

Generic programming should be a core part of the language.

A possible syntax:

```cpp
fn max<T: Comparable>(a: T, b: T) -> T {
    if (a > b)
        return a;
    return b;
}
```

Constraints should be checked explicitly and predictably.

The language should avoid the historical complexity of C++ templates where possible.

The aim is:

- Fast compilation.
- Clear diagnostics.
- Strong constraints.
- Predictable overload resolution.
- Powerful compile-time programming without turning the language into a second programming language.

---

# 11. Concurrency

Concurrency should be designed into C++2 rather than bolted onto it.

The language and standard library should provide coherent concepts for:

- Tasks.
- Threads.
- Structured concurrency.
- Async execution.
- Cancellation.
- Channels.
- Mutexes.
- Atomics.
- Shared state.
- Ownership across threads.

The compiler should make data races harder to express.

Ownership rules should extend naturally to concurrent programs.

---

# 12. The Standard Library

The standard library should be designed from scratch.

Do not create a second namespace containing direct copies of the existing C++ standard library.

The library should be coherent from the beginning.

Likely foundational types include:

```text
String
String_view
Vector
Array
Hash_map
Tree_map
Set
Optional
Result
Variant
Tuple
Span
Range
Path
File
Socket
Thread
Task
Mutex
Atomic
Time
Date
Regex
Random
```

The final set should be determined by actual language and library requirements rather than attempting to reproduce all of `std`.

The standard library should demonstrate the intended style of the language.

If implementing `Vector`, `String` or `File` requires strange compiler-specific hacks, the language design should be reconsidered.

---

# 13. Interoperability

C++2 should be a new language, but it should be practical in an existing systems environment.

C interoperability should be a major goal.

For example:

```cpp
extern "C" fn legacy_function(...);
```

Existing C libraries should be usable without rewriting them.

C++ interoperability can be pursued separately, potentially through an ABI/interface boundary.

However, C++ compatibility must not dictate the core semantics of C++2.

The language should not inherit historical problems merely to preserve source compatibility.

---

# 14. What C++2 Should Avoid

C++2 should deliberately avoid reproducing historical complexity.

## Avoid source compatibility with C++

Do not inherit C++ syntax and semantics merely because programmers recognise them.

Familiarity is useful, but a reboot should be free to make better decisions.

## Avoid manual memory management as the normal model

`new` and `delete` should not be normal application-level constructs.

## Avoid textual inclusion

No normal header-file model.

## Avoid accidental complexity in generics

Generic programming should be explicit and constraint-based.

## Avoid making unsafe behaviour invisible

Raw pointers, unsafe casts and manual lifetime manipulation should be explicit.

## Avoid a giant language

Do not attempt to solve every problem before the core language works.

---

# 15. Project Phases

## Phase 0: Language Manifesto

Define the principles before implementation.

Deliverables:

- Language goals.
- Non-goals.
- Ownership philosophy.
- Object model.
- Safety model.
- Generic programming philosophy.
- Error-handling philosophy.
- Module model.
- Concurrency philosophy.
- Compatibility strategy.

The manifesto should become the decision-making framework for all later design decisions.

---

## Phase 1: Semantic Core

Design the semantics before syntax.

Focus on:

- Values.
- Objects.
- Lifetimes.
- Ownership.
- References.
- Moves.
- Copies.
- Destruction.
- Allocation.
- Classes.
- Functions.
- Generic types.
- Error values.

This is the highest-priority phase.

---

## Phase 2: Minimal Language

Implement a tiny usable language.

Initial feature set:

```text
Primitive types
Structs
Classes
Functions
References
Arrays
Enums
Pattern matching
Generics
Ownership
RAII
Modules
```

Do not add:

- Reflection.
- Coroutines.
- Huge metaprogramming systems.
- GUI frameworks.
- Exotic compiler features.

Get the core correct first.

---

## Phase 3: Compiler

A possible compiler architecture:

```text
Source
  |
  v
Lexer
  |
  v
Parser
  |
  v
AST
  |
  v
Type Checker
  |
  v
Ownership/Lifetime Checker
  |
  v
Intermediate Representation
  |
  v
LLVM
  |
  v
Machine Code
```

LLVM is a possible backend.

The C++2 frontend must own the language semantics.

LLVM should not define the language.

---

## Phase 4: Bootstrap the Standard Library

Implement foundational types in C++2 itself.

Initial candidates:

```text
Vector
String
String_view
Optional
Result
Array
Span
Hash_map
```

Then:

```text
Path
File
Time
Thread
Mutex
Atomic
Task
```

The standard library should be a major test of the language design.

---

## Phase 5: Real Programs

Build actual software early.

Targets should include:

- Command-line applications.
- HTTP server.
- Compiler components.
- Database components.
- Game/graphics application.
- Text processing tools.
- Systems utilities.

The purpose is not merely to demonstrate that the language works.

Real applications expose weaknesses in:

- Ownership.
- Error handling.
- Generic programming.
- Compile times.
- Module design.
- Library ergonomics.
- Concurrency.
- Debugging.

---

# 16. Example: Basic Program

A minimal program might eventually look like:

```cpp
fn main() {
    print("Hello, world!");
}
```

The exact syntax is intentionally not fixed yet.

The important property is that the language should have a very small conceptual distance between a trivial program and a production program.

---

# 17. Example: RAII

```cpp
fn process_file(path: Path) -> Result<(), IoError> {
    File file = open_file(path)?;

    process(file);

    return Ok(());
}
```

When the function exits, `file` is deterministically destroyed.

There is no explicit `close()` required for normal ownership-based resource management.

---

# 18. Example: Ownership

Conceptually:

```cpp
Owned<Widget> create_widget() {
    return make<Widget>();
}

fn use_widget(Owned<Widget> widget) {
    widget.run();
}
```

Ownership moves into `use_widget`.

The compiler knows which operation is responsible for destruction.

---

# 19. Example: Borrowing

```cpp
fn inspect(Widget& widget) {
    print(widget.name);
}

fn main() {
    Widget widget = make_widget();

    inspect(widget);
}
```

`inspect` does not own the object.

The compiler can verify that the reference remains valid for its entire use.

---

# 20. Example: Error Handling

```cpp
fn load_config(path: Path) -> Result<Config, ConfigError> {
    let text = read_file(path)?;
    let config = parse_config(text)?;

    return Ok(config);
}
```

Errors are explicit in the function's type.

Error propagation should remain concise.

---

# 21. Example: Algebraic Data Types

```cpp
enum Shape {
    Circle(float radius),
    Rectangle(float width, float height)
}

fn area(shape: Shape) -> float {
    match shape {
        Circle(radius) => return PI * radius * radius,
        Rectangle(width, height) => return width * height
    }
}
```

The compiler should know whether all possible variants have been handled.

---

# 22. Example: Generic Programming

```cpp
fn max<T: Comparable>(a: T, b: T) -> T {
    if (a > b)
        return a;

    return b;
}
```

Constraints are part of the function declaration.

Generic code should be statically resolved and have no inherent runtime overhead.

---

# 23. Example: Standard Library

A typical program should eventually be able to look like:

```cpp
fn main() -> Result<(), Error> {
    let users = read_file("users.json")?
        .parse_json<Vector<User>>()?;

    for user in users {
        print(user.name);
    }

    return Ok(());
}
```

The exact syntax can change.

The desired property is that the program expresses the programmer's intent directly rather than exposing memory management, iterator mechanics, ownership boilerplate and error plumbing.

---

# 24. Design Questions That Must Be Resolved

These are deliberately unresolved.

They should be treated as research questions rather than assumptions.

## Ownership

- How much ownership checking should be static?
- Should the model resemble Rust's borrow checker?
- Can a simpler ownership system provide most of the benefits?
- How should ownership interact with inheritance?
- How should ownership interact with exceptions?
- How should ownership interact with coroutines?
- How should ownership work across threads?

## Classes

- Should inheritance exist?
- Should inheritance be limited to interfaces?
- Should composition be strongly preferred?
- Should virtual dispatch require explicit syntax?
- Should classes and structs actually be distinct?

## Memory

- Should all allocations be visible to the type system?
- Should stack and heap placement be semantically invisible?
- How should custom allocators work?
- How should arenas and pools be represented?
- How should embedded systems disable or replace the normal allocator?

## Generics

- How powerful should compile-time programming be?
- Should there be compile-time reflection?
- How should specialisation work?
- How should overload resolution work?

## Safety

- What exactly constitutes "safe" C++2?
- What operations require an `unsafe` block?
- Can all memory unsafety be isolated to unsafe code?

## ABI

- What is the C++2 ABI?
- How stable should it be?
- Should the ABI be standardised?
- How can libraries compiled by different compilers interoperate?

---

# 25. Success Criteria

C++2 should not be considered successful merely because it can compile C++-like code.

A successful C++2 should make the following kinds of statements unnecessary:

> "Don't forget to delete this."

> "This pointer is actually owned by..."

> "Don't return this reference."

> "This `string_view` becomes invalid after..."

> "You need to use `unique_ptr` here."

> "Don't copy this object."

> "This function can throw."

> "Make sure you hold the mutex before calling this."

> "Don't use this iterator after modifying the vector."

> "This API takes ownership, but it isn't obvious from the type."

The compiler should understand as many of these facts as possible.

---

# 26. The Central Idea

The project should not aim merely to create:

> C++ but safer.

The stronger goal is:

> **A language where the things C++ programmers currently have to know as folklore are enforced by the language.**

C++ has many excellent ideas.

Its greatest weakness is that programmers have to combine those ideas correctly themselves.

C++2 should turn the accumulated lessons of decades of systems programming into the language's default model.

The most important design principle is therefore:

> **Make the correct program the natural program.**

And the most important implementation priority is:

> **Design the ownership and object lifetime model before designing the syntax.**

Once the lifetime model is correct, many other decisions follow naturally:

```text
Ownership
    |
    +-- Object lifetime
    |
    +-- RAII
    |
    +-- References
    |
    +-- Pointers
    |
    +-- Moves and copies
    |
    +-- Containers
    |
    +-- Error handling
    |
    +-- Concurrency
    |
    +-- Standard library
```

That should be the foundation of C++2.
