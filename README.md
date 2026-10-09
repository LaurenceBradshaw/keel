# Keel

A systems language that keeps C++'s notation and throws away its object model.

Keel compiles to portable C11, which is then handed to `cc`. The source extension is `.kl`, the
compiler is `keelc`.

```cpp
// A class owns what it allocates, and its destructor is the one place it is freed.
class Buffer
{
    i32[*] data;
    u64    count;

    Buffer( u64 n )
    {
        count = n;
        unsafe { data = alloc<i32>( n ); }   // raw memory is a block you can see
    }

    ~Buffer()
    {
        unsafe { free( data ); }
    }

    public void set( u64 i, i32 value )
    {
        assert( i < count );
        unsafe { data[ i ] = value; }
    }

    public i32 get( u64 i ) const
    {
        assert( i < count );
        unsafe { return data[ i ]; }
    }

    public u64 size() const
    {
        return count;
    }
};

// Sum types with payloads, and a switch the compiler proves exhaustive.
enum Shape
{
    Circle( f64 radius ),
    Rect( f64 width, f64 height ),
};

f64 area( Shape s )
{
    switch( s )
    {
        case Shape::Circle( r ): return 3.0 * r * r;
        case Shape::Rect( w, h ): return w * h;
    }
}

// Generics are checked once, at the definition, against their bounds.
T larger<T>( T a, T b ) where T : Comparable
{
    return a < b ? b : a;
}

// The call site names everything a call does to the caller's variable.
void fill( ref Buffer b )                    // called fill( ref b ): may change b
{
    for( u64 i = 0; i < b.size(); i++ )
    {
        b.set( i, wrap<i32>( i ) );          // narrowing is spelled out
    }
}

i32 total( Buffer b )                        // called total( b ): a read-only borrow, never a copy; because Buffer owns
{                                            // if it were non-owning, it would be a copy
    i32 sum = 0;
    for( u64 i = 0; i < b.size(); i++ )
    {
        sum += b.get( i );
    }
    return sum;
}

void consume( move Buffer b )                // called consume( move b ): b is freed in here
{
}

i32 main()
{
    Buffer b = Buffer( 4 );

    fill( ref b );
    i32 sum = total( b );                    // 0 + 1 + 2 + 3
    consume( move b );                       // using b after this is a compile error

    f64 big = larger( area( Shape::Circle( 1.0 ) ), area( Shape::Rect( 2.0, 2.0 ) ) );

    return sum - 6 + ( big == 4.0 ? 0 : 1 );
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
the C backend all work, and the standard library has begun: `kl::list<T>` and `kl::string`, written in
Keel.

**Working today:** functions, control flow, fixed-width primitives, `struct` and `class`, methods,
constructors, destructors, access control, `const` fields, static methods and static fields,
payload-carrying `enum`s with exhaustive `switch`, generics by monomorphisation with definition-time
bound checking, function and constructor overloading, `operator==` and `operator[]`, function pointers,
method pointers and field types, single-item and many-item pointers, pointer to `const`, string
literals, `assert`, modules and packages, move checking on the IR's CFG, `unsafe`, `extern`, native
executables via `cc`, and a VS Code extension.

```cpp
import kl::list;
import kl::string;

