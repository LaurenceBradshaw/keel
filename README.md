# Keel

A systems language that keeps C++'s notation and throws away its object model.

Keel compiles to portable C11, which is then handed to `cc`. The source extension is `.kl`, the
compiler is `keelc`.

```cpp
i32 add( i32 a, i32 b )
{
    return a + b;
}

// A class owns what it allocates. `alloc<T>()` and `free( p )` are keywords, and both need `unsafe`.
class Cell
{
    i32* slot;

    Cell()
    {
        unsafe { slot = alloc<i32>(); }
    }

    ~Cell()
    {
        unsafe { free( slot ); }
    }
};

// The call site names every form that does something to the caller's variable.
void consume( move Cell c )
{
}

void inspect( Cell c )   // a bare parameter of an owning type is a read-only borrow
{
}

i32 main()
{
    Cell c = Cell();

    inspect( c );        // c is still alive here
    consume( move c );   // c is dead from here, and the destructor runs in the callee

    return add( 1, 2 ) - 3;
}
```

---

## What it is for

C++ is extremely capable, and most of its difficulty is accumulated rather than designed. A working
C++ developer is expected to hold in their head: the Rule of 0/3/5, when a value is copied versus
moved, iterator invalidation, exception safety, dangling `string_view`s, integral promotion, two-phase
template lookup, and which of the dozen ways to spell a pointer means ownership. None of that is
essential to systems programming. It is what happens to a language that must stay compatible with
every version of itself.

Keel asks what a systems language would look like designed today, keeping the properties that make
C++ worth using:

- Compiled, statically typed, native, no garbage collector.
- Deterministic destruction. RAII is the resource model, not a convention.
- Zero-cost abstractions: nothing costs what you did not write.
- Value semantics as the default, with ownership transfer written at the point it happens.
- Generics as a first-class feature rather than textual substitution.
- Straightforward interoperation with C.

and discarding the parts that are historical: implicit copies, the preprocessor, exceptions, headers,
undefined behaviour as a routine consequence of ordinary code, and a type system that reports template
errors at the expansion instead of at the definition.

The name is the shipbuilding one. The keel is the first member laid down and the hull is built off it;
the ownership and lifetime model is that member here.

---

## Status

This is an in-progress compiler, not a released language. The front end, the IR, the move checker and
the C backend all work; the standard library does not exist yet.

**Working today:** functions, control flow, fixed-width primitives, `struct` and `class`, methods,
constructors, destructors, operators, access control, static methods, payload-carrying `enum`s with
exhaustive `switch`, generics by monomorphisation with definition-time bound checking, function and
constructor overloading, move checking on the IR's CFG, `unsafe`, `extern`, and native executables via
`cc`.

**Designed and not yet built:** modules and `import`, the standard library, `Result` and the `?`
operator, `String`, `Optional<T>`, many-item pointers (`[*]T`), inheritance and dynamic dispatch.
Anything in that list appearing below is marked *(planned)*.

---

## If you write C++

You should be able to read Keel without a tutorial. Declarations, `struct`, methods, constructors,
destructors, `if`/`while`/`for`/`switch`, `auto`, `const`, `template`-shaped generics, and both comment
forms are spelled as in C++. C declaration order is kept, so a type is written before the name it
declares and `T*` is postfix.

That is deliberate, and it is a trade. Familiar syntax with unfamiliar semantics is the one genuinely
dangerous combination: a C++ developer reading `void consume( Buffer b )` assumes a copy. So the
language is governed by one containment rule:

> **Where Keel's semantics differ from C++'s, the syntax must differ too, or the compiler must reject
> the C++ reading outright.**

Nothing that is valid C++ silently means something else in Keel. Every divergence below is either new
notation, or a hard error carrying a diagnostic that names the replacement. The single exception is
noted in its row.

---

## Divergences from C++, and why

### Types, literals and conversions

| Keel | C++ | Why |
| --- | --- | --- |
| Fixed-width primitives only: `i32`, `u64`, `f64`. `int`, `long`, `char` and `unsigned` are hard errors naming the replacement. | Sizes are a platform question. | One spelling per type. Aliases would mean every reader has to know both. |
| No lossy implicit conversions. A binary operator widens both sides to the smallest type that holds both, and is an error where C++'s result type would not. | Integral promotion and the usual arithmetic conversions. | The conversions that lose information are the ones nobody writes on purpose. |
| Mixed-signedness and int/float comparisons are answered by cases. | Compiles, and answers thirteen pairs wrongly. | The one divergence in the permissive direction: it removes a silent wrong answer rather than creating one. |
| `cast<T>( x )` preserves the value, `wrap<T>( x )` keeps the low bits. Neither does float↔int or int↔bool. | `static_cast`, C casts, implicit narrowing. | The failure policy is the interesting part, and one spelling hides which you meant. |
| A leading zero on a decimal is an error: `010` does not compile. | `010` is octal, so it is 8. | Keel has no octal, so accepting it would silently change the value of valid C++. |
| `'A'` is a `u8` holding one byte. String literals have no type yet *(planned: `String`)*. | `char` exists; `'ab'` is implementation-defined. | There is no `char` to give them, and inventing one for literals would be a second spelling for `u8`. |
| `_` joins `'` as a digit separator; `\x` takes exactly two hex digits. | `\x` is unbounded and silently overflows. | The separator is pure addition; the escape rule rejects what C++ accepts loosely. |
| `nullptr` is a literal typed by context, so `auto p = nullptr;` is an error. | `nullptr_t` is a type. | A literal with no type of its own keeps the conversion rules intact. |