i32 main()
{
    kl::list<kl::string> names = kl::list<kl::string>();
    names.push( move kl::string( "ada" ) );
    names.push( move kl::string( "grace" ) );
    names[1].append( kl::string( "!" ) );

    assert( names[0] == kl::string( "ada" ) );

    kl::string last = names.pop();
    return last.size() == 6 ? 0 : 1;
}
```

**Designed and not yet built:** error handling (`Result<T, E>`, a `try` prefix, and anonymous error
unions), interfaces with dynamic dispatch, the remaining operators, and most of the library, printing
included. Anything in that list appearing below is marked *(planned)*.

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
| Functions, methods and constructors overload, and selection is by exact type first. Only when nothing matches does an argument widen, to the closest candidate: the same kind (integer or float), then the narrowest, then the same signedness. A literal carries a family, not a type, so `f( 1 )` against `f( i32 )` and `f( i64 )` is ambiguous. | Every viable candidate is ranked across promotions, standard conversions and user-defined conversions. | One widening step ranked by one rule is enough for `print( i64 )` to take any integer, and a call either matches exactly or says why not. |
| Mixed-signedness and int/float comparisons are answered by cases. | Compiles, and answers thirteen pairs wrongly. | The one divergence in the permissive direction: it removes a silent wrong answer rather than creating one. |
| `cast<T>( x )` preserves the value, and aborts at run time if it does not fit; `wrap<T>( x )` keeps the low bits. Neither does float↔int or int↔bool. | `static_cast`, C casts, implicit narrowing. | The failure policy is the interesting part, and one spelling hides which you meant. |
| A leading zero on a decimal is an error: `010` does not compile. | `010` is octal, so it is 8. | Keel has no octal, so accepting it would silently change the value of valid C++. |
| `'A'` is a `u8` holding one byte. | `char` exists; `'ab'` is implementation-defined. | There is no `char` to give them, and inventing one for literals would be a second spelling for `u8`. |
| A string literal is a `str`: its bytes and their count, read-only. `kl::string` owns and grows. | `const char[N]`, decaying to a pointer, its length found by a `strlen`. | The length travels with the bytes, so nothing walks to a terminator, and a `\0` inside is just a byte. |
| `const` goes before what it applies to: `const T*` points to `const`, `T* const` cannot be reseated. `T const*` is an error naming `const T*`. | Both orders mean the same. | One order for one meaning. |
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
| No headers and no preprocessor; `#include` is an error directing to `import`. | Textual inclusion. | A header is not a translation unit, which is why C++ needs `namespace` as a second mechanism. A module is one. |
| `import kl::list;` loads a module; the package is the namespace, so its names are written `kl::list<i32>`. Imports are not transitive, and there is no `using`. | `#include`, `namespace` and `using`, three mechanisms. | A file's imports list what it uses, and a library name can never collide with a local one. |
| A few declarations every program needs, `str` among them, live in a prelude seen without an import. A program's own declaration of the same name shadows it, except a function's overloads, which fall back: a call goes to the prelude's set only when the program's has no candidate for it, so declaring `print( Point )` leaves `print( "hi" )` working. `--print-prelude` shows it. | The language and `std` are separate; overloads in different namespaces are merged into one set. | Adding to the prelude must never break a program that already used the name, and a merged set would let a closer prelude overload silently take a call. |
| `assert( cond )` is a keyword, always on, and a failure prints the file, line and condition, then aborts without running any destructor. | A macro that `NDEBUG` removes. | There is no debug mode yet to turn it off in, and destructors run on state the program has just said is broken would make things worse. |

### Aggregates