### Declarations and statements

| Keel | C++ | Why |
| --- | --- | --- |
| Braces are mandatory on every `if`/`while`/`for` body. | Optional. | A parse error, not a reinterpretation. Kills the `goto fail;` class of bug. |
| `++` and `--` are statements, so `x = a[i++]` does not parse. | Expressions, with a pre/post distinction. | Removes every sequencing hazard at once. `a[i++] = i++` is undefined in C++ and inexpressible here. |
| An expression statement must have an effect. `a * b;` and `x.field;` are errors. | Legal, and warned about. | Also settles the `a * b;` declaration-versus-multiply ambiguity from syntax alone. |
| Known precedence warts need parentheses: bitwise with comparison, shift with arithmetic, `&&` with `\|\|`. | `a & b == c` parses as `a & (b == c)`. | Changing the precedence would silently alter valid C++; requiring parentheses names both readings. |
| `*` and `&` bind to the type and must touch it: `u32 *p;` is an error. | Binds to the name, so `int *p, q;` makes `q` an `int`. | A wart every C style guide already works around. |
| Top-level declarations are visible throughout the file. | Needs a forward declaration. | Accepts what C++ rejects, and never changes the meaning of anything accepted. |
| A declaration may not shadow another reachable by the same name. | Legal at every scope. | Shadowing is a bug more often than a technique. Function bodies are barriers, so file-scope names may be reused. |
| Reading an uninitialised variable is a compile error. | Undefined behaviour. | Strictly rejects programs C++ accepts. |
| `main` is `i32 main()`; any other signature is an error. | Several signatures. | Anything else produces a `cc` error in generated code instead of a diagnostic in yours. |
| No headers and no preprocessor; `#include` is an error directing to `import` *(planned)*. | Textual inclusion. | A header is not a translation unit, which is why C++ needs `namespace` as a second mechanism. A module is one. |

### Aggregates

| Keel | C++ | Why |
| --- | --- | --- |
| A `struct` is a type whose representation is its interface: public fields, trivially copyable, no destructor, no owning members. A `class` hides its representation: it may own resources, have a destructor, and is moved rather than copied. | The two differ only in default access. | The compiler makes the distinction C++ spends style guides teaching. Which one you wrote says what the type does. |
| A `class`'s fields are private by default, its methods and constructors public. Access is written on the member — `private i32 x;` — and a `struct` has no private members. | `private:` and `public:` section labels. | A label is parser state outliving its declaration. A class hides its representation, and a method is not its representation. |
| A struct literal must initialise every field. | Missing fields are value-initialised. | Adding a field otherwise changes what every existing construction site silently builds. |
| `.` is the only member-access operator and reaches through a pointer; `->` is an error. | Both, because `.` on a pointer means nothing. | Generics: `thing.x` works whether `T` is a value, a borrow or a pointer. |
| An operator is a method, never a free function. | Either. | One place to look. |
| One `enum` keyword with `enum class`'s semantics: always scoped, never an integer. `enum class E` is an error saying to drop the `class`. | Two kinds, one of which leaks into the integers. | Nobody wants the unscoped one. |
| `enum` variants carry payloads — `Shape::Circle( 1.0 )` — destructured in a `case`, and destroyed by reading the tag. | No sum types; `union` maintains its tag by hand. | A union is safe only when the tag is right every time, and this is the one legitimate use of one. |
| `switch` has no fallthrough, needs no `break`, and must be exhaustive. Running on is written `fallthrough;`. | Fallthrough by default; a missing case is a silent skip. | The default is backwards. The keyword keeps the ability without the hazard. |
| No inheritance or virtual dispatch in v0 — planned, unscheduled, not foreclosed. | Both, plus multiple inheritance and RTTI. | v0 has nothing that needs it. RTTI and downcasting stay refused. |

### Ownership, references and memory