| Keel | C++ | Why |
| --- | --- | --- |
| A `struct` is a type whose representation is its interface: public fields, trivially copyable, no destructor, no owning members. A `class` hides its representation: it may own resources, have a destructor, and is moved rather than copied. | The two differ only in default access. | The compiler makes the distinction C++ spends style guides teaching. Which one you wrote says what the type does. |
| A `class`'s fields are private by default, its methods and constructors public. Access is written on the member — `private i32 x;` — and a `struct` has no private members. | `private:` and `public:` section labels. | A label is parser state outliving its declaration. A class hides its representation, and a method is not its representation. |
| A struct literal must initialise every field. | Missing fields are value-initialised. | Adding a field otherwise changes what every existing construction site silently builds. |
| `.` is the only member-access operator and reaches through a pointer; `->` is an error. | Both, because `.` on a pointer means nothing. | Generics: `thing.x` works whether `T` is a value, a borrow or a pointer. |
| A `const` field is assigned once by each constructor, and never written again, but the whole object may still be replaced. | A `const` member deletes assignment for the whole class. | Otherwise a type holding one, `str` included, could never be reassigned. |
| An operator is a method, never a free function. `==` and `[]` exist so far; `!=` is derived from `==`. | Either, and `!=` is declared separately. | One place to look, and two operators that cannot disagree. |
| `operator[]` returns a `T*`, and `v[i]` is the element it points at. | Returns `T&`. | An element of a growable buffer can dangle while its container lives. A `ref` is the spelling that cannot dangle, and a pointer the one that can. |
| One `enum` keyword with `enum class`'s semantics: always scoped, never an integer. `enum class E` is an error saying to drop the `class`. | Two kinds, one of which leaks into the integers. | Nobody wants the unscoped one. |
| `enum` variants carry payloads — `Shape::Circle( 1.0 )` — destructured in a `case`, and destroyed by reading the tag. | No sum types; `union` maintains its tag by hand. | A union is safe only when the tag is right every time, and this is the one legitimate use of one. |
| `switch` has no fallthrough, needs no `break`, and must be exhaustive. Running on is written `fallthrough;`. | Fallthrough by default; a missing case is a silent skip. | The default is backwards. The keyword keeps the ability without the hazard. |
| No inheritance. Interfaces with dynamic dispatch are *(planned)*: an `interface` of method signatures, conformance declared with `implements`, and no implementation inheritance. | Inheritance, virtual dispatch, multiple inheritance and RTTI. | Polymorphism needs a shared interface, not a shared base. RTTI and downcasting stay refused. |

### Ownership, references and memory

| Keel | C++ | Why |
| --- | --- | --- |
| Ownership transfers only where the call site says `move`. A bare argument is a copy for a `struct`, a read-only borrow for a `class`. | `consume( b )` copies, and a copy can be arbitrarily expensive. | `void consume( Buffer b )` reads as a copy to a C++ developer, so the transfer has to be visible where it happens. |
| Five argument forms, and the call site names every one that affects the caller's variable: `f( x )`, `f( move x )`, `f( ref x )`, `f( out x )`, and `const ref` called bare, because like `f( x )` it leaves your variable unchanged. | The signature decides, invisibly at the call. | You can see what a call does to your variables without opening the callee. |
| A borrow takes exactly its type: an `i32` cannot be passed as a `const ref i64` or an `out i64`, though a bare `i64` parameter widens it. | `const T&` binds a converted temporary; a non-`const` reference refuses. | A borrow is your variable itself, so there is nowhere for a widened copy to live, and an `out` would write eight bytes into four. |
| `ref` is a binding mode, not a type. `ref T x` replaces `T&`, which is an error in type position, leaving `&` to mean address-of only. | `T&` is a type, and `&` means two things. | Treating a reference as a type is what makes `T&&`, reference collapsing and `std::forward` necessary. |
| A `ref` is never reseated, may not be stored or captured, and may be returned only as a `const ref` derived from a reference parameter. | References may dangle, silently. | Dangling becomes unrepresentable by construction — no borrow checker and no lifetime annotations. |
| No `new`/`delete` in safe code. `alloc<T>( n )`, `free( p )` and `destroy( p, n )` are keywords needing `unsafe`: memory, its release, and ending the values in it are three separate steps. | `new`/`delete`, and owning raw pointers as an idiom. | An address says nothing about who frees it. Manual allocation is the exception, spelled as one. |
| `unsafe` is a block, and it permits operations rather than disabling checks. | Unmarked, and available everywhere. | The permission is always a pair of braces you can see, and everything checked outside one is checked inside. |
| `T*` points at exactly one live `T` and has no arithmetic. `T[*]` points at many slots of raw storage; indexing it and `+` need `unsafe`. | Any pointer is an iterator. | `p + 1` on a single-item pointer is not dangerous, it is nonsense. |
| Writing through a `T*` destroys the old value first; writing a `T[*]` slot initialises it. | `*p = x` assigns, and raw memory needs placement `new`. | The pointer's type already says whether a live value is there. |
| An owning value leaves a place only by `move`, and only a local, a parameter or a raw slot may be moved out. `return field;` is an error. | Copies, or moves anything `std::move` names. | A field or an element outlives the return, so copying it would destroy it twice, and moving it would leave a hole its owner still counts. |
| `extern` declares a C function, is not mangled, and calling one needs `unsafe`. | `extern "C"`, and calls are unchecked but unmarked. | The FFI boundary is where the compiler's guarantees stop, so the marker goes there. |
| No exceptions and no unwinding. Errors are `Result<T, E>`, propagated with a `try` prefix *(planned)*. | Exceptions and exception safety as a discipline. | An invisible second path through every call is the feature most C++ code bases turn off. |

### Generics

| Keel | C++ | Why |
| --- | --- | --- |
| Type parameters go on the name, bounds in a trailing `where`: `T max<T>( T a, T b ) where T : Comparable`. `template` is an error naming the replacement. | `template<typename T>` on a preceding line. | Both new forms are hard errors in C++, so this is new notation rather than a reinterpretation. |
| Bounds come from a closed, compiler-provided set, joined with `&`. | Concepts: open, structural, arbitrarily complex. | A closed set can be checked completely at the definition; an open one cannot. |
| Generics are checked at the definition, not at the instantiation. | Both, and the interesting errors surface at the expansion. | Errors point at the generic you wrote, not at the expansion you did not. |
| Monomorphised. | Monomorphised. | No divergence — this one C++ got right. |

### Function and member pointers

| Keel | C++ | Why |
| --- | --- | --- |
| A function type is written `fn( i32, i32 ) -> i32`, and a parameter's mode is part of it. | `int (*)( int, int )`, a declarator with the name in the middle. | `fn( ref i32 )` and `fn( i32 )` are different types, and a C declarator has nowhere to put a mode. It also reads left to right. |
| `&add` on an overloaded name is chosen by the type it is stored as; `auto f = &add;` is then an error. A generic's address names its arguments: `&id<i32>`. | The same selection, plus deduction from the target. | The expected type is the one thing that can choose, so it is the only rule. |
| A method pointer is an ordinary function pointer with the receiver as parameter 0: `&C::get` is `fn( const ref C ) -> i32`, called `p( obj )`. A static method's, `&C::make`, has no receiver. | A separate member-function-pointer type, often fat, called `( obj.*p )()`. | Without inheritance or virtual dispatch there is nothing to adjust, so one kind of function pointer and one call syntax are enough. |
| A member data pointer is `field( C ) -> T`: an offset, applied as `p( obj )`, returning a copy. It is read-only. | `T C::*`, applied with `.*` or `->*`, writable. | `->*` went with `->`. Every use found in real code reads. Writing waits on a returned `ref` becoming a writable place. |

---

## Building

Requires CMake 3.24+, Ninja, clang-18, and vcpkg (for `fmt`, `cxxopts` and `catch2`).

```sh
cmake --preset debug
cmake --build build/debug
```

Presets are `debug`, `asan` (ASan + UBSan) and `release`.

`keelc` links every program against the runtime library, and looks for it where an install would put
it: `/usr/local/lib/keel/libkeel_rt.a`. Nothing installs it yet, so link the release build's there
once:

```sh
sudo mkdir -p /usr/local/lib/keel
sudo ln -s "$PWD/build/release/keel_rt/src/libkeel_rt.a" /usr/local/lib/keel/libkeel_rt.a
```

## Using

```sh
build/debug/bin/keelc examples/hello.kl -o hello && ./hello
```

A program using the library names its package:

```sh
build/debug/bin/keelc --package kl=keel_stl/src prog.kl -o prog
```