| Keel | C++ | Why |
| --- | --- | --- |
| Ownership transfers only where the call site says `move`. A bare argument is a copy for a `struct`, a read-only borrow for a `class`. | `consume( b )` copies, and a copy can be arbitrarily expensive. | `void consume( Buffer b )` reads as a copy to a C++ developer, so the transfer has to be visible where it happens. |
| Five argument forms, and the call site names every one that affects the caller's variable: `f( x )`, `f( move x )`, `f( ref x )`, `f( out x )`, and `const ref` called bare. | The signature decides, invisibly at the call. | You can see what a call does to your variables without opening the callee. |
| `ref` is a binding mode, not a type. `ref T x` replaces `T&`, which is an error in type position, leaving `&` to mean address-of only. | `T&` is a type, and `&` means two things. | Treating a reference as a type is what makes `T&&`, reference collapsing and `std::forward` necessary. |
| A `ref` is never reseated, may not be stored or captured, and may be returned only as a `const ref` derived from a reference parameter. | References may dangle, silently. | Dangling becomes unrepresentable by construction — no borrow checker and no lifetime annotations. |
| No `new`/`delete` in safe code. `alloc<T>()` and `free( p )` are keywords needing `unsafe`. | `new`/`delete`, and owning raw pointers as an idiom. | An address says nothing about who frees it. Manual allocation is the exception, spelled as one. |
| `unsafe` is a block, and it permits operations rather than disabling checks. | Unmarked, and available everywhere. | The permission is always a pair of braces you can see, and everything checked outside one is checked inside. |
| No pointer arithmetic on `T*`, which points at exactly one `T` *(planned: `[*]T`)*. | Any pointer is an iterator. | `p + 1` on a single-item pointer is not dangerous, it is nonsense. |
| `extern` declares a C function, is not mangled, and calling one needs `unsafe`. | `extern "C"`, and calls are unchecked but unmarked. | The FFI boundary is where the compiler's guarantees stop, so the marker goes there. |
| No exceptions. Errors are `Result<T, E>` and the postfix `?` *(planned)*. | Exceptions and exception safety as a discipline. | An invisible second path through every call is the feature most C++ code bases turn off. |

### Generics

| Keel | C++ | Why |
| --- | --- | --- |
| Type parameters go on the name, bounds in a trailing `where`: `T max<T>( T a, T b ) where T : Comparable`. `template` is an error naming the replacement. | `template<typename T>` on a preceding line. | Both new forms are hard errors in C++, so this is new notation rather than a reinterpretation. |
| Bounds come from a closed, compiler-provided set, joined with `&`. | Concepts: open, structural, arbitrarily complex. | A closed set can be checked completely at the definition; an open one cannot. |
| Generics are checked at the definition, not at the instantiation. | Both, and the interesting errors surface at the expansion. | Errors point at the generic you wrote, not at the expansion you did not. |
| Monomorphised. | Monomorphised. | No divergence — this one C++ got right. |

---

## Building

Requires CMake 3.24+, Ninja, clang-18, and vcpkg (for `fmt`, `cxxopts` and `catch2`).

```sh
cmake --preset debug
cmake --build build/debug
```

Presets are `debug`, `asan` (ASan + UBSan) and `release`.

## Using

```sh
build/debug/bin/keelc examples/hello.kl -o hello && ./hello
```

| Flag | Effect |
| --- | --- |
| `--check` | Run the front end and report diagnostics, emitting nothing |
| `--dump-tokens` / `--dump-ast` / `--dump-kir` | Print that stage and stop |
| `--emit-c` | Print the generated C and stop |

`keelc` does not link the runtime yet, so a program that allocates needs `keel_rt` compiled alongside
it:

```sh
build/debug/bin/keelc --emit-c prog.kl > prog.c
cc -fwrapv -fno-strict-aliasing -std=c11 prog.c keel_rt/src/kl_rt.c -o prog
```

## Testing

```sh
build/debug/bin/keel_tests                              # unit tests
keelc/test/run_tests.sh build/debug/bin/keelc           # golden-file tests
```

Unit tests live in the source file they test, behind `ENABLE_UNIT_TESTS`. Golden-file tests under
`keelc/test/` cover language behaviour: one `.kl` fixture per topic, with its expected diagnostics,
exit code or output beside it.

---

## Layout

```
keelc/src/lex        hand-written lexer, one switch, interns identifiers
keelc/src/parse      recursive descent, Pratt for expressions
keelc/src/ast        flat vectors, u32 handles, a span on every node
keelc/src/sema       name resolution, bidirectional type checking, overloads, generics
keelc/src/ir         lowering to KIR and simplification
keelc/src/check      move checking over the KIR CFG
keelc/src/codegen_c  KIR to C11
keel_rt              the runtime floor (allocation, aborts)
keel_stl             the standard library, when it exists
```

The C backend is not the portability boundary — KIR is. It is kept clean enough that another backend
could consume it, and eventually one will, most likely LLVM.

## Documents

- [`docs/PLAN.md`](docs/PLAN.md) — the engineering plan. Every decision above is a numbered entry there
  with the reasoning, the alternatives, and what it cost. §6.3 is the complete divergence list and the
  audit surface for the containment rule; §15 is the live work tracker.
- [`docs/MANIFESTO.md`](docs/MANIFESTO.md) — the vision document that started it. Deliberately
  unresolved on syntax and semantics; the plan inverts its phase ordering on purpose and says why.