| Flag | Effect |
| --- | --- |
| `-o <path>` | Write the executable here (default: the input's stem) |
| `--check` | Run the front end and report diagnostics, emitting nothing |
| `--diagnostics=json` | Report diagnostics as JSON lines on stdout, for an editor |
| `--names` | With `--diagnostics=json`, also report what each name refers to |
| `--declarations` | With `--diagnostics=json`, also list every declaration with its signature and doc |
| `--dump-tokens` / `--dump-ast` / `--dump-kir` | Print that stage and stop |
| `--emit-c` | Print the generated C and stop |
| `--print-prelude` | Print the prelude every program sees, and exit |
| `--runtime <path>` | Link this runtime library instead of the installed one |
| `--package name=<dir>` | Make a package importable as `name`; may be repeated |

`keelc` compiles the generated C with `$CC` (default `cc`), adding `$CFLAGS`.

[`editors/vscode`](editors/vscode) is a VS Code extension: highlighting, keelc's errors underlined on
save, names coloured by what they refer to, hovers, and go-to-definition. Its README says how to install it.

`keeldoc` writes a package's documentation as HTML pages from its `///` and `//!` comments. The
library's are in [`keel_stl/docs`](keel_stl/docs); after changing a doc comment, regenerate them:

```sh
build/debug/bin/keeldoc -o keel_stl/docs kl=keel_stl/src
```

## Testing

```sh
build/debug/bin/keel_tests                              # unit tests
keelc/test/run_tests.sh build/debug/bin/keelc           # golden-file tests
build/debug/bin/keeldoc_tests                           # keeldoc's unit tests
keeldoc/test/run_tests.sh build/debug/bin/keeldoc build/debug/bin/keelc   # keeldoc's pages
```

The golden runner links the runtime built beside the `keelc` it is given. Under the `asan` preset,
point `KEEL_RT` at a library built without sanitizers, such as the `debug` one. Set `KEEL_VALGRIND=1`
to run every executed fixture under valgrind.

Unit tests live in the source file they test, behind `ENABLE_UNIT_TESTS`. Golden-file tests under
`keelc/test/` cover language behaviour: one `.kl` fixture per topic, with its expected diagnostics,
exit code or output beside it.

---

## Layout

```
keelc/src/lex        hand-written lexer, one switch, interns identifiers
keelc/src/parse      recursive descent, Pratt for expressions, the module loader
keelc/src/ast        flat vectors, u32 handles, a span on every node
keelc/src/sema       name resolution, bidirectional type checking, overloads, generics
keelc/src/ir         lowering to KIR and simplification
keelc/src/check      move checking and definite assignment over the KIR CFG
keelc/src/codegen_c  KIR to C11
keelc/src/prelude    the prelude, Keel source built into the compiler
keel_rt              the runtime floor (allocation, aborts)
keel_stl             the standard library, the `kl` package, and its pages in docs/
keeldoc              the doc tool: keelc's --declarations to HTML pages
editors/vscode       the VS Code extension
```

The C backend is not the portability boundary — KIR is. It is kept clean enough that another backend
could consume it, and eventually one will, most likely LLVM.

## Documents

- [`docs/PLAN.md`](docs/PLAN.md) — the engineering plan, as the rules stand. Every decision above is
  a numbered entry there with its reasoning. §6.3 is the complete divergence list and the audit
  surface for the containment rule, §6.7 holds the designs that are not divergences, §9 the
  milestones, and §15 the open work.
- [`docs/MANIFESTO.md`](docs/MANIFESTO.md) — the vision document that started it. Deliberately
  unresolved on syntax and semantics; the plan inverts its phase ordering on purpose and says why.

## License

Copyright 2026 Laurence Bradshaw. Keel is licensed under the Apache License 2.0 with LLVM Exceptions
(`Apache-2.0 WITH LLVM-exception`); see [`LICENSE`](LICENSE). The exception means a program compiled
with Keel owes nothing for the runtime and library code built into it.
