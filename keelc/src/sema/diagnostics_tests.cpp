// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Programs `--check` diagnoses, each with every diagnostic it shows.

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

#include "check/pipeline_test_support.h"

namespace keel
{
namespace
{

using Lines = std::vector<std::string>;

} // namespace

TEST_CASE( "errors_absorption", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// One mistake, one diagnostic. Each line below is a single error, and the literal sitting beside
// the failure must not add a second complaint about having no type - the error already shown is
// the cause, and `nullptr` is the only literal with no default to fall back on.
struct Point { i32* address; };

i32* identity( i32* q )
{
    return q;
}

void nothing()
{
    return nullptr;
}

i32 main()
{
    i32 value = 1;

    if ( missing() != nullptr ) { return 1; }
    if ( nullptr != missing() ) { return 2; }

    identity( &value ) = nullptr;
    undeclared = nullptr;

    // One error: the uninitialised field is what the wrong name most likely meant, so the help names it.
    auto p = Point { .wrong = nullptr };

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "13:12-13:19 error: a `void` function cannot return a value",
            "20:10-20:17 error: `missing` is not declared",
            "21:21-21:28 error: `missing` is not declared",
            "23:5-23:23 error: cannot assign to this expression = `identity( &value )` is a value, not a place",
            "24:5-24:15 error: `undeclared` is not declared",
            "27:22-27:38 error: `Point` has no field `wrong` = `address` is not given"
        }
    );
}

TEST_CASE( "errors_access_control", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN D29, M7. A `class` is a type whose interface hides its representation: its fields are
// private unless a member says otherwise, and its methods, constructors and destructors are not -
// they are how one is used rather than what it holds. Every refusal below is about a *qualified*
// access, because a bare name that reaches a member is resolved inside that member's own type and
// so is always visible.

class Account
{
    i32 balance;

    private i32 secret() const
    {
        return balance;
    }

    private static i32 rate()
    {
        return 3;
    }

    // Its own members see everything, which is the half that must keep working.
    i32 report() const
    {
        return balance + secret() + rate();
    }
};

// A type with only a private constructor, which is the shape a named constructor exists for.
class Sealed
{
    i32 v;

    private Sealed( i32 n )
    {
        v = n;
    }

    static Sealed make( i32 n )
    {
        return Sealed( n );
    }
};

// A class with no constructor at all: every field is private by default, so the literal spelling
// has nothing left to write.
class Plain
{
    i32 x;
};

// Another type is as much an outsider as a free function is - visibility is per type, not per file.
class Snooper
{
    public i32 seen;

    i32 peek( const ref Account a ) const
    {
        return a.balance;
    }
};

i32 read_a_private_field( const ref Account a )
{
    return a.balance;
}

void write_a_private_field( ref Account a )
{
    a.balance = 1;
}

i32 call_a_private_method( const ref Account a )
{
    return a.secret();
}

i32 call_a_private_static_method()
{
    return Account::rate();
}

i32 call_a_private_constructor()
{
    Sealed s = Sealed( 1 );

    return 0;
}

i32 build_from_a_literal()
{
    Plain p = Plain { 1 };

    return p.x;
}

// Neither static method builds one from outside: one is private, the other returns something else.
class Locked
{
    i32 v;

    private Locked( i32 n )
    {
        v = n;
    }

    private static Locked make( i32 n )
    {
        return Locked( n );
    }

    static i32 count()
    {
        return 0;
    }
};

i32 call_a_constructor_with_no_way_round()
{
    Locked l = Locked( 1 );

    return 0;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "58:16-58:25 error: `balance` is private to `Account` = only that type's own members may name it",
            "64:12-64:21 error: `balance` is private to `Account` = only that type's own members may name it",
            "69:5-69:14 error: `balance` is private to `Account` = only that type's own members may name it",
            "74:12-74:20 error: `secret` is private to `Account` = only that type's own members may name it",
            "79:12-79:25 error: `rate` is private to `Account` = only that type's own members may name it",
            "84:16-84:22 error: `Sealed`'s constructor is private = build it through one of its own static methods instead",
            "91:15-91:20 error: `Plain` keeps `x` private, so it cannot be built from a literal = give it a constructor, and "
            "build it by calling that",
            "93:12-93:15 error: `x` is private to `Plain` = only that type's own members may name it",
            "119:16-119:22 error: `Locked`'s constructor is private"
        }
    );
}

TEST_CASE( "errors_address_globals", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// The addresses a global's initialiser may not take, one error each.
struct P
{
    i32 x;
    i32 y;
};

struct Box<T> where T : copyable
{
    T v;

    static i32 made = 0;

    static Box<T> of( T x )
    {
        return Box { x };
    }
};

T id<T>( T a ) where T : copyable
{
    return a;
}

extern i32 puts( u8* s );

const i32 N = 3;
i32[2]    pair = [ 1, 2 ];

i32*                  to_const       = &N;
fn( i32 ) -> i32      generic        = &id<i32>;
i32*                  generic_static = &Box<i32>::made;
fn( i32 ) -> Box<i32> generic_method = &Box<i32>::of;
fn( u8* ) -> i32      c_function     = &puts;
i32*                  element        = &pair[0];
field( P ) -> i32     offset         = &P::x;
i32*                  undeclared     = &nope;
i32                   also           = nope;

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "30:40-30:42 error: expected `i32*`, but got `const i32*` = a pointer to `const` never converts back to one that "
            "writes",
            "34:40-34:45 error: taking the address of `puts` needs an `unsafe` block = it is defined in C, so the compiler "
            "cannot check what it does with its arguments",
            "37:41-37:45 error: `nope` is not declared",
            "38:40-38:44 error: `nope` is not declared"
        }
    );
}

TEST_CASE( "errors_aggregate_clauses", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A method's clause cannot bound the aggregate's parameter: no conditional methods.
struct Cell<T> where T : copyable
{
    T v;

    bool same( const ref Cell<T> o ) const where T : equatable { return v == o.v; }

    bool with<U>( U u ) const where U : copyable, where T : comparable { return v < v; }
};

interface sink<T>
{
    void put( T x ) where T : copyable;
};

// The operator's help names the aggregate whose parameter it is.
struct Plain<T> where T : copyable
{
    T v;

    bool same( const ref Plain<T> o ) const { return v == o.v; }
};

i32 main() { return 0; }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "6:44-6:63 error: `T` is `Cell`'s parameter, so its bounds go on `Cell` = write the clause after `Cell`'s "
            "parameters, or make this method a free generic function",
            "6:73-6:81 error: no operator `==` for `T` and `T` = `==` needs `equatable`; add `equatable` to the `where` clause "
            "for `T` on `Cell`",
            "8:51-8:71 error: `T` is `Cell`'s parameter, so its bounds go on `Cell` = write the clause after `Cell`'s "
            "parameters, or make this method a free generic function",
            "8:81-8:86 error: no operator `<` for `T` and `T` = `<` needs `comparable`; add `comparable` to the `where` clause "
            "for `T` on `Cell`",
            "13:21-13:39 error: `T` is `sink`'s parameter, so its bounds go on `sink` = write the clause after `sink`'s "
            "parameters, or make this method a free generic function",
            "21:54-21:62 error: no operator `==` for `T` and `T` = `==` needs `equatable`; add `equatable` to the `where` "
            "clause for `T` on `Plain`"
        }
    );
}

TEST_CASE( "errors_aggregates", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN D29. A struct is a transparent aggregate and a class is a type with invariants; the line is
// trivial copyability, and a destructor is what breaks it. Each mistake below is one diagnostic.
class Buffer
{
    u8* ptr;

    ~Buffer()
    {
    }
};

// A destructor is what makes a type owning, so a struct cannot have one.
struct Bad_struct
{
    i32 x;

    ~Bad_struct()
    {
    }
};

// Nor can it contain one, transitively - destroying it would destroy the member.
struct Holds_owning
{
    Buffer b;
};

class Wrapper
{
    Buffer inner;
};

struct Holds_transitively
{
    Wrapper w;
};

// A pointer owns nothing, so this one is legal.
struct Holds_a_pointer
{
    Buffer* p;
};

// The name after the `~` must be the enclosing type's.
class Mismatched
{
    u8* ptr;

    ~Wrong()
    {
    }
};

// And there may be only one.
class Twice
{
    u8* ptr;

    ~Twice()
    {
    }

    ~Twice()
    {
    }
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "17:5-19:6 error: a struct cannot have a destructor = use `class` if this type owns a resource",
            "25:5-25:14 error: a struct cannot contain `Buffer`, which has a destructor = a struct is copied freely, so "
            "declare `Holds_owning` as a class if it owns this",
            "35:5-35:15 error: a struct cannot contain `Wrapper`, which owns a resource = a struct is copied freely, so "
            "declare `Holds_transitively` as a class if it owns this",
            "49:5-51:6 error: `~Wrong` does not name the enclosing type = write `~Mismatched`",
            "63:5-65:6 error: `Twice` already has a destructor = previous declaration is at: t.kl:59:5"
        }
    );
}

TEST_CASE( "errors_aliased_locals", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D54 through locals holding the borrows: reported at the call, not where each was taken.
interface Shape { i32 area() const; };
struct Sq implements Shape { i32 s; i32 area() const { return s; } };

i32 both( Shape* a, Shape* b ) { return a.area() + b.area(); }

i32 as_interface()
{
    Sq     x = Sq { 2 };
    Shape* a = &x;
    Shape* b = &x;
    return both( a, b );
}

i32 converted()
{
    Sq  x = Sq { 2 };
    Sq* a = &x;
    Sq* b = &x;
    return both( a, b );
}

i32 written()
{
    Sq x = Sq { 2 };
    return both( &x, &x );
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "12:21-12:22 error: one object is passed twice to this call, which could change it = `a` at 12:18 is the same "
            "object; the callee would see it under two names",
            "20:21-20:22 error: one object is passed twice to this call, which could change it = `a` at 20:18 is the same "
            "object; the callee would see it under two names",
            "26:22-26:24 error: one object is passed twice to this call, which could change it = `&x` at 26:18 is the same "
            "object; the callee would see it under two names"
        }
    );
}

TEST_CASE( "errors_aliased_parameters", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D54: a callee borrows through one parameter and changes another, so a call may not pass one
// object to both when either may change it.
class Buf
{
    u64* data;

    Buf( u64 n )
    {
        unsafe { data = alloc<u64>(); }
        *data = n;
    }

    ~Buf()
    {
        unsafe { free( data ); }
    }

    u64 get() const
    {
        return *data;
    }
};

enum Holder
{
    Full( Buf b ),
    Empty
};

class buffer
{
    buffer()
    {
        unsafe { data = alloc<i32>( 4 ); data[0] = 1; }
    }

    ~buffer()
    {
        unsafe { free( data ); }
    }

    public ref i32 operator[]( u64 index ) const
    {
        unsafe { return data[index]; }
    }

    public void grow()
    {
        unsafe { free( data ); data = alloc<i32>( 4 ); data[0] = 2; }
    }

    private i32[*] data;
};

class M
{
    public buffer inner;

    M()
    {
        inner = buffer();
    }

    i32 borrow_then_grow( ref M other )
    {
        ref i32 x = inner[0];
        other.inner.grow();
        return x;
    }
};

u64 match_then_assign( ref Holder a, ref Holder b )
{
    switch( a )
    {
        case Holder::Full( x ):
            b = Holder::Empty;
            return x.get();
        case Holder::Empty:
            return 0;
    }
}

i32 borrow_then_grow( ref buffer a, ref buffer b )
{
    ref i32 x = a[0];
    b.grow();
    return x;
}

void grow_it( ref buffer b )
{
    b.grow();
}

i32 borrow_then_lend( ref buffer a, ref buffer b )
{
    ref i32 x = a[0];
    grow_it( ref b );
    return x;
}

u64 same_holder_twice()
{
    Holder h = Holder::Full( Buf( 1 ) );
    return match_then_assign( ref h, ref h );
}

i32 same_buffer_twice()
{
    buffer v = buffer();
    return borrow_then_grow( ref v, ref v );
}

i32 buffer_and_alias()
{
    buffer v = buffer();
    ref buffer r = v;
    return borrow_then_lend( ref v, ref r );
}

i32 receiver_and_alias()
{
    M m = M();
    ref M r = m;
    return m.borrow_then_grow( ref r );
}

// Accepted: neither argument can change.
i32 both_const( const ref buffer a, const ref buffer b )
{
    return a[0] + b[0];
}

i32 same_buffer_twice_const()
{
    buffer v = buffer();
    return both_const( v, v );
}

// Accepted: `x` owns nothing, so neither name can free what the other reaches.
class Counter
{
    public i32 x;

    Counter()
    {
        x = 0;
    }

    void add( const ref i32 by )
    {
        x = x + by;
    }
};

i32 self_add()
{
    Counter c = Counter();
    c.add( c.x );
    return c.x;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "106:38-106:43 error: one object is passed twice to this call, which could change it = `ref h` at 106:31 is the "
            "same object; the callee would see it under two names",
            "112:37-112:42 error: one object is passed twice to this call, which could change it = `ref v` at 112:30 is the "
            "same object; the callee would see it under two names",
            "119:37-119:42 error: one object is passed twice to this call, which could change it = `ref v` at 119:30 is the "
            "same object; the callee would see it under two names",
            "126:32-126:37 error: one object is passed twice to this call, which could change it = `m` at 126:12 is the same "
            "object; the callee would see it under two names"
        }
    );
}

TEST_CASE( "errors_alloc", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Both operations are gated (D35), both are typed, and neither is excused from any other rule.
struct Node { i32 value; };

i32 ungated()
{
    Node* n = alloc<Node>();

    free( n );

    return 0;
}

i32 wrong_type()
{
    i32 v = 0;

    unsafe
    {
        i32* n = alloc<Node>();

        free( v );
    }

    return v;
}

i32 nothing_to_allocate()
{
    unsafe
    {
        i32* n = alloc<void>();
    }

    return 0;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "6:15-6:28 error: `alloc` needs an `unsafe` block = it hands back memory that does not hold a value yet, so the "
            "pointer's type is a claim rather than a fact",
            "8:5-8:14 error: `free` needs an `unsafe` block = the compiler cannot tell whether anything still points at it",
            "19:18-19:31 error: expected `i32*`, but got `Node*`",
            "21:9-21:18 error: `free` needs a pointer, but got `i32`",
            "29:5-32:6 error: an `unsafe` block that does nothing unsafe = remove `unsafe`",
            "31:18-31:31 error: `alloc` needs a type to allocate = `void` has no size"
        }
    );
}

TEST_CASE( "errors_array_elements", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN D60: an element is written or read only once the whole array holds a value. These are flow
// errors, apart from errors_array_fill because a sema error stops the flow checks. A line ending
// `// error` reports exactly one error; every other line reports none.
struct S
{
    u64[2] arr;
    u64    n;
};

u64 field_first()
{
    S s;
    s.arr[0] = 1; // error
    s.n      = 2;
    return s.arr[1] + s.n;
}

void fill( out u64[2] a )
{
    a[0] = 1; // error
}

u64 on_some_paths( bool c )
{
    S s;
    if( c )
    {
        s.arr = [ 0, .. ];
    }
    s.arr[0] = 1; // error
    s.n      = 0;
    return s.arr[0] + s.n;
}

u64 read_first()
{
    S s;
    s.n = 1;
    return s.arr[1]; // error
}

void whole_first( out u64[2] a )
{
    a    = [ 0, .. ];
    a[1] = 3;
}

u64 whole_field()
{
    S s;
    s.arr    = [ 4, .. ];
    s.arr[1] = 5;
    s.n      = 6;
    return s.arr[1] + s.n;
}

i32 main()
{
    u64[2] a = [ 0, .. ];
    fill( out a );
    whole_first( out a );
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "13:5-13:18 error: `s.arr` is written one element at a time before it holds a value = assign the whole of `s.arr` "
            "first",
            "20:5-20:14 error: `a` is written one element at a time before it holds a value = assign the whole of `a` first",
            "30:5-30:18 error: `s.arr` is written one element at a time, but may not hold a value yet = it is assigned on some "
            "paths to here, but not all - assign the whole of `s.arr` first",
            "39:12-39:20 error: `s.arr` is used before it is initialised = assign it before reading it"
        }
    );
}

TEST_CASE( "errors_array_fill", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN D60's fill `[ x, .. ]`, and an array local with no initialiser. A line ending `// error`
// reports exactly one error; every other line reports none.
class Buf
{
    u64* data;

    Buf()
    {
        unsafe
        {
            data = alloc<u64>();
        }

        *data = 0;
    }

    ~Buf()
    {
        unsafe
        {
            free( data );
        }
    }
};

void declared()
{
    i32[4] a; // error
    Buf[2] b; // error
}

u64 filled()
{
    u64[4] a = [ 0, .. ];
    auto   f = [ 0, .. ]; // error
    return a[0];
}

void owning()
{
    Buf[2] g = [ Buf(), .. ];
    Buf    x = Buf();
    Buf[2] h = [ move x, .. ]; // error
    Buf    y = Buf();
    Buf[2] k = [ y, .. ]; // error
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "28:5-28:14 error: `a` needs a value for every element = fill it, as in `i32[4] a = [0, ..];`",
            "29:5-29:14 error: `b` needs a value for every element = give it a literal listing every element, or a fill, "
            "`[value, ..]`",
            "35:16-35:25 error: an array literal needs a type to fill = write its type on the variable, as in `u64[2] a = [0, "
            "..];`",
            "43:18-43:24 error: a fill evaluates its value once per element, so one value cannot go to each = build a new one "
            "each time, as in `[Buf(), ..]`",
            "45:18-45:19 error: a fill evaluates its value once per element, so one value cannot go to each = build a new one "
            "each time, as in `[Buf(), ..]`"
        }
    );
}

TEST_CASE( "errors_array_globals", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// What an array global's initialiser may not be: each element must be a constant a global could
// hold on its own, and the literal must still list every element.
struct P
{
    i32 x;
};

enum shape
{
    none,
    circle( f64 radius )
};

i32 seven()
{
    return 7;
}

i32       counter = 0;
const i32 LIMIT   = 3;

i32[2]       called  = [ 1, seven() ];
i32[2]       named   = [ LIMIT, counter ];
P[1]         points  = [ P { .x = 1 } ];
shape[1]     shapes  = [ shape::none ];
i32[3]       short   = [ 1, 2 ];
i32[2]       mixed   = [ 1, "two" ];
u8[2]        big     = [ 300, .. ];
const i32[2] SELF    = SELF;
i32[2]       ok      = [ LIMIT, LIMIT * 2 ];

i32 main()
{
    return ok[0];
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "26:24-26:32 error: `i32[3]` holds 3 values, but this lists 2 = a literal lists every element",
            "27:29-27:34 error: expected `i32`, but got `str`",
            "28:26-28:29 error: `300` does not fit in `u8`",
            "29:24-29:28 error: `SELF`'s initialiser depends on `SELF` itself = a `const` cannot be defined in terms of itself"
        }
    );
}

TEST_CASE( "errors_array_operators", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// `==` on an array of a type with no `==`, `<=>` on an array, and a program's block for an array
// of an interface it does not own.
struct Plain
{
    i32 x;
};

bool same( const ref Plain[2] a, const ref Plain[2] b )
{
    return a == b;
}

ordering order( i32[2] a, i32[2] b )
{
    return a <=> b;
}

implements equatable for i32[3]
{
    bool operator==( const ref i32[3] other ) const
    {
        return true;
    }
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "10:12-10:18 error: no operator `==` for `Plain[2]` and `Plain[2]` = the block at <prelude>:164:36 makes `T[N]` "
            "implement `equatable` only where `T : equatable`, which `Plain` is not",
            "15:12-15:19 error: no operator `<=>` for `i32[2]` and `i32[2]`",
            "18:26-18:32 error: a block for `i32[3]` must be in the package that declares `i32[3]` or `equatable` = two "
            "packages could otherwise each make `i32[3]` a `equatable`"
        }
    );
}

TEST_CASE( "errors_array_size_expressions", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Array sizes written as expressions that are not a valid size. A line ending `// error` reports
// exactly one error; every other line reports none.
const u64 N = 3;

void f()
{
    i32[-1] negative;      // error
    i32[1 << 70] shifted;  // error
    i32[N - 3] empty;      // error
    i32[N * 2] fine = [ 0, .. ];
}

i32 main() { return 0; }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "7:9-7:11 error: an array's size must be at least 1",
            "8:9-8:16 error: an array's size must be known at compile time = write literals and `const` names, with arithmetic "
            "over them",
            "9:9-9:14 error: an array's size must be at least 1"
        }
    );
}

TEST_CASE( "errors_assert", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// `assert` takes a `bool`, has no value, and is a keyword. One refusal per function.
void not_a_bool()
{
    i32 x = 1;
    assert( x );
}

void no_value()
{
    i32 y = assert( true );
}

void undeclared()
{
    assert( nope );
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "5:13-5:14 error: expected `bool`, but got `i32` = there is no conversion to `bool`: compare explicitly, as in `x "
            "!= 0`",
            "10:13-10:27 error: expected `i32`, but got `void`",
            "15:13-15:17 error: `nope` is not declared"
        }
    );
}

TEST_CASE( "errors_assert_paths", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// An `assert` may not run - it is off without --debug - so the checks see its condition on one
// path only, whatever the build. One refusal per function.
class Box
{
    public Box()
    {
        n_ = 1;
    }

    i32 n_;
};

bool fill( out i32 x )
{
    x = 1;
    return true;
}

bool consume( move Box _ )
{
    return true;
}

i32 read_after_out()
{
    i32 x;
    assert( fill( out x ) );
    return x;
}

void use_after_move()
{
    Box b = Box();
    assert( consume( move b ) );
    _ = consume( move b );
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "28:12-28:13 error: `x` may be used before it is initialised = it is assigned on some paths to here, but not all",
            "35:18-35:24 error: `b` may already have been moved = moved at 34:22 on some path to here"
        }
    );
}

TEST_CASE( "errors_block_cascades", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Block errors that are reported once, without the errors that follow from them.
interface Named
{
    i32 code() const;
};

struct Opaque
{
    i32 x;
};

struct Wrap<T> where T : copyable
{
    T v;
};

struct Pair<A, B> where A : copyable, where B : copyable
{
    A a;
    B b;
};

// error: once, not once more per method
implements<T, T> Named for Wrap<T*> where T : Named
{
    i32 code() const { return 1; }
    i32 two() const { return 2; }
};

// error: `U` is unknown, and nothing more is said of its bound
implements<T> Named for Wrap<T*> where U : copyable
{
    i32 code() const { return 1; }
};

// error: once for `A`, though it fills both parameters
implements<A> Named for Pair<A, A> where A : Named
{
    i32 code() const { return 1; }
};

// error: the refused block's type is still named in the `self` help
implements equatable for Opaque*
{
    bool operator==( const ref self other ) const { return true; }
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "24:15-24:16 error: `T` is already declared in this scope = previous declaration is at: t.kl:24:12",
            "31:34-31:52 error: `U` is not a type parameter of the block",
            "37:30-37:31 error: `A` does not promise `copyable`, and `Pair` requires it of `A` = add `copyable` to the block's "
            "`where` clause for `A`",
            "43:26-43:33 error: a block cannot make `Opaque*` `equatable` = the compiler compares and hashes a pointer by its "
            "address",
            "45:32-45:36 error: `self` is written only inside an `interface` = write `Opaque*`, the type's own name"
        }
    );
}

TEST_CASE( "errors_block_covers_every_instance", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// M12: a block that covers every instance of this package's type is refused, even when its `where`
// only restates the `copyable` the type requires. One error per marked line.
interface Named
{
    i32 code() const;
};

struct Wrap<T> where T : copyable
{
    T v;
};

implements<T> Named for Wrap<T> where T : copyable // error: unconditional in all but spelling
{
    i32 code() const { return 1; }
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() == Lines { "13:25-13:32 error: `Wrap<T>` is declared in this package, so a block cannot be written for it = "
                             "write `implements Named` where `Wrap` is declared" }
    );
}

TEST_CASE( "errors_block_help", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// What the help names when a block exists but does not answer.
interface Named
{
    i32 code() const;
};

interface Other
{
    i32 o() const;
};

interface Shape implements Named
{
    i32 area() const;
};

interface Solid implements Other
{
    i32 volume() const;
};

implements Named for i32
{
    i32 code() const { return this; }
};

struct Box3<T> where T : copyable
{
    T v;
};

implements<T> Named for Box3<T> where T : copyable & Other
{
    i32 code() const { return 1; }
};

// error: names the narrower block and the bound to add; the call stays quiet
implements<T> Shape for Box3<T> where T : copyable & Named
{
    i32 area() const { return 2; }
};

// error: no `Other` block, so the conditional form is asked for
implements<T> Solid for Box3<T> where T : copyable & Named
{
    i32 volume() const { return 3; }
};

struct Wrap<T> where T : copyable
{
    T v;
};

implements<T> Named for Wrap<T> where T : copyable & Named
{
    i32 code() const { return 4; }
};

implements<T> Named for const T* where T : Named
{
    i32 code() const { return 5; }
};

enum Opt<T> where T : copyable
{
    some( T v ),
    none
};

implements<T> equatable for Opt<T> where T : copyable & equatable
{
    bool operator==( const ref Opt<T> other ) const { return true; }
};

struct Opaque
{
    i32 x;
};

i32 area<T>( const ref T x ) where T : Shape
{
    return x.area();
}

i32 read<T>( const ref T x ) where T : Named
{
    return x.code();
}

i32 through( const Named* n )
{
    return n.code();
}

i32 main()
{
    Wrap<bool> w = Wrap<bool> { true };
    i32        x = 3;
    i32*       q = &x;
    Opt<Opaque> a = Opt::some( Opaque { 1 } );

    i32 total = area( Box3<i32> { 3 } );
    // error: the `where` that `bool` misses
    total = total + through( &w );
    // error: the `const T*` block, not another for `i32*`
    total = total + read( q );
    // error: the `where` that `Opaque` misses, not a second block
    return a == a ? total : 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "38:15-38:20 error: `Box3<T>` implements `Shape` but not `Named`, which `Shape` requires = the block at t.kl:32:25 "
            "makes `Box3<T>` implement `Named` only where `T : Other`; add `Other` to this block's `where` clause for `T`",
            "44:15-44:20 error: `Box3<T>` implements `Other` but has no method `o` = add `i32 o() const` to the block",
            "104:30-104:32 error: expected `const Named*`, but got `Wrap<bool>*` = the block at t.kl:54:25 makes `Wrap<T>` "
            "implement `Named` only where `T : Named`, which `bool` is not",
            "106:27-106:28 error: `read` needs a `T` that implements `Named`, but `i32*` does not = `const i32*` implements "
            "`Named`, so pass a `const i32*`",
            "108:12-108:18 error: no operator `==` for `Opt<Opaque>` and `Opt<Opaque>` = the block at t.kl:70:29 makes "
            "`Opt<T>` implement `equatable` only where `T : equatable`, which `Opaque` is not"
        }
    );
}

TEST_CASE( "errors_block_on_a_pointer", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A pointer compares, orders and hashes by its address, whatever it points at.

struct Opaque
{
    i32 v;
};

struct Wrap<T> where T : copyable
{
    T v;
};

implements equatable for Opaque* // error: not even for one pointer type
{
    bool operator==( const ref Opaque* other ) const { return true; }
};

implements<T> comparable for Wrap<T>* where T : copyable & comparable // error: nor for an instance's
{
    ordering operator<=>( const ref Wrap<T>* other ) const { return ordering::equal; }
};

implements hashable for const Opaque* // error: nor through `const`
{
    void hash( ref hasher h ) const { }
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "13:26-13:33 error: a block cannot make `Opaque*` `equatable` = the compiler compares and hashes a pointer by its "
            "address",
            "18:30-18:38 error: a block cannot make `Wrap<T>*` `comparable` = the compiler compares and hashes a pointer by "
            "its address",
            "23:25-23:38 error: a block cannot make `const Opaque*` `hashable` = the compiler compares and hashes a pointer by "
            "its address"
        }
    );
}

TEST_CASE( "errors_block_overlap_occurs", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Blocks overlap only where a finite type matches both: `T` cannot also be `Wrap<T>`.
interface Named
{
    i32 code() const;
};

interface Sized
{
    i32 size() const;
};

interface Tagged
{
    i32 tag() const;
};

struct Pair<A, B> where A : copyable, where B : copyable
{
    A a;
    B b;
};

struct Wrap<T> where T : copyable
{
    T v;
};

implements<T> Named for Pair<T, Wrap<T>> where T : copyable
{
    i32 code() const { return 1; }
};

implements<U> Named for Pair<Wrap<U>, U> where U : copyable
{
    i32 code() const { return 2; }
};

implements<T> Sized for Pair<T, Wrap<T>> where T : copyable
{
    i32 size() const { return 1; }
};

implements<U> Sized for Pair<U, U> where U : copyable
{
    i32 size() const { return 2; }
};

implements<T, U> Tagged for Pair<T, Wrap<U>> where T : copyable, where U : copyable
{
    i32 tag() const { return 1; }
};

implements<V> Tagged for Pair<Wrap<V>, V> where V : copyable // error: both at `Pair<Wrap<Wrap<U>>, Wrap<U>>`
{
    i32 tag() const { return 2; }
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines { "53:15-53:21 error: `Pair<Wrap<V>, V>` could meet `Pair<T, Wrap<U>>` at one type, and both implement `Tagged` "
                "= the block at t.kl:48:29 implements it first; a `where` does not keep blocks apart, so narrow one's type" }
    );
}

TEST_CASE( "errors_block_sibling", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A block's method answers its interface: inside the block, `this` reaches it once bound as that
// interface; elsewhere every interface declaring the name is named.
interface to_primitive { i32 to_int() const; };
interface adder { i32 to_int() const; i32 plus( i32 n ) const; };

implements to_primitive for i32 { i32 to_int() const { return this; } };

implements adder for i32
{
    i32 to_int() const { return this; }
    i32 plus( i32 n ) const { return this.to_int() + n.to_int(); }
};

i32 main()
{
    i32 x = 3;
    return x.to_int();
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "11:38-11:42 error: `i32` has no methods = `to_int` is reached only as a `adder`; bind `this` first: `const ref "
            "adder me = this;`",
            "11:54-11:55 error: `i32` has no methods = `to_int` is reached only as a `to_primitive` or a `adder`: through a "
            "`where T :` parameter bounded by one, or a pointer to one",
            "17:12-17:13 error: `i32` has no methods = `to_int` is reached only as a `to_primitive` or a `adder`: through a "
            "`where T :` parameter bounded by one, or a pointer to one"
        }
    );
}

TEST_CASE( "errors_borrow_after_move", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A borrow reads the value, so borrowing a moved local is a use after move in every form a borrow
// takes. One function each, since a local is reported once.
class Buffer
{
    public u64 size;

    Buffer()
    {
        size = 0;
    }

    ~Buffer()
    {
    }

    public u64 count() const
    {
        return size;
    }
};

void take( move Buffer _ )
{
}

u64 look( Buffer b )
{
    return b.size;
}

void poke( ref Buffer _ )
{
}

void bare()
{
    Buffer b = Buffer();
    take( move b );
    _ = look( b );
}

void by_ref()
{
    Buffer b = Buffer();
    take( move b );
    poke( ref b );
}

void receiver()
{
    Buffer b = Buffer();
    take( move b );
    _ = b.count();
}

void address()
{
    Buffer b = Buffer();
    take( move b );
    _ = &b;
}

void maybe( bool c )
{
    Buffer b = Buffer();

    if( c )
    {
        take( move b );
    }

    _ = look( b );
}

// Built again, it can be borrowed again.
void refilled()
{
    Buffer b = Buffer();
    take( move b );
    b = Buffer();
    _ = look( b );
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "39:15-39:16 error: `b` is used after it was moved = moved at 38:11",
            "46:11-46:16 error: `b` is used after it was moved = moved at 45:11",
            "53:9-53:10 error: `b` is used after it was moved = moved at 52:11",
            "60:9-60:11 error: `b` is used after it was moved = moved at 59:11",
            "72:15-72:16 error: `b` may already have been moved = moved at 69:15 on some path to here"
        }
    );
}

TEST_CASE( "errors_borrows", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN D31. A bare parameter of an owning type is a *read-only* borrow: the caller still owns it
// and still destroys it, so the callee may look and must not touch. It is forced rather than
// chosen - a class cannot be copied while copy constructors sit outside v0, so borrowing is the
// only meaning a bare argument has left.
struct P
{
    i32 x;
};

class B
{
    public u64 n;

    B( u64 x )
    {
        n = x;
    }

    ~B()
    {
    }
};

class W
{
    public P p;

    ~W()
    {
    }
};

i32 writes_a_field( B b )
{
    b.n = 5;

    return 0;
}

i32 compound_assigns( B b )
{
    b.n += 5;

    return 0;
}

// `++` reaches assignability by its own route, and a `u64` field types perfectly well - nothing
// else here would have stopped it.
i32 increments( B b )
{
    b.n++;

    return 0;
}

i32 assigns_the_binding( B b )
{
    b = B( 2 );

    return 0;
}

// The walk is a loop rather than one step: a path of any depth still roots in the borrow.
i32 writes_a_nested_field( W w )
{
    w.p.x = 5;

    return 0;
}

void consume( move B b )
{
}

i32 moves_it( B b )
{
    consume( move b );

    return 0;
}

void grow( ref B b )
{
    b.n = 1;
}

i32 lends_it_mutably( B b )
{
    grow( ref b );

    return 0;
}

// The sharpest of the three: this freed one resource twice before the rule existed, because the
// return copied an owning value the caller was still going to destroy.
B returns_it( B b )
{
    return b;
}

// Everything below is accepted, and the fixture would be worth much less without it: it is what
// shows the rule refuses giving the borrow away rather than every use of an owning type.

u64 reads_a_field( B b )
{
    return b.n;
}

// The return type owns nothing, so a value read out of the borrow leaves with no one else holding it.
u64 returns_a_field( B b )
{
    return b.n;
}

// Lending on what you were lent needs no marker and no copy: the address is simply forwarded.
u64 passes_it_on( B b )
{
    return reads_a_field( b );
}

// Shallow, the way `const` is in C++, and for the same reason: what a pointer points at was never
// part of the object that was lent.
class Holder
{
    public P* q;

    ~Holder()
    {
    }
};

i32 writes_through_a_pointer_it_holds( Holder h )
{
    h.q.x = 5;

    return 0;
}

// A `move` parameter is not a borrow - the callee owns what it was given - so all three transfers
// stay legal through one.
B owns_it( move B b )
{
    b.n = 99;

    return b;
}

// And a local owns what it holds, so none of this touches one.
i32 main()
{
    B a = B( 1 );

    a.n = 5;

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "35:5-35:8 error: `b` is borrowed, so it cannot be modified = take it as `ref B b` to modify it",
            "42:5-42:8 error: `b` is borrowed, so it cannot be modified = take it as `ref B b` to modify it",
            "51:5-51:8 error: `b` is borrowed, so it cannot be modified = take it as `ref B b` to modify it",
            "58:5-58:6 error: `b` is borrowed, so it cannot be modified = take it as `ref B b` to modify it",
            "66:5-66:10 error: `w` is borrowed, so it cannot be modified = take it as `ref W w` to modify it",
            "77:19-77:20 error: cannot move out of a borrow = take it as `move B b` to own it",
            "89:15-89:16 error: `b` is borrowed, so it cannot be modified = take it as `ref B b` to modify it",
            "98:12-98:13 error: cannot return a borrowed value = the caller still owns it",
            "134:5-134:10 error: this is reached through a `const P*`, so it cannot be modified = a pointer to `const` only "
            "reads what it points at"
        }
    );
}

TEST_CASE( "errors_bound_interface", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A `T` reaches only the interfaces its `where` clause promises, and a `const T*` only a `const`
// interface pointer. Each mistake is reported once, where it was made.
interface Shape
{
    f64 area() const;
};

interface Named
{
    u64 id() const;
};

f64 by_ref( const ref Shape s ) { return s.area(); }
f64 by_pointer( Shape* s ) { return s.area(); }
f64 by_box( move box<Shape> s ) { return s.get().area(); }

void unbounded<T>( const ref T x, move box<T> b )
{
    by_ref( x );
    by_box( move b );
}

void other<T>( const ref T x )
    where T : Named
{
    by_ref( x );
    const ref Shape s = x;
}

void constant<T>( const T* p )
    where T : Shape
{
    by_pointer( p );
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "19:13-19:14 error: expected `Shape`, but got `T`",
            "20:13-20:19 error: expected `box<Shape>`, but got `box<T>`",
            "26:13-26:14 error: expected `Shape`, but got `T`",
            "27:25-27:26 error: expected `Shape`, but got `T`",
            "33:17-33:18 error: expected `Shape*`, but got `const T*` = a pointer to `const` never converts back to one that "
            "writes"
        }
    );
}

TEST_CASE( "errors_bound_method_ambiguity", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Two bounds that each declare a method of one name do not say which a call through them means,
// whichever is written first; one interface at two instances is a single overload set.

interface A
{
    i32 foo() const;
    static i32 make();
};

interface B
{
    i32 foo() const;
    static i32 make();
};

i32 a_then_b<T>( const ref T t ) where T : A & B { return t.foo(); }
i32 b_then_a<T>( const ref T t ) where T : B & A { return t.foo(); }
i32 shared_static<T>() where T : A & B { return T::make(); }

// The arity differs, which must not decide it either.
interface An { i32 name() const; };
interface Bn { i32 name( bool v ) const; };
i32 by_arity<T>( const ref T t ) where T : An & Bn { return t.name( true ); }

interface Ap { self operator+( const ref self other ) const; };
interface Bp { self operator+( const ref self other ) const; };
T plus<T>( const ref T a, const ref T b ) where T : Ap & Bp { return a + b; }

// Still accepted: one `-` takes one operand and the other two.
interface Negates { self operator-() const; };
interface Subtracts { self operator-( const ref self other ) const; };
T minus<T>( const ref T a, const ref T b ) where T : Negates & Subtracts { return a - -b; }

// Still accepted: one interface, two instances.
interface sink<U> { void put( U value ); };
void two_instances<T>( ref T t ) where T : sink<i64> & sink<bool> { t.put( true ); t.put( cast<i64>( 1 ) ); }

i32 main() { return 0; }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "16:59-16:64 error: `foo` is declared by both `A` and `B`, so the call does not say which = call it from a "
            "function bound by only one of them",
            "17:59-17:64 error: `foo` is declared by both `B` and `A`, so the call does not say which = call it from a "
            "function bound by only one of them",
            "18:49-18:56 error: `make` is declared by both `A` and `B`, so the call does not say which = call it from a "
            "function bound by only one of them",
            "23:61-23:67 error: `name` is declared by both `An` and `Bn`, so the call does not say which = call it from a "
            "function bound by only one of them",
            "27:70-27:75 error: `operator+` is declared by both `Ap` and `Bp`, so the call does not say which = call it from a "
            "function bound by only one of them"
        }
    );
}

TEST_CASE( "errors_bounds", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D40: `equatable`, `comparable` and `hashable` are prelude interfaces, and the compiler's own
// bounds are spelled lower case. The compiler makes a number all three, and `bool`, an enum without
// payloads and a pointer `equatable` and `hashable`; anything else declares what it implements.
struct Point implements equatable
{
    i32 x;

    bool operator==( const ref Point other ) const
    {
        return x == other.x;
    }
};

struct Ranked implements comparable
{
    i32 x;

    ordering operator<=>( const ref Ranked other ) const
    {
        return x <=> other.x;
    }
};

struct Plain
{
    i32 x;
};

enum Shape
{
    Circle( f64 r ),
    Dot,
};

enum Suit
{
    Hearts,
    Spades,
};

bool same<T>( const ref T a, const ref T b ) where T : equatable
{
    return a == b;
}

bool less<T>( const ref T a, const ref T b ) where T : comparable
{
    return a < b;
}

bool unordered<T>( const ref T a, const ref T b ) where T : equatable
{
    return a < b;
}

bool forwards<T>( const ref T a ) where T : equatable
{
    return less( a, a );
}

bool old_spelling<T>( const ref T a ) where T : Equatable
{
    return true;
}

void keep( const ref hashable h )
{
}

i32 main()
{
    Point p = Point { 1 };
    Plain q = Plain { 1 };
    Shape s = Shape::Dot;
    Suit  h = Suit::Hearts;
    bool  t = true;
    i32*  r = nullptr;

    same( p, p );
    same( h, h );
    same( t, t );
    same( r, r );
    same( q, q );
    same( s, s );

    less( p, p );
    less( h, h );
    less( t, t );
    less( r, r );

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "14:26-14:36 error: `Ranked` implements `equatable` but has no method `operator==` = add `bool operator==(const "
            "ref Ranked other) const` to `Ranked`",
            "53:12-53:17 error: no operator `<` for `T` and `T` = `<` needs `comparable`; write `where T : comparable` on "
            "`unordered`",
            "58:18-58:19 error: `T` does not promise `comparable`, and `less` requires it of `T` = add `comparable` to the "
            "`where` clause for `T` on `forwards`",
            "61:49-61:58 error: unknown bound `Equatable` = a bound is an interface, or one of copyable, numeric, integral, "
            "floating and defaultable",
            "66:22-66:30 error: `hashable` can only bound a type parameter, because it requires `equatable`, whose "
            "`operator==` mentions `self` = take a `T` with `where T : hashable` instead",
            "83:11-83:12 error: `same` needs a `T` that implements `equatable`, but `Plain` does not = declare `Plain` with "
            "`implements equatable`",
            "84:11-84:12 error: `same` needs a `T` that implements `equatable`, but `Shape` does not = write `implements "
            "equatable for Shape { ... }`",
            "86:11-86:12 error: `less` needs a `T` that implements `comparable`, but `Point` does not = declare `Point` with "
            "`implements comparable`",
            "87:11-87:12 error: `less` needs a `T` that implements `comparable`, but `Suit` does not = write `implements "
            "comparable for Suit { ... }`",
            "88:11-88:12 error: `less` needs a `T` that implements `comparable`, but `bool` does not = the compiler makes only "
            "numbers `comparable`",
            "89:11-89:12 error: `less` needs a `T` that implements `comparable`, but `i32*` does not = the compiler makes only "
            "numbers `comparable`"
        }
    );
}

TEST_CASE( "errors_box", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// M9.5: what `box` refuses. Only the prelude's `box` takes an interface as its type argument, and
// only by converting a box of an implementer: it is never built from a `Shape`, never converted
// from a non-implementer or back to an implementer, copied, or borrowed as another box's type.
// Each mistake is reported once, where it was made.
interface Shape
{
    f64 area() const;
};

struct Square implements Shape
{
    f64 side;

    f64 area() const { return side * side; }
};

struct Point
{
    f64 x;
};

class Holder<T>
{
    T* p;
};

void hold( Holder<Shape>* h ) { }

i32 main()
{
    box<Shape>  built = box<Shape>( Square { 1.0 } );
    box<Point>  point = box<Point>( Point { 1.0 } );
    box<Shape>  wrong = move point;
    box<Square> plain = box<Square>( Square { 1.0 } );
    box<Shape>  copy  = plain;
    ref box<Shape> borrowed = plain;
    box<Shape>  shape = move plain;
    box<Square> back  = move shape;
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "27:19-27:24 error: `Shape` is an interface, so it cannot be held by value = borrow one as `const ref Shape` or "
            "`ref Shape`, or point at one with `Shape*`",
            "31:29-31:34 error: a `box<Shape>` is made by converting a box of a type that implements `Shape` = box the value "
            "as its own type, and it converts where a `box<Shape>` is expected",
            "33:25-33:35 error: expected `box<Shape>`, but got `box<Point>`",
            "35:25-35:30 error: an owning value is transferred, not copied = write `move plain`",
            "36:31-36:36 error: cannot borrow `box<Square>` as `ref box<Shape>` = a borrow is the variable itself, so its type "
            "must match exactly",
            "38:25-38:35 error: expected `box<Square>`, but got `box<Shape>`"
        }
    );
}

TEST_CASE( "errors_builtin_type_arguments", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A builtin type is a known name that takes no type arguments.
i32 main()
{
    i32<u8> x = 1; // error
    bool<i32> y = true; // error

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "4:5-4:12 error: `i32` is not a generic = it takes no type arguments, so write `i32` on its own",
            "5:5-5:14 error: `bool` is not a generic = it takes no type arguments, so write `bool` on its own"
        }
    );
}

TEST_CASE( "errors_builtin_type_arguments_positions", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Type arguments on a builtin, outside a local's declaration.
void parameter( i32<i64> x ) { } // error
void pointer( i32<u8>* p ) { } // error
i32<i64> result() { return 1; } // error
class Holder
{
    bool<i32> f; // error
    public Holder() { f = true; }
};

i32 main()
{
    i32<u8>[2] a = [ 1, 2 ]; // error
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "2:17-2:25 error: `i32` is not a generic = it takes no type arguments, so write `i32` on its own",
            "3:15-3:22 error: `i32` is not a generic = it takes no type arguments, so write `i32` on its own",
            "4:1-4:9 error: `i32` is not a generic = it takes no type arguments, so write `i32` on its own",
            "7:5-7:14 error: `bool` is not a generic = it takes no type arguments, so write `bool` on its own",
            "13:5-13:12 error: `i32` is not a generic = it takes no type arguments, so write `i32` on its own"
        }
    );
}

TEST_CASE( "errors_builtin_type_names", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A builtin type's name is reserved. Each declaration taking one is refused once, at its name, and
// declares nothing, so every later use of the type still names the type.
i32 i32( i32 n )
{
    return n;
}

// A swapped type and name is two faults, each at its own token. Every use of `n` is undeclared.
i32 twice( n i32 )
{
    return n * 2;
}

i32 main()
{
    i32 u8 = 1;
    u8  small = 2;
    total i32 = 0;
    total = total + small;
    return total;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "3:5-3:8 error: `i32` is the name of a builtin type = a declaration may not take one",
            "9:12-9:13 error: unknown type `n`",
            "9:14-9:17 error: `i32` is the name of a builtin type = a declaration may not take one",
            "11:12-11:13 error: `n` is not declared",
            "16:9-16:11 error: `u8` is the name of a builtin type = a declaration may not take one",
            "18:5-18:10 error: unknown type `total`",
            "18:11-18:14 error: `i32` is the name of a builtin type = a declaration may not take one",
            "19:5-19:10 error: `total` is not declared",
            "19:13-19:18 error: `total` is not declared",
            "20:12-20:17 error: `total` is not declared"
        }
    );
}

TEST_CASE( "errors_case_label_method", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A method named in a `case` label is one error, not a second "known at compile time".
class K
{
    public i32 k;
    public K() { k = 1; }
    public i32 boom() const { return k; }
};

i32 main()
{
    i32 n = 1;
    switch( n )
    {
        case K::boom: return 1;
        default: return 0;
    }
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() == Lines { "14:14-14:21 error: `boom` is a function, not a value = take its address as `&K::boom`, or call "
                             "it as `value.boom(...)`" }
    );
}

TEST_CASE( "errors_cast_literal", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A literal takes the target of a `cast` only when the target is a number, so a refused pair is
// the conversion's own error with its help.
enum E { a, b };

void give( i32 x )
{
    return cast<void>( x ); // error: the conversion only
}

i32 main()
{
    bool t = cast<bool>( 5 ); // error
    f32  f = cast<f32>( false ); // error
    u8*  p = cast<u8*>( 42 ); // error
    E    e = cast<E>( 1 ); // error
    u8   m = cast<u8>( 200 ); // fine
    f32  h = cast<f32>( 1 ); // fine
    u8*  n = cast<u8*>( nullptr ); // fine

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "7:12-7:27 error: `cast` cannot convert `i32` to `void`",
            "12:14-12:29 error: `cast` cannot convert `i32` to `bool` = compare explicitly, as in `5 != 0`",
            "13:14-13:32 error: `cast` cannot convert `bool` to `f32`",
            "14:14-14:29 error: `cast` cannot convert `i32` to `u8*`",
            "15:14-15:26 error: `cast` cannot convert `i32` to `E`"
        }
    );
}

TEST_CASE( "errors_char_and_string", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D20. A character literal is a u8-sized code point; a string literal is the prelude's `str`.
i32 main()
{
    bool a = 'x';
    u8   b = "hello";
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "4:14-4:17 error: expected `bool`, but got a character literal", "5:14-5:21 error: expected `u8`, but got `str`"
        }
    );
}

TEST_CASE( "errors_compare_help", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// The help for a value that is not a `bool` names the value, compares a pointer with `nullptr`,
// and offers nothing for a type with no zero.
void nothing() { }

enum Colour { red, green };

i32 main()
{
    u64  count = 3;
    i32* ptr   = nullptr;
    i32  a     = 1;
    i32  b     = 2;

    if( count ) { } // error
    while( ptr ) { } // error
    if( a + b ) { } // error
    if( ( a - b ) ) { } // error
    if( nothing() ) { } // error
    bool c = cast<bool>( ptr ); // error
    bool d = cast<bool>( Colour::red ); // error

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "14:9-14:14 error: expected `bool`, but got `u64` = there is no conversion to `bool`: compare explicitly, as in "
            "`count != 0`",
            "15:12-15:15 error: expected `bool`, but got `i32*` = there is no conversion to `bool`: compare explicitly, as in "
            "`ptr != nullptr`",
            "16:9-16:14 error: expected `bool`, but got `i32` = there is no conversion to `bool`: compare explicitly, as in "
            "`(a + b) != 0`",
            "17:9-17:18 error: expected `bool`, but got `i32` = there is no conversion to `bool`: compare explicitly, as in `( "
            "a - b ) != 0`",
            "18:9-18:18 error: expected `bool`, but got `void` = there is no conversion to `bool`",
            "19:14-19:31 error: `cast` cannot convert `i32*` to `bool` = compare explicitly, as in `ptr != nullptr`",
            "20:14-20:39 error: `cast` cannot convert `Colour` to `bool` = an `enum`'s tag converts only to an integer"
        }
    );
}

TEST_CASE( "errors_conditional", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN §6.7. A conditional has two type rules: an expectation reaches both arms, and with none the
// arms must match exactly. Widening happens after a type is settled, never to settle one - so two
// arms that §6.4 would happily widen to a common type are still refused when nothing else decides.
i32 arms_must_agree()
{
    i32 a = 1;
    i64 b = 2;

    auto c = true ? a : b;

    return 0;
}

// The case that separates the rule taken from `Type_table::common`: u8 and i64 have a common type
// and it is still not allowed to be the answer, because nothing asked for one.
i32 widening_does_not_decide()
{
    u8  a = 1;
    i64 b = 2;

    auto c = true ? a : b;

    return 0;
}

// A condition is a condition wherever it appears, so this is the same rule an `if` applies.
i32 condition_must_be_bool()
{
    i32 a = 1;

    auto c = a ? 1 : 2;

    return 0;
}

// With an expectation the arms never meet: each is measured against `i32` on its own, so the
// report names the arm that failed rather than a disagreement between them.
i32 an_arm_that_cannot_reach_the_expectation()
{
    i32 x = true ? 1 : false;

    return x;
}

// Right-associative, so the inner conditional is the outer one's else arm and is inferred on its
// own. One report, from the arms that actually disagree.
i32 nested_arms_must_agree()
{
    i32  a = 1;
    i32  b = 2;
    bool q = false;

    auto c = true ? a : q ? b : q;

    return 0;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "9:14-9:26 error: ternary branches have different types: `i32` and `i64`",
            "21:14-21:26 error: ternary branches have different types: `u8` and `i64`",
            "31:14-31:15 error: expected `bool`, but got `i32` = there is no conversion to `bool`: compare explicitly, as in "
            "`a != 0`",
            "40:24-40:29 error: expected `i32`, but got `bool`",
            "53:25-53:34 error: ternary branches have different types: `i32` and `bool`"
        }
    );
}

TEST_CASE( "errors_conditional_borrow", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D75: each arm of a borrowed conditional is checked as the whole would be. One error per marked
// line, at the arm.
void bump( ref i32 n ) { n = n + 10; }
u64 peek( const ref i32 _ ) { return 0; }

void spelled( bool c )
{
    i32 a = 1;
    u8 n = 2;
    bump( c ? ref a : a );       // error: the second arm takes `ref`
    bump( c ? ref a : ref 1 );   // error: no variable
    bump( c ? ref a : ref n );   // error: `u8` as `ref i32`
    _ = peek( c ? ref a : ref a ); // error: twice, `peek` does not modify
    ref i32 r = c ? a : 1;       // error: no variable
}

i32 main() { return 0; }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "10:23-10:24 error: `bump` may modify this argument = write `ref`",
            "11:27-11:28 error: `ref` needs a variable to borrow",
            "12:23-12:28 error: cannot borrow `u8` as `ref i32` = a borrow is the variable itself, so its type must match "
            "exactly",
            "13:19-13:24 error: `peek` does not modify this argument = remove `ref`",
            "13:27-13:32 error: `peek` does not modify this argument = remove `ref`",
            "14:25-14:26 error: a `ref` binding needs a variable to bind to = drop `ref` to copy the value"
        }
    );
}

TEST_CASE( "errors_conditional_literal_arguments", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A conditional of literals chooses an overload as a literal does: by the types that hold both arms.
// Each marked line is one error.
void two( u64 _, u64 _ ) { }
void two( i64 _, i64 _ ) { }
void wide( u8 _ ) { }
void wide( u64 _ ) { }
void small( u8 _ ) { }
void small( i8 _ ) { }

i32 main()
{
    u64  x = 3;
    bool c = x > 1;
    two( x, c ? 1 : 2 );
    wide( c ? 1 : 300 );
    small( c ? -1 : 100 );
    small( c ? -1 : 200 );                          // error: no `small` holds both arms
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() == Lines { "17:5-17:26 error: no `small` matches these arguments = the ones declared take `(u8)` and `(i8)`" }
    );
}

TEST_CASE( "errors_const", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN D32's shape applied to const: `ref` is a binding mode, `const` is a binding property. Both
// are about the name rather than the data, which is what lets this reuse the read-only machinery
// the borrow rule already has instead of putting constness into the type.
struct P
{
    i32 x;
};

struct Outer
{
    P inner;
};

struct Holder
{
    P* q;
};

const i32 g = 1;

i32 assigns_a_global()
{
    g = 2;

    return g;
}

i32 assigns_a_parameter( const i32 n )
{
    n = 2;

    return n;
}

i32 assigns_a_local()
{
    const i32 k = 1;

    k = 2;

    return k;
}

// Each reaches assignability by its own route, so each needs its own case.
i32 increments_a_local()
{
    const i32 k = 1;

    k++;

    return k;
}

i32 compound_assigns_a_local()
{
    const i32 k = 1;

    k += 2;

    return k;
}

// The walk is the borrow rule's: a field path of any depth roots in the declaration.
i32 writes_a_field()
{
    const P p = P { 1 };

    p.x = 5;

    return p.x;
}

i32 writes_a_nested_field()
{
    const Outer o = Outer { P { 1 } };

    o.inner.x = 5;

    return 0;
}

// `const` attaches to the name, so `i32* const q` is a const *binding*. Reseating it is refused;
// writing through it is not, which is exactly what C++ means by the spelling.
i32 reseats_a_const_pointer()
{
    i32 y = 1;
    i32 z = 2;

    i32* const q = &y;

    q = &z;

    return y;
}

// `const` on the element is C++'s promise about the data, made through this pointer, so writing
// through it is refused the way writing a `const` binding is.
i32 writes_through_a_pointer_to_const()
{
    i32        y = 1;
    const i32* q = &y;

    *q = 2;
    return y;
}

void grow( ref i32 n )
{
    n = n + 1;
}

// Both routes into a mutable binding are the same question, so both are refused.
i32 launders_through_a_binding()
{
    const i32 k = 1;

    ref i32 r = k;

    return k;
}

i32 launders_through_an_argument()
{
    const i32 k = 1;

    grow( ref k );

    return k;
}

// Everything below is accepted, and the fixture would be worth much less without it: it is what
// shows the rule refuses writing a const rather than mentioning one.

i32 reads_and_initialises()
{
    const i32 k = 1;

    i32 y = k + 1;

    return y;
}

// Shallow, the way C++'s const is: what a pointer points at was never named by this declaration.
i32 writes_through_a_pointer_it_holds()
{
    P v = P { 1 };

    const Holder h = Holder { &v };

    h.q.x = 5;

    return v.x;
}

i32 writes_through_a_const_pointer()
{
    i32 y = 1;

    i32* const q = &y;

    *q = 5;

    return y;
}

i32 main()
{
    i32 k = 1;

    k = 2;

    return k;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "23:5-23:6 error: `g` is `const` = remove `const` to modify it",
            "30:5-30:6 error: `n` is `const` = remove `const` to modify it",
            "39:5-39:6 error: `k` is `const` = remove `const` to modify it",
            "49:5-49:6 error: `k` is `const` = remove `const` to modify it",
            "58:5-58:6 error: `k` is `const` = remove `const` to modify it",
            "68:5-68:8 error: `p` is `const` = remove `const` to modify it",
            "77:5-77:14 error: `o` is `const` = remove `const` to modify it",
            "91:5-91:6 error: `q` is `const` = remove `const` to modify it",
            "103:5-103:7 error: this is reached through a `const i32*`, so it cannot be modified = a pointer to `const` only "
            "reads what it points at",
            "117:17-117:18 error: `k` is `const` = remove `const` to modify it",
            "126:15-126:16 error: `k` is `const` = remove `const` to modify it",
            "150:5-150:10 error: this is reached through a `const P*`, so it cannot be modified = a pointer to `const` only "
            "reads what it points at"
        }
    );
}

TEST_CASE( "errors_const_field", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A `const` field is assigned by its own constructor, with `=`, and by nothing else. Assigning one
// twice is errors_const_field_once, since this file's errors stop the checks that would report it.
struct Point
{
    const i32 x;
    i32       y;
};

class Inner
{
    i32 v;

    Inner()
    {
        v = 0;
    }

    void poke()
    {
        v = 1;
    }
};

void set( ref i32 a )
{
    a = 1;
}

void init( out i32 a )
{
    a = 1;
}

class Counter
{
    public const i32 n;

    Counter()
    {
        n = 1;
    }

    // Another object of its own class is not the one being built.
    Counter( ref Counter other )
    {
        n       = other.n;
        other.n = 2;
    }

    void bump()
    {
        n += 1;
    }

    void bump_this()
    {
        this.n++;
    }

    ~Counter()
    {
        n = 0;
    }
};

// Each of these writes the field after its one assignment, or through something other than `=`.
class Once
{
    const i32   a;
    const i32   b;
    const i32   c;
    const Point p;
    const Inner i;

    Once( bool flag )
    {
        a = 1;
        b = 1;
        c = 1;
        c += 1;

        p   = Point { 1, 2 };
        p.y = 3;

        i = Inner();
        i.poke();
    }

    Once()
    {
        set( ref a );
        init( out b );
        c = 1;
        p = Point { 1, 2 };
        i = Inner();
    }
};

void through_a_reference( ref Point p )
{
    p.x = 2;
}

void through_a_pointer( Counter* c )
{
    c.n = 2;
}

i32 main()
{
    Point p = Point { 1, 2 };
    p.x     = 3;

    Counter c = Counter();
    i32*    q = &c.n;

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "47:9-47:16 error: `n` is a `const` field, so it cannot be modified = only a constructor assigns it",
            "52:9-52:10 error: `n` is a `const` field, so it cannot be modified = only a constructor assigns it",
            "57:9-57:15 error: `n` is a `const` field, so it cannot be modified = only a constructor assigns it",
            "62:9-62:10 error: `n` is a `const` field, so it cannot be modified = only a constructor assigns it",
            "80:9-80:10 error: `c` is a `const` field, so it cannot be modified = a constructor assigns it once, whole, with "
            "`=`",
            "83:9-83:12 error: `p` is a `const` field, so it cannot be modified = a constructor assigns it once, whole, with "
            "`=`",
            "86:9-86:10 error: `i` is a `const` field, so it cannot be modified = a constructor assigns it once, whole, with "
            "`=`",
            "91:18-91:19 error: `a` is a `const` field, so it cannot be modified = a constructor assigns it once, whole, with "
            "`=`",
            "92:19-92:20 error: `b` is a `const` field, so it cannot be modified = a constructor assigns it once, whole, with "
            "`=`",
            "101:5-101:8 error: `x` is a `const` field, so it cannot be modified = only a constructor assigns it",
            "106:5-106:8 error: `n` is a `const` field, so it cannot be modified = only a constructor assigns it",
            "112:5-112:8 error: `x` is a `const` field, so it cannot be modified = only a constructor assigns it",
            "115:17-115:21 error: expected `i32*`, but got `const i32*` = a pointer to `const` never converts back to one that "
            "writes"
        }
    );
}

TEST_CASE( "errors_const_field_once", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A constructor assigns a `const` field once on each path, and its address only reads it.
class Once
{
    const i32 a;
    const i32 b;
    const i32 c;
    const i32 d;

    Once( bool flag, i32 count )
    {
        a = 1;
        if( flag )
        {
            a = 2;
        }

        if( flag )
        {
            b = 1;
        }
        b = 2;

        c       = 0;
        i32 i   = 0;
        while( i < count )
        {
            c = i;
            i = i + 1;
        }

        _ = &d;
        d                = 1;
    }

    // Once on each arm is once.
    Once( bool flag )
    {
        if( flag )
        {
            a = 1;
        }
        else
        {
            a = 2;
        }

        b = 1;
        c = 1;
        d = 1;

        _ = &d;
    }
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "14:13-14:19 error: `a` already holds a value here = a constructor assigns a `const` field once on each path",
            "21:9-21:15 error: `b` may already hold a value here = a constructor assigns a `const` field once on each path",
            "27:13-27:19 error: `c` already holds a value here = a constructor assigns a `const` field once on each path",
            "31:13-31:15 error: `d` is used before this constructor assigns it = assign it before reading it"
        }
    );
}

TEST_CASE( "errors_const_global", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A global or static `const` can never change, so it takes its value where it is declared. A line
// ending `// error` reports exactly one error; every other line reports none.
const i32 Z; // error
const i32 ONE = 1;
i32 blank;

struct S
{
    i32 x;
    static const u8 limit; // error
    static u8 count;
};

i32 main() { return Z + ONE + blank; }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "3:1-3:13 error: `Z` is `const`, so it needs its value here = write `const i32 Z = 0;` if zero is meant",
            "10:12-10:27 error: `limit` is `const`, so it needs its value here = write `static const u8 limit = 0;` if zero is "
            "meant"
        }
    );
}

TEST_CASE( "errors_const_local", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A const local never changes once it holds a value, so it takes one where it is declared. A
// line ending `// error` reports exactly one error; every other line reports none.
struct P
{
    i32 x;
};

class Holder
{
    const i32 n;

    Holder( i32 v )
    {
        n = v;
    }
};

i32 main()
{
    const i32 v; // error
    const P p; // error
    const i32 w = 1;
    const Holder h = Holder( w );
    _ = h;
    return w;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "20:5-20:17 error: `v` is `const`, so it needs its value here = write `const i32 v = value;`, or remove `const` to "
            "set it later",
            "21:5-21:15 error: `p` is `const`, so it needs its value here = write `const P p = value;`, or remove `const` to "
            "set it later"
        }
    );
}

TEST_CASE( "errors_const_names", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A const that leads back to itself, and an overflow through a const's name. A line ending
// `// error` reports exactly one error; every other line reports none.
const i32 R = R + 1; // error
const i32 A = B * 2; // error
const i32 B = A + 1; // error

i32 big()
{
    return N * 1000000000; // error
}

const i32 N = 4;

i32 main() { return big(); }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "3:15-3:20 error: `R`'s initialiser depends on `R` itself = a `const` cannot be defined in terms of itself",
            "4:15-4:20 error: `A`'s initialiser depends on `A` itself = a `const` cannot be defined in terms of itself",
            "5:15-5:20 error: `B`'s initialiser depends on `B` itself = a `const` cannot be defined in terms of itself",
            "9:12-9:26 error: `4000000000` does not fit in `i32`"
        }
    );
}

TEST_CASE( "errors_const_ref", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN D31/D32. `const ref T` is the read-only borrow. Every rule the mutable binding has still
// applies - initialised at its declaration, never bound to a temporary - and the const half adds
// that it may not be written through.
struct Point
{
    i32 x;
};

i32 make()
{
    return 1;
}

i32 writes_through_a_parameter( const ref Point p )
{
    p.x = 5;

    return p.x;
}

i32 writes_through_a_binding()
{
    i32 x = 1;

    const ref i32 r = x;

    r = 5;

    return x;
}

i32 uninitialised_binding()
{
    const ref i32 r;

    return 0;
}

i32 binds_a_temporary()
{
    const ref i32 r = make();

    return r;
}

i32 reads( const ref Point p )
{
    return p.x;
}

// The marker at a call site announces that something happens to the caller's variable. With
// `const ref` nothing does, so there is no marker to write - and writing one is refused.
i32 marks_the_call()
{
    Point v = Point { 1 };

    return reads( ref v );
}

void grow( ref Point p )
{
    p.x = 1;
}

// The contrast: a mutable borrow of a const variable is laundering, and is refused.
i32 lends_a_const_mutably()
{
    const Point v = Point { 1 };

    grow( ref v );

    return 0;
}

// Everything below is accepted, and the fixture would be worth much less without it.

i32 reads_through_a_binding()
{
    i32 x = 1;

    const ref i32 r = x;

    // The referent stays writable in its own right: `const` is a property of *this* name.
    x = 5;

    return r;
}

i32 passes_bare()
{
    Point v = Point { 1 };

    return reads( v );
}

i32 passes_a_const()
{
    const Point v = Point { 1 };

    return reads( v );
}

i32 binds_a_const()
{
    const i32 k = 1;

    const ref i32 r = k;

    return r;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "16:5-16:8 error: `p` is `const` = remove `const` to modify it",
            "27:5-27:6 error: `r` is `const` = remove `const` to modify it",
            "34:5-34:21 error: a `ref` binding must be initialised = write `ref T r = x;`",
            "41:23-41:29 error: a `ref` binding needs a variable to bind to = drop `ref` to copy the value",
            "57:19-57:24 error: `reads` does not modify this argument = remove `ref`",
            "70:15-70:16 error: `v` is `const` = remove `const` to modify it"
        }
    );
}

TEST_CASE( "errors_const_ref_return", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN §8. A reference may be returned only when derived from one of the function's own reference
// parameters. The check is syntactic with no dataflow: trace the returned expression to
// its root and require a parameter that travels by address.
//
// Why no lifetimes are needed: a `ref` binding is initialised at its declaration and never
// reseated, so everything nameable at a call site outlives any binding declared there. Which
// parameter the result came from does not matter, because all of them outlive it.
struct Point
{
    i32 x;
};

i32 make()
{
    return 1;
}

const ref i32 returns_a_local()
{
    i32 v = 1;

    return v;
}

// The callee's own copy, so it dies with the frame exactly as a local does. This is the case that
// reads as safe and is not.
const ref i32 returns_a_by_value_parameter( i32 n )
{
    return n;
}

const ref i32 returns_a_temporary()
{
    return make();
}

const ref i32 returns_a_field_of_a_local()
{
    Point v = Point { 1 };

    return v.x;
}

// Accepted: a local binding is never reseated, so it lasts as long as what it was bound to.
const ref i32 returns_a_local_binding( const ref i32 a )
{
    const ref i32 r = a;

    return r;
}

// A `ref` from a `ref` parameter is accepted.
ref i32 returns_a_mutable_ref( ref i32 a )
{
    return a;
}

// The other two modes are argument-passing modes with no argument to pass. Both used to reach the
// emitter with the keyword silently dropped, because the check asked for `ref` alone and these
// matched neither the refusal nor the binding.
out i32 returns_an_out()
{
    return 1;
}

move i32 returns_a_move()
{
    return 1;
}

// Everything below is accepted, and the fixture would be worth much less without it.

const ref i32 picks( const ref i32 a, const ref i32 b )
{
    if( a > b )
    {
        return a;
    }

    return b;
}

const ref i32 gets( const ref Point p )
{
    return p.x;
}

const ref i32 looks( ref i32 a )
{
    return a;
}

class B
{
    public u64 n;

    B( u64 x )
    {
        n = x;
    }

    ~B()
    {
    }
};

// A bare parameter of an owning type is already a read-only borrow, so it is a reference parameter
// for this rule too.
const ref u64 peeks( B b )
{
    return b.n;
}

i32 uses_the_result()
{
    i32 x = 1;
    i32 y = 2;

    const ref i32 bound = picks( x, y );

    i32 copied = picks( x, y );

    return bound + copied;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "22:12-22:13 error: a returned reference must borrow from a parameter = anything else here dies when this function "
            "returns",
            "29:12-29:13 error: a returned reference must borrow from a parameter = anything else here dies when this function "
            "returns",
            "34:12-34:18 error: a returned reference must borrow from a parameter = anything else here dies when this function "
            "returns",
            "41:12-41:15 error: a returned reference must borrow from a parameter = anything else here dies when this function "
            "returns",
            "61:1-61:4 error: `out` is not a return mode = `out` says how an argument travels, and a return is not an argument",
            "66:1-66:5 error: `move` is not a return mode = `move` says how an argument travels, and a return is not an "
            "argument"
        }
    );
}

TEST_CASE( "errors_constant_chain", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A chain of constants is checked in the type it is written for, not in i32 first.
i64 below() { return 0 - 3000000000 - 1; }
i64 product() { return 2 * 3 * 1000000000; }
i64 negated() { return -( 0 - 3000000000 ); }
u32 sum() { return 1 + 2 + 3; }
u64 shifted() { return 1 << 3 << 2; }

u32 beside( u32 x )
{
    return x + 2 * 3;
}

// A comparison still answers rather than refusing.
bool impossible( u64 n )
{
    return n < 0 - 1;
}

i32 main()
{
    u8 over = 200 + 50 + 10;
    u32 under = -( 1 - 2 );
    i8 step = 100 + 100 - 1;
    u64 wide = 1 << 40 << 30;
    i32 zero = 1 / ( 2 - 2 );
    u32 negation = -( 2 - 1 );
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "16:12-16:21 warning: this comparison is always false [-Wconstant-comparison] = `-1` is outside the range of `u64`",
            "21:15-21:28 error: `260` does not fit in `u8`",
            "22:18-22:27 error: `-1` does not fit in `u32`",
            "23:15-23:24 error: `200` does not fit in `i8`",
            "24:16-24:29 error: this constant does not fit in `u64`",
            "25:16-25:29 error: division by zero",
            "26:20-26:30 error: no operator `-` for `u32` = negation needs a signed type"
        }
    );
}

TEST_CASE( "errors_constant_hints", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// One error each, with help that names the fix.
const i32 N = nope;
const i32 M = N * 2;
const i32 K = 1;

i32 main()
{
    i32[M] cascade = [ 0, .. ];
    char letter = 'a';
    i32 whole = cast<i32>( 3.0 );
    i32 negative = -2.0;
    i32 fraction = 1.5;
    i32[2] a = [ 1, 2 ];
    i32 named = a[K];
    i32 sum = a[K - 1];
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "2:15-2:19 error: `nope` is not declared",
            "9:5-9:9 error: unknown type `char` = Keel spells this `u8`",
            "10:28-10:31 error: expected `i32`, but got a floating-point literal = an integer has no `.`; write `3`",
            "11:20-11:24 error: expected `i32`, but got a floating-point literal = an integer has no `.`; write `-2`",
            "12:20-12:23 error: expected `i32`, but got a floating-point literal = a fractional value cannot be an integer",
            "14:19-14:20 error: expected `u64`, but got `i32` = write `cast<u64>(K)`, or declare `K` `u64`",
            "15:17-15:22 error: expected `u64`, but got `i32` = write `cast<u64>(K - 1)`"
        }
    );
}

TEST_CASE( "errors_constants", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A constant whose value is already known cannot be allowed to wrap silently: §12 decided that
// arithmetic wraps at run time, and this is the case where the answer exists before the program
// does. Division by zero and an out-of-range shift are undefined in C rather than merely wrong,
// and are just as knowable.
i32 main()
{
    u32 below = 1 - 2;
    u8  over = 200 + 100;
    i32 wide = 2000000000 * 2;
    i32 midway = 2000000000 + 2000000000 - 2000000000;
    u64 enormous = 18446744073709551615 * 2;

    i32 divided = 1 / 0;
    i32 remainder = 1 % 0;
    i32 computed = 1 / ( 3 - 3 );
    f64 real = 1.0 / 0.0;

    i32 shifted = 1 << 40;
    u32 exact = 1 << 32;
    i32 backwards = 1 << -1;

    f32 huge = 1e30 * 1e30;

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "7:17-7:22 error: `-1` does not fit in `u32`",
            "8:16-8:25 error: `300` does not fit in `u8`",
            "9:16-9:30 error: `4000000000` does not fit in `i32`",
            "10:18-10:41 error: `4000000000` does not fit in `i32`",
            "11:20-11:44 error: this constant does not fit in `u64`",
            "13:19-13:24 error: division by zero",
            "14:21-14:26 error: remainder by zero",
            "15:20-15:33 error: division by zero",
            "16:16-16:25 error: division by zero",
            "18:24-18:26 error: the shift count is out of range = `i32` is 32 bits wide, so the count must be between 0 and 31",
            "19:22-19:24 error: the shift count is out of range = `u32` is 32 bits wide, so the count must be between 0 and 31",
            "20:26-20:28 error: the shift count is out of range = `i32` is 32 bits wide, so the count must be between 0 and 31",
            "22:16-22:27 error: `1.0000000300949327e+60` does not fit in `f32`"
        }
    );
}

TEST_CASE( "errors_constructor_defaults", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A `const` field with a default is assigned by its constructor on every path or on none.
class Fixed
{
    const i32 limit = 9;
    i32       count = 0;

    Fixed( bool b )
    {
        if( b )
        {
            limit = 1;
        }
    }

    public i32 get() const
    {
        return limit + count;
    }
};

i32 main()
{
    Fixed f = Fixed( true );
    return f.get();
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() == Lines { "11:13-11:23 error: `limit` already holds a value here = `limit` has a default, so assign it on "
                             "every path or on none" }
    );
}

TEST_CASE( "errors_constructor_fields", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D9 for a constructor's fields: each holds a value before it is read, before `this` is used whole,
// and at every way out. An owning field is written once per path.
class Buf
{
    u8[*] p;

    Buf()
    {
        unsafe
        {
            p = alloc<u8>( 4 );
        }
    }

    ~Buf()
    {
        unsafe
        {
            free( p );
        }
    }
};

class Never
{
    i32 a;
    i32 b;

    Never()
    {
        a = 1;
    }
};

class Sometimes
{
    i32 a;

    Sometimes( bool c )
    {
        if( c )
        {
            a = 1;
        }
    }
};

class Early
{
    i32 a;

    Early( bool c )
    {
        if( c )
        {
            return;
        }

        a = 1;
    }
};

class Read_first
{
    i32 a;
    i32 b;

    Read_first()
    {
        a = b;
        b = 1;
    }
};

class Whole
{
    i32 a;
    i32 b;

    Whole()
    {
        touch();
        a = 1;
        b = 2;
    }

    void touch()
    {
    }
};

class Twice
{
    Buf b;

    Twice()
    {
        b = Buf();
        b = Buf();
    }
};

class Maybe_twice
{
    Buf b;

    Maybe_twice( bool c )
    {
        if( c )
        {
            b = Buf();
        }

        b = Buf();
    }
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "29:5-32:6 error: this constructor never assigns `b` = every field must hold a value when the constructor returns",
            "39:5-45:6 error: this constructor does not assign `a` on every path = every field must hold a value when the "
            "constructor returns",
            "56:13-56:20 error: this constructor never assigns `a` = every field must hold a value when the constructor "
            "returns",
            "70:13-70:14 error: `b` is used before this constructor assigns it = assign it before reading it",
            "82:9-82:16 error: `this` is used before this constructor assigns `a` = assign every field before calling a method "
            "or using `this`",
            "82:9-82:16 error: `this` is used before this constructor assigns `b` = assign every field before calling a method "
            "or using `this`",
            "99:9-99:19 error: `b` already holds a value here = a constructor assigns an owning field once on each path",
            "114:9-114:19 error: `b` may already hold a value here = a constructor assigns an owning field once on each path"
        }
    );
}

TEST_CASE( "errors_constructor_name", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A constructor is reached only as `C( ... )`: it has no address, and `C::C` does not name it.
class C
{
    i32 n;

    C( i32 v )
    {
        n = v;
    }
};

i32 main()
{
    auto p = &C::C;
    fn( i32 ) -> C q = &C::C;
    C a = C::C( 1 );
    auto r = C::C;
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "14:14-14:19 error: a constructor has no address = take the address of a function that returns `C(...)`",
            "15:24-15:29 error: a constructor has no address = take the address of a function that returns `C(...)`",
            "16:11-16:15 error: a constructor is not a static method = construct it as `C(...)`",
            "17:14-17:18 error: a constructor is not a static method = construct it as `C(...)`"
        }
    );
}

TEST_CASE( "errors_consuming_switch", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D7. A `switch` over `move x` takes the value, so each binding owns its payload as a local owns its
// value: returned as a local is, taken elsewhere only with `move`. The scrutinee is moved by the
// ordinary rules, so only what this function owns whole can be.
class Buf
{
    u64 n;

    Buf( u64 m )
    {
        n = m;
    }

    ~Buf()
    {
    }
};

enum Holder
{
    Full( Buf b ),
    Empty
};

enum Option<T>
{
    Some( T value ),
    None
};

void sink( move Buf b )
{
}

void copies( move Holder h )
{
    switch( move h )
    {
        case Holder::Full( b ):
            Buf c = b;
            return;
        case Holder::Empty:
            return;
    }
}

void passes( move Holder h )
{
    switch( move h )
    {
        case Holder::Full( b ):
            sink( b );
            return;
        case Holder::Empty:
            return;
    }
}

void borrowed( ref Holder h )
{
    switch( move h )
    {
        case Holder::Full( b ):
            return;
        case Holder::Empty:
            return;
    }
}

class Keeper
{
    Holder held;

    Keeper( move Holder h )
    {
        held = move h;
    }

    u64 peek()
    {
        switch( move held )
        {
            case Holder::Full( b ):
                return 1;
            case Holder::Empty:
                return 0;
        }
    }
};

// The outer switch only borrows, so its binding still belongs to the outer value.
u64 nested( Option<Holder> o )
{
    switch( o )
    {
        case Option::Some( h ):
            switch( move h )
            {
                case Holder::Full( b ):
                    return 1;
                case Holder::Empty:
                    return 0;
            }

            break;
        case Option::None:
            break;
    }

    return 0;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "39:21-39:22 error: an owning value is transferred, not copied = write `move b`",
            "51:19-51:20 error: `sink` takes ownership of this argument = write `move`",
            "60:18-60:19 error: cannot move out of a borrow = take it as `move Holder h` to own it",
            "80:22-80:26 error: a field cannot be moved on its own = it belongs to `this`, which a method only borrows",
            "96:26-96:27 error: a binding cannot be moved = `h` is part of the value being matched, which still owns it"
        }
    );
}

TEST_CASE( "errors_consuming_switch_moves", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D7. A `switch` over `move x` moves `x` where it is written, and a binding it owns moves like a
// local: each use after is caught by the same flow analysis.
class Buf
{
    u64 n;

    Buf( u64 m )
    {
        n = m;
    }

    ~Buf()
    {
    }
};

enum Holder
{
    Full( Buf b ),
    Empty
};

u64 peek( Holder _ )
{
    return 0;
}

u64 read( Buf _ )
{
    return 0;
}

void sink( move Buf _ )
{
}

void after( move Holder h )
{
    switch( move h )
    {
        case Holder::Full( b ):
            break;
        case Holder::Empty:
            break;
    }

    peek( h );
}

void inside( move Holder h )
{
    switch( move h )
    {
        case Holder::Full( b ):
            peek( h );
            return;
        case Holder::Empty:
            return;
    }
}

void twice( move Holder h )
{
    switch( move h )
    {
        case Holder::Full( b ):
            sink( move b );
            sink( move b );
            return;
        case Holder::Empty:
            return;
    }
}

void sometimes( bool c, move Holder h )
{
    switch( move h )
    {
        case Holder::Full( b ):
            if( c )
            {
                sink( move b );
            }

            read( b );
            return;
        case Holder::Empty:
            return;
    }
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "47:11-47:12 error: `h` is used after it was moved = moved at 39:13",
            "55:19-55:20 error: `h` is used after it was moved = moved at 52:13",
            "68:19-68:25 error: `b` is used after it was moved = moved at 67:19",
            "85:19-85:20 error: `b` may already have been moved = moved at 82:23 on some path to here"
        }
    );
}

TEST_CASE( "errors_containment_enums", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A payload holds its fields by value, as a struct does, so an enum can contain itself too.
enum MList
{
    Cons( i32 v, MList rest ),
    Nil,
};

// Through a struct.
struct P
{
    i32 n;
    Q   q;
};

enum Q
{
    X( P p ),
    Y,
};

// Through a second enum, neither used.
enum Object_type
{
    Sig( Type_sig s ),
    Plain,
};

enum Type_sig
{
    Of( Object_type o ),
    None,
};

// Through a generic enum's parameter.
enum Holder<T>
{
    Has( T value ),
    Empty,
};

struct Loop
{
    Holder<Loop> h;
};

// Held through `box`, it has a size.
enum List
{
    Cons( i32 v, box<List> rest ),
    Nil,
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "2:1-6:3 error: `MList` contains itself, so it has no size = MList -> MList; hold one through `box`",
            "9:1-13:3 error: `P` contains itself, so it has no size = P -> Q -> P; hold one through `box`",
            "22:1-26:3 error: `Object_type` contains itself, so it has no size = Object_type -> Type_sig -> Object_type; hold "
            "one through `box`",
            "41:1-44:3 error: `Loop` contains itself, so it has no size = Loop -> Holder -> Loop; hold one through `box`"
        }
    );
}

TEST_CASE( "errors_containment_through_arguments", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A by-value cycle can close through a type argument: `Box` holds its `T` by value, so whatever
// `Box` is given is inside whatever holds the `Box`. Each is one error, and none hangs.
struct Box<T> where T : copyable
{
    T value;
};

struct Ring<T> where T : copyable
{
    Box<Ring<T>> link;
};

// The argument grows each time round, so walking instances never meets the same one twice.
struct Spiral<T> where T : copyable
{
    Box<Spiral<Box<T>>> link;
};

// A pointer anywhere along the way ends the containment.
struct Chain<T> where T : copyable
{
    Box<Chain<T>*> link;
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "8:1-11:3 error: `Ring` contains itself, so it has no size = Ring -> Box -> Ring; hold one through `box`",
            "14:1-17:3 error: `Spiral` contains itself, so it has no size = Spiral -> Box -> Spiral; hold one through `box`"
        }
    );
}

TEST_CASE( "errors_conversions", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Every cell the conversion table refuses, and a constant `cast` that does not fit. One diagnostic
// each.
struct Point { i32 x; };

i32 main()
{
    i32   whole = 1;
    f64   real = 1.5;
    bool  flag = true;
    i32*  address = &whole;
    Point origin = Point { 0 };

    i32 rounded = cast<i32>( real );
    i32 tested = cast<bool>( whole );
    f32 promoted = wrap<f32>( whole );
    i32 counted = wrap<i32>( flag );
    u8* recast = cast<u8*>( address );
    i32 flattened = cast<i32>( origin );
    u8  overlarge = cast<u8>( 300 );

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "13:19-13:36 error: `cast` cannot convert `f64` to `i32` = rounding is not implied; this needs an explicit "
            "rounding function",
            "14:18-14:37 error: `cast` cannot convert `i32` to `bool` = compare explicitly, as in `whole != 0`",
            "15:20-15:38 error: `wrap` cannot convert `i32` to `f32` = `wrap` keeps the low bits of an integer; use `cast` "
            "here",
            "16:19-16:36 error: `wrap` cannot convert `bool` to `i32` = `wrap` keeps the low bits of an integer; use `cast` "
            "here",
            "17:18-17:38 error: converting between `i32*` and `u8*` needs an `unsafe` block = the compiler cannot check that "
            "the target type describes what is there",
            "18:21-18:40 error: `cast` cannot convert `Point` to `i32`",
            "19:31-19:34 error: `300` does not fit in `u8`"
        }
    );
}

TEST_CASE( "errors_counting", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D59: a counter is read-only, counts in an integer type, holds its own value, and its bounds
// check against its type as an initialiser does.
void take( ref u64 x )
{
    x += 1;
}

i32 main()
{
    u64 n = 3;
    for( u64 i : 0..n )
    {
        i += 1;
        i++;
        i = 0;
        ref u64 r = i;
        const ref u64 c = i; // reading is fine
        take( ref i );
    }

    for( i32 i : 0..n )
    {
    }

    for( f64 x : 0.0..1.0 )
    {
    }

    for( u64 i : 5..2 )
    {
    }

    for( u64 i : 3..3 ) // empty, not an error
    {
    }

    for( ref u64 i : 0..n )
    {
    }

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "13:9-13:10 error: `i` counts the loop, so it cannot be modified = step it yourself with `for(u64 i = a; i < b; "
            "i++)`",
            "14:9-14:10 error: `i` counts the loop, so it cannot be modified = step it yourself with `for(u64 i = a; i < b; "
            "i++)`",
            "15:9-15:10 error: `i` counts the loop, so it cannot be modified = step it yourself with `for(u64 i = a; i < b; "
            "i++)`",
            "16:21-16:22 error: `i` counts the loop, so it cannot be modified = step it yourself with `for(u64 i = a; i < b; "
            "i++)`",
            "18:19-18:20 error: `i` counts the loop, so it cannot be modified = step it yourself with `for(u64 i = a; i < b; "
            "i++)`",
            "21:21-21:22 error: expected `i32`, but got `u64` = write `cast<i32>(n)`",
            "25:18-25:26 error: a counting loop counts in an integer type",
            "29:18-29:22 error: `5..2` counts nothing = a range runs upwards; the lower bound comes first",
            "37:10-37:13 error: a counter holds its own count = write the type on its own"
        }
    );
}

TEST_CASE( "errors_cyclic_refinement", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A refused requirement cycle loses its edge, so a method reached only through it is not reported
// again: not on the interface, not on a bound reaching it, and not on a static call. A struct
// missing the kept edge's interface still is.
interface Loop implements Again
{
    i32 spin() const;
};

interface Again implements Loop
{
    i32 turn() const;
};

interface A implements C
{
    i32 a() const;
    static self make();
};

interface B implements A
{
    i32 b() const;
};

interface C implements B
{
    i32 c() const;
    static self build();
};

struct Only implements Loop
{
    i32 w;
    i32 spin() const { return 0; }
};

i32 via( const ref Again a )
{
    return a.spin() + a.turn();
}

i32 bound<T>( const ref T t ) where T : A
{
    return t.a() + t.b() + t.c();
}

void made<T>() where T : C
{
    T::make();
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "9:28-9:32 error: `Again` cannot require `Loop`, which already requires `Again`",
            "25:24-25:25 error: `C` cannot require `B`, which already requires `C`",
            "31:24-31:28 error: `Only` implements `Again` but has no method `turn` = add `i32 turn() const` to `Only`"
        }
    );
}

TEST_CASE( "errors_deduce_through_bound", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D82: deduction through a bound refuses a type that conforms twice, names the bound a type does
// not meet, and writes one placeholder per type parameter.
struct Two implements from<str>, from<u64>
{
    u64 v;
    static Two from( move str value ) { return Two { 1 }; }
    static Two from( move u64 value ) { return Two { value }; }
};

struct None
{
    u64 v;
};

u64 make<C, T>( const ref C c ) where C : from<T>
{
    return 0;
}

void flip<C, T>( ref C c ) where C : fixed_memory<T>
{
    c.swap( 0, c.size() - 1 );
}

void lone<C, T>( const ref C c )
{
}

i32 main()
{
    Two t = Two { 1 };
    make( t );
    None n = None { 1 };
    flip( ref n );
    lone( n );
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "32:5-32:9 error: `Two` implements `from` more than once, so nothing says what `T` is in `make` = it implements "
            "`from<str>` and `from<u64>`; write the type arguments, as in `make<Two, str>(...)`",
            "34:5-34:9 error: `flip` needs a `C` that implements `fixed_memory<T>`, but `None` does not = declare `None` with "
            "`implements fixed_memory<i32>`",
            "35:5-35:9 error: nothing here says what `T` is in `lone` = write the type arguments, as in `lone<i32, i32>(...)`"
        }
    );
}

TEST_CASE( "errors_deduced_interface", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// An interface is never a type argument, written or deduced: a generic's body may hold its `T` by
// value. Each refusal is at the argument that settled `T`. A pointer to one, or the implementer
// itself, is an ordinary type argument.
interface Shape
{
    f64 area() const;
};

struct Sq implements Shape
{
    f64 s;

    f64 area() const { return s; }
};

struct Ptr<T>
{
    const T* p;
};

void touch<T>( const ref T x )
{
}

f64 area_of<T>( const ref T x ) where T : Shape
{
    return x.area();
}

Ptr<T> hold<T>( const ref T x )
{
    return Ptr { &x };
}

u64 boxed<T>( const ref box<T> b )
{
    return 1;
}

u64 pointed<T>( T p ) where T : copyable
{
    return 2;
}

i32 main()
{
    Sq              q = Sq { 3.0 };
    const ref Shape s = q;
    box<Shape>      b = box<Sq>( Sq { 1.0 } );
    const Shape*    p = &q;

    touch( s );
    f64 a = area_of( s );
    auto h = hold( s );
    u64 n = boxed( b );

    u64 m = pointed( p );
    f64 c = area_of( q );
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "52:12-52:13 error: `T` cannot be `Shape`: an interface is never a type argument = pass the implementer itself, or "
            "take `const ref Shape` in a function that is not generic",
            "53:22-53:23 error: `T` cannot be `Shape`: an interface is never a type argument = pass the implementer itself, or "
            "take `const ref Shape` in a function that is not generic",
            "54:20-54:21 error: `T` cannot be `Shape`: an interface is never a type argument = pass the implementer itself, or "
            "take `const ref Shape` in a function that is not generic",
            "55:20-55:21 error: `T` cannot be `Shape`: an interface is never a type argument = pass the implementer itself, or "
            "take `const ref Shape` in a function that is not generic"
        }
    );
}

TEST_CASE( "errors_defaultable", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// `T()` needs `defaultable` and takes nothing; an unmet `defaultable` says what is missing.
T plain<T>() where T : copyable
{
    return T();
}

T given<T>( T x ) where T : defaultable
{
    return T( x );
}

T zero<T>() where T : defaultable
{
    return T();
}

enum Colour
{
    Red,
    Green
};

struct Half
{
    i32 x = 1;
    i32 y;
};

class Hidden
{
    i32 n = 1;

    private Hidden()
    {
    }

    public i32 get() const
    {
        return n;
    }
};

i32 main()
{
    Colour c = zero<Colour>();
    Half   h = zero<Half>();
    Hidden d = zero<Hidden>();
    i32    i = i32(); // outside a generic, a number writes its literal
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "4:12-4:13 error: `T()` needs `where T : defaultable` = a type parameter is called only for its default",
            "9:15-9:16 error: `T()` takes no arguments = it is the default, which is built from nothing",
            "45:21-45:27 error: `Colour` is not `defaultable`, and `zero` requires it of `T` = an enum has no default: which "
            "variant is the program's to say",
            "46:21-46:25 error: `Half` is not `defaultable`, and `zero` requires it of `T` = its field `y` has no default",
            "47:21-47:27 error: `Hidden` is not `defaultable`, and `zero` requires it of `T` = `Hidden` has no public "
            "constructor taking nothing",
            "48:16-48:19 error: `i32` is a type, not a value"
        }
    );
}

TEST_CASE( "errors_destroy", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// `destroy( p, n )` needs `unsafe`, a `T[*]` that does not point to `const`, and an integer count.
// A wrong operand inside `unsafe` is one error, not two: the block is not also called useless.
void outside( i32[*] slots )
{
    destroy( slots, 1 );
}

void single( i32* one )
{
    unsafe
    {
        destroy( one, 1 );
    }
}

void read_only( const i32[*] slots )
{
    unsafe
    {
        destroy( slots, 1 );
    }
}

void counted_by_a_bool( i32[*] slots )
{
    unsafe
    {
        destroy( slots, true );
    }
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "5:5-5:24 error: `destroy` needs an `unsafe` block = the compiler cannot tell which slots hold a value",
            "12:23-12:24 error: a `T*` holds one value, so `destroy` takes no count = write `destroy(p)`",
            "20:18-20:23 error: `destroy` cannot end a `const i32[*]`, which points to `const`",
            "28:25-28:29 error: expected `u64`, but got `bool`"
        }
    );
}

TEST_CASE( "errors_dft", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Each `@dft` function below does not read two arrays and write two, one error apiece at its directive.

@dft
void narrow<u64 N>( const ref f32[N] re, const ref f32[N] im, ref f32[N] yr, ref f32[N] yi ) {}

@dft
void three<u64 N>( const ref f64[N] re, const ref f64[N] im, ref f64[N] yr ) {}

@dft
void writes_input<u64 N>( ref f64[N] re, const ref f64[N] im, ref f64[N] yr, ref f64[N] yi ) {}

@dft
void ragged<u64 N, u64 M>( const ref f64[N] re, const ref f64[N] im, ref f64[M] yr, ref f64[M] yi ) {}

@dft
f64 returns( const ref f64[4] re, const ref f64[4] im, ref f64[4] yr, ref f64[4] yi ) { return 0.0; }

@dft
void accepted<u64 N>( const ref f64[N] re, const ref f64[N] im, ref f64[N] yr, ref f64[N] yi ) {}

@dft
void fixed( const ref f64[8] re, const ref f64[8] im, ref f64[8] yr, ref f64[8] yi ) {}

i32 main() { return 0; }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "3:1-3:5 error: `narrow` is `void( const ref f32[N], const ref f32[N], ref f32[N], ref f32[N] )`, but a `@dft` "
            "function is `void( const ref f64[N], const ref f64[N], ref f64[N], ref f64[N] )` = it reads `re` and `im` and "
            "writes their transform; `N` is a constant or its one `u64` type parameter",
            "6:1-6:5 error: `three` is `void( const ref f64[N], const ref f64[N], ref f64[N] )`, but a `@dft` function is "
            "`void( const ref f64[N], const ref f64[N], ref f64[N], ref f64[N] )` = it reads `re` and `im` and writes their "
            "transform; `N` is a constant or its one `u64` type parameter",
            "9:1-9:5 error: `writes_input` is `void( ref f64[N], const ref f64[N], ref f64[N], ref f64[N] )`, but a `@dft` "
            "function is `void( const ref f64[N], const ref f64[N], ref f64[N], ref f64[N] )` = it reads `re` and `im` and "
            "writes their transform; `N` is a constant or its one `u64` type parameter",
            "12:1-12:5 error: `ragged` is `void( const ref f64[N], const ref f64[N], ref f64[M], ref f64[M] )`, but a `@dft` "
            "function is `void( const ref f64[N], const ref f64[N], ref f64[N], ref f64[N] )` = it reads `re` and `im` and "
            "writes their transform; `N` is a constant or its one `u64` type parameter",
            "15:1-15:5 error: `returns` is `f64( const ref f64[4], const ref f64[4], ref f64[4], ref f64[4] )`, but a `@dft` "
            "function is `void( const ref f64[N], const ref f64[N], ref f64[N], ref f64[N] )` = it reads `re` and `im` and "
            "writes their transform; `N` is a constant or its one `u64` type parameter"
        }
    );
}

TEST_CASE( "errors_dft_ties", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A module finds one `@dft` for each N: two generics, or two fixed at one N, are one error a pair;
// a fixed one beside a generic, and two fixed at different N, are fine.

@dft
void first<u64 N>( const ref f64[N] re, const ref f64[N] im, ref f64[N] yr, ref f64[N] yi ) {}

@dft
void second<u64 N>( const ref f64[N] re, const ref f64[N] im, ref f64[N] yr, ref f64[N] yi ) {}

@dft
void eight( const ref f64[8] re, const ref f64[8] im, ref f64[8] yr, ref f64[8] yi ) {}

@dft
void also_eight( const ref f64[8] re, const ref f64[8] im, ref f64[8] yr, ref f64[8] yi ) {}

@dft
void four( const ref f64[4] re, const ref f64[4] im, ref f64[4] yr, ref f64[4] yi ) {}

i32 main() { return 0; }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "7:1-7:5 error: `first` and `second` are both `@dft` for every N, and one module finds both = a module finds one "
            "`@dft` for each N; a fixed one is taken over a generic at its N",
            "13:1-13:5 error: `eight` and `also_eight` are both `@dft` at N = 8, and one module finds both = a module finds "
            "one `@dft` for each N; a fixed one is taken over a generic at its N"
        }
    );
}

TEST_CASE( "errors_digit_names", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A name that starts with a digit, or a bare number where a name belongs. Each is reported once, on
// the token itself, and the declaration carries on as one with no name - so nothing after it is a
// follow-on error.
struct Point
{
    i32 3x;
    i32 y;
};

// Its only field was left out, but it was written with one, so it is not also reported as empty.
struct Numbered
{
    i32 3;
};

struct Reserved
{
    i32 if;
};

i32 first( i32 2nd )
{
    return 0;
}

i32 main()
{
    i32 v = first( 1 );
    i32 1st = v;
    i32 2 = v;

    // Used rather than declared, so there is no "not declared" after it.
    1st = v;

    // A literal is a value: this is the assignment's error, not the name's.
    3 = v;

    return 3rd;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "6:9-6:11 error: expected an identifier, found `3x` = a name cannot start with a digit",
            "13:9-13:10 error: expected an identifier, found `3` = a name cannot start with a digit",
            "18:9-18:11 error: expected an identifier, found `if` = `if` is a keyword, so it cannot be used as a name",
            "21:16-21:19 error: expected an identifier, found `2nd` = a name cannot start with a digit",
            "29:9-29:12 error: expected an identifier, found `1st` = a name cannot start with a digit",
            "30:9-30:10 error: expected an identifier, found `2` = a name cannot start with a digit",
            "33:5-33:8 error: expected an identifier, found `1st` = a name cannot start with a digit",
            "36:5-36:6 error: cannot assign to this expression = `3` is a value, not a place",
            "38:12-38:15 error: expected an identifier, found `3rd` = a name cannot start with a digit"
        }
    );
}

TEST_CASE( "errors_discarded_result", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D52. A statement may not discard a `result`, since the error it may hold would be lost. `_ = `
// discards one on purpose, and takes an owning place only by `move`.
class Buf
{
    public u64 n;

    Buf( u64 m )
    {
        n = m;
    }

    ~Buf()
    {
    }
};

enum Bad
{
    nope
};

result<i32, Bad> parse()
{
    return result::ok( 1 );
}

result<void, Bad> step()
{
    return result::ok();
}

result<result<i32, Bad>, Bad> nested()
{
    result<i32, Bad> inner = parse();
    return result::ok( inner );
}

result<void, Bad> passes()
{
    try step();
    try nested();
    _ = try nested();
    return result::ok();
}

i32 main()
{
    parse();
    step();
    _ = parse();
    _ = step();
    Buf held = Buf( 1 );
    _ = held;
    _ = move held;
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "41:5-41:17 error: a `result` cannot be discarded = use `try` to pass its error on, or `_ = try nested();` to "
            "discard it",
            "48:5-48:12 error: a `result` cannot be discarded = use `try` to pass its error on, or `_ = parse();` to discard "
            "it",
            "49:5-49:11 error: a `result` cannot be discarded = use `try` to pass its error on, or `_ = step();` to discard it",
            "53:9-53:13 error: an owning value is transferred, not copied = write `move held`"
        }
    );
}

TEST_CASE( "errors_do_while", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A `do` condition is a `bool`, as a `while`'s is, and it cannot see what the body declared: the
// body is a block of its own, closed before the test.
i32 main()
{
    i32 n = 0;
    do
    {
        n++;
    } while( n ); // error

    do
    {
        i32 inner = 1;
        n += inner;
    } while( inner < 3 ); // error

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "9:14-9:15 error: expected `bool`, but got `i32` = there is no conversion to `bool`: compare explicitly, as in `n "
            "!= 0`",
            "15:14-15:19 error: `inner` is not declared"
        }
    );
}

TEST_CASE( "errors_duplicate_cascade", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// One duplicate, one error: the first declaration answers every use of the name.
i32 twice( i32 a )
{
    return a;
}

i32 twice( i32 b )
{
    return b;
}

// Three of one signature are two mistakes, each against the first.
i32 thrice( i32 a )
{
    return a;
}

i32 thrice( i32 b )
{
    return b;
}

i32 thrice( i32 c )
{
    return c;
}

class Box
{
    i32 v;

    Box( i32 a )
    {
        v = a;
    }

    Box( i32 b )
    {
        v = b;
    }

    i32 get()
    {
        return v;
    }

    i32 get()
    {
        return v;
    }

    i32 doubled()
    {
        return get() * 2;
    }
};

// A variable keeps the name, but inside the class `counter` is still the class.
i32 counter = 0;

class counter
{
    i32 n;

    counter()
    {
        n = 0;
    }

    i32 get()
    {
        return n;
    }
};

// The same for a type that names itself in a field or a payload.
i32 Node = 0;

struct Node
{
    Node* next;
};

i32 Tree = 0;

enum Tree
{
    Leaf,
    Branch( Tree* left ),
};

i32 main()
{
    auto p    = &twice;
    Box  b    = Box( 1 );
    i32  sum  = twice( 1 ) + thrice( 2 ) + p( 3 ) + b.get() + b.doubled();
    return sum + counter;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "7:5-7:10 error: `twice` is already declared with these parameters = previous declaration is at: t.kl:2:1",
            "18:5-18:11 error: `thrice` is already declared with these parameters = previous declaration is at: t.kl:13:1",
            "23:5-23:11 error: `thrice` is already declared with these parameters = previous declaration is at: t.kl:13:1",
            "37:5-37:8 error: `Box` is already declared with these parameters = previous declaration is at: t.kl:32:5",
            "47:9-47:12 error: `get` is already declared with these parameters = previous declaration is at: t.kl:42:5",
            "61:7-61:14 error: `counter` is already declared in this scope = previous declaration is at: t.kl:59:1",
            "79:8-79:12 error: `Node` is already declared in this scope = previous declaration is at: t.kl:77:1",
            "86:6-86:10 error: `Tree` is already declared in this scope = previous declaration is at: t.kl:84:1"
        }
    );
}

TEST_CASE( "errors_dying_address", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Every address that dies at the `return` carrying it, one to a function. What outlives the call is
// accepted, and `&counter` on a global is in codegen/globals.kl.
struct Point
{
    i32 x;
};

class Box
{
    public i32 n;

    Box()
    {
        n = 1;
    }

    ~Box()
    {
    }
};

enum Shape
{
    Circle( i32 r ),
    Dot
};

i32* of_a_local()
{
    i32 x = 1;

    return &x;
}

i32* of_a_parameter( i32 v )
{
    return &v;
}

i32* of_a_field( Point p )
{
    return &p.x;
}

i32* of_a_moved_parameter( move Box b )
{
    return &b.n;
}

const i32* of_a_pattern_binding( Shape s )
{
    switch( s )
    {
        case Shape::Circle( r ):
            return &r;
        default:
            return nullptr;
    }
}

i32* through_a_binding()
{
    i32     x = 1;
    ref i32 r = x;

    return &r;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "32:12-32:14 error: `x` dies when this function returns, so its address would dangle = return the value itself, or "
            "allocate it with `alloc` and return that",
            "37:12-37:14 error: `v` dies when this function returns, so its address would dangle = return the value itself, or "
            "allocate it with `alloc` and return that",
            "42:12-42:16 error: `p` dies when this function returns, so its address would dangle = return the value itself, or "
            "allocate it with `alloc` and return that",
            "47:12-47:16 error: `b` dies when this function returns, so its address would dangle = return the value itself, or "
            "allocate it with `alloc` and return that",
            "55:20-55:22 error: `r` dies when this function returns, so its address would dangle = return the value itself, or "
            "allocate it with `alloc` and return that",
            "66:12-66:14 error: `x` dies when this function returns, so its address would dangle = return the value itself, or "
            "allocate it with `alloc` and return that"
        }
    );
}

TEST_CASE( "errors_dying_local_address", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D54: a read through a pointer after the local it was given the address of has ended, on some
// path. One error per marked line.
i32 block_scope()
{
    i32* p = nullptr;
    {
        i32 inner = 1;
        p         = &inner;
    }
    return *p; // error: has ended
}

// The path that skips the loop leaves `p` null; the one through it leaves `p` dangling.
i32 loop_scope()
{
    i32* p = nullptr;
    i32  n = 0;
    while( n < 2 )
    {
        i32 inner = n;
        p         = &inner;
        n++;
    }
    return *p; // error: has ended
}

i32 copied()
{
    i32* p = nullptr;
    {
        i32  inner = 1;
        i32* q     = &inner;
        p          = q;
    }
    return *p; // error: has ended
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "10:12-10:14 error: `inner` has ended, so `p` points at nothing = declare `inner` where `p` is",
            "24:12-24:14 error: `inner` has ended, so `p` points at nothing = declare `inner` where `p` is",
            "35:12-35:14 error: `inner` has ended, so `p` points at nothing = declare `inner` where `p` is"
        }
    );
}

TEST_CASE( "errors_empty_aggregates", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN §6.7. An aggregate with no fields emits a C struct with no members, which standard C does not
// have - so it is refused, and an `enum` with one variant is the spelling for a type with one value.
// The rule is "no fields", not "no members": a method-only class is just as empty once emitted.
struct Empty
{
};

class Empty_class
{
};

// Methods take no storage, so this is the same empty C struct as the two above.
class Only_a_method
{
    i32 get()
    {
        return 1;
    }
};

// A destructor cannot own what the type does not hold, so the emptiness is the only mistake here
// and the ownership rules stay quiet about it.
class Only_lifecycle
{
    Only_lifecycle()
    {
    }

    ~Only_lifecycle()
    {
    }
};

// Empty for every `T`, so it is refused once where it is written rather than at each instantiation.
struct Box<T>
{
};

// The alternative the diagnostic names, and it must stay legal - conflating "no fields" with "no
// data" would reject the fix the message recommends.
enum Unit
{
    Only
};

// The same point where payloads are in play: a payload-free variant beside one that carries data.
enum Shape
{
    Nothing,
    Circle( f64 radius )
};

// One field is enough, which is where the boundary sits.
struct One
{
    i32 x;
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "4:8-4:13 error: `Empty` has no fields, so it has no size = use an `enum` with one variant for a type with one "
            "value",
            "8:7-8:18 error: `Empty_class` has no fields, so it has no size = use an `enum` with one variant for a type with "
            "one value",
            "13:7-13:20 error: `Only_a_method` has no fields, so it has no size = use an `enum` with one variant for a type "
            "with one value",
            "23:7-23:21 error: `Only_lifecycle` has no fields, so it has no size = use an `enum` with one variant for a type "
            "with one value",
            "35:8-35:11 error: `Box` has no fields, so it has no size = use an `enum` with one variant for a type with one "
            "value"
        }
    );
}

TEST_CASE( "errors_empty_enums", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN §6.7. An `enum` with no variants has no values at all - nothing can ever construct one - so
// it is refused rather than left as the accidental spelling of a concept the language has not
// decided it wants. A different fact from the empty aggregate beside it: that one has no *size*.

enum Nothing
{
};

// The underlying type is written and there are still no values, which is where a check against the
// node's children rather than its variants would go quiet.
enum Tagged : u8
{
};

// Empty for every `T`, so it is refused once where it is written rather than at each instantiation.
enum Empty<T>
{
};

// One variant is enough, which is where the boundary sits - and it is the spelling the empty
// aggregate's diagnostic recommends, so it must stay legal.
enum Unit
{
    Only
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "5:6-5:13 error: `Nothing` has no variants, so it has no values = add a variant, or delete the type",
            "11:6-11:12 error: `Tagged` has no variants, so it has no values = add a variant, or delete the type",
            "16:6-16:11 error: `Empty` has no variants, so it has no values = add a variant, or delete the type"
        }
    );
}

TEST_CASE( "errors_enum_cast", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D28: `cast` answers the tag of an enum without payloads, to any integer that holds every tag.
// `wrap` on an enum, a `cast` to one, a `cast` to anything but an integer, and a `cast` of an
// enum carrying payloads are refused, as is an integer too narrow for the last tag.
enum Suit
{
    Hearts,
    Spades,
};

enum Shape
{
    Circle( f64 r ),
    Dot,
};

// The last tag is 128, one past what an `i8` holds.
enum Many
{
    T0, T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T11, T12, T13, T14, T15,
    T16, T17, T18, T19, T20, T21, T22, T23, T24, T25, T26, T27, T28, T29, T30, T31,
    T32, T33, T34, T35, T36, T37, T38, T39, T40, T41, T42, T43, T44, T45, T46, T47,
    T48, T49, T50, T51, T52, T53, T54, T55, T56, T57, T58, T59, T60, T61, T62, T63,
    T64, T65, T66, T67, T68, T69, T70, T71, T72, T73, T74, T75, T76, T77, T78, T79,
    T80, T81, T82, T83, T84, T85, T86, T87, T88, T89, T90, T91, T92, T93, T94, T95,
    T96, T97, T98, T99, T100, T101, T102, T103, T104, T105, T106, T107, T108, T109, T110, T111,
    T112, T113, T114, T115, T116, T117, T118, T119, T120, T121, T122, T123, T124, T125, T126, T127,
    T128,
};

i32 main()
{
    Suit  s   = Suit::Spades;
    Shape d   = Shape::Dot;
    Many  m   = Many::T0;
    i32   one = 1;

    u8   a = wrap<u8>( s );
    Suit b = cast<Suit>( one );
    f64  c = cast<f64>( s );
    u8   e = cast<u8>( d );
    i8   f = cast<i8>( m );
    u8   g = cast<u8>( m );

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "37:14-37:27 error: `wrap` cannot convert `Suit` to `u8` = an `enum`'s tag converts with `cast`",
            "38:14-38:31 error: `cast` cannot convert `i32` to `Suit`",
            "39:14-39:28 error: `cast` cannot convert `Suit` to `f64` = an `enum`'s tag converts only to an integer",
            "40:14-40:27 error: `cast` cannot convert `Shape` to `u8` = only an `enum` without payloads converts to its tag",
            "41:14-41:27 error: `cast` cannot convert `Many` to `i8` = its last tag is 128, which `i8` cannot hold"
        }
    );
}

TEST_CASE( "errors_enum_globals", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A global of an enum with payloads takes no initialiser yet; one of a plain enum takes any variant.
enum colour { red, green, blue };
enum shape { dot, circle( f64 r ) };

colour fine  = colour::blue;
colour wrong = colour::purple;
shape  s     = shape::dot;
shape  zero;

class Holder
{
    public i32 n;

    public static shape kept = shape::circle( 1.0 );
};

i32 main() { return 0; }
)kl",
        Stage::Reported
    );

    REQUIRE( p.shown() == Lines { "6:16-6:30 error: `colour` has no variant `purple`" } );
}

TEST_CASE( "errors_enum_recovery", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A `where` clause with nothing to constrain, on an enum rather than a function.
enum Unconstrained where T : copyable
{
    Red,
};

// Recovery: a variant with no name is left out, but the enum is not then empty - it was written with
// variants, so only the variants are reported.
enum Nameless
{
    3,
    3,
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "2:20-2:38 error: a `where` clause needs type parameters = write them on the name, as in `enum Opt<T> where T : "
            "copyable`",
            "11:5-11:6 error: expected an identifier, found `3` = a name cannot start with a digit",
            "12:5-12:6 error: expected an identifier, found `3` = a name cannot start with a digit"
        }
    );
}

TEST_CASE( "errors_enums", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN D30. `enum class` semantics without the `class`. Most of what is refused below is refused by
// a rule that already existed - holds() has no branch reaching an integer from an enum, the
// arithmetic table has no row for one, and the variants enter no lexical scope - so this fixture is
// as much a record of what is *absent* as of what was written.
enum Colour
{
    Red,
    Green
};

enum Other
{
    A,
    B
};

enum Backed : bool
{
    X
};

enum Repeats
{
    P,
    Q,
    P
};

struct Point
{
    i32 x;
};

i32 leaks_a_variant()
{
    Colour c = Red;

    return 0;
}

i32 converts_to_an_integer()
{
    i32 n = Colour::Red;

    return n;
}

i32 converts_from_an_integer()
{
    Colour c = 0;

    return 0;
}

i32 does_arithmetic()
{
    i32 n = Colour::Red + 1;

    return n;
}

i32 compares_across_enums()
{
    bool b = Colour::Red == Other::A;

    return 0;
}

i32 orders_variants()
{
    bool b = Colour::Red < Colour::Green;

    return 0;
}

i32 names_a_missing_variant()
{
    Colour c = Colour::Purple;

    return 0;
}

i32 qualifies_a_non_enum()
{
    i32 n = Point::x;

    return n;
}

i32 qualifies_something_that_is_not_a_name()
{
    i32 n = Colour::Red::Green;

    return n;
}

// Everything below is accepted, and the fixture would be worth much less without it: it is what
// shows the rules refuse conversions and arithmetic rather than every use of an enum.

Colour flip( Colour c )
{
    if( c == Colour::Red )
    {
        return Colour::Green;
    }

    return Colour::Red;
}

i32 main()
{
    Colour c = flip( Colour::Red );

    bool same = c == Colour::Green;

    return 0;
}

// D7. A payload variant is not a value until it has one, and a payload-free one is not a function.
enum Shape
{
    Circle( f64 radius ),
    Dot
};

i32 omits_the_payload()
{
    Shape s = Shape::Circle;

    return 0;
}

i32 supplies_one_that_is_not_wanted()
{
    Shape s = Shape::Dot( 1.0 );

    return 0;
}

i32 binds_too_few( Shape s )
{
    switch( s )
    {
        case Shape::Circle():
            return 1;
        default:
            return 0;
    }
}

i32 writes_a_binding( Shape s )
{
    switch( s )
    {
        case Shape::Circle( r ):
            r = 2.0;
            return 1;
        default:
            return 0;
    }
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "17:15-17:19 error: an `enum` must be backed by an integer, but `bool` is not one",
            "26:5-26:6 error: `Repeats` already has a variant `P` = previous declaration is at: t.kl:24:5",
            "36:16-36:19 error: `Red` is not declared = write `Colour::Red`",
            "43:13-43:24 error: expected `i32`, but got `Colour`",
            "50:16-50:17 error: expected `Colour`, but got an integer literal",
            "57:13-57:28 error: no operator `+` for `Colour` and `Colour`",
            "57:27-57:28 error: expected `Colour`, but got an integer literal",
            "64:14-64:37 error: no operator `==` for `Colour` and `Other` = only values of the same `enum` can be compared",
            "71:14-71:41 error: no operator `<` for `Colour` and `Colour`",
            "78:16-78:30 error: `Colour` has no variant `Purple`",
            "85:13-85:21 error: `Point` has no static field `x` = take its offset as `&Point::x`, or read it as `value.x`",
            "92:13-92:24 error: `::` needs the name of a type on its left",
            "128:15-128:28 error: `Circle` carries a payload = write `Shape::Circle(...)` with a value for each field",
            "135:15-135:32 error: `Dot` carries no payload = write it without arguments",
            "144:14-144:29 error: `Circle` carries 1 value, but 0 were bound = bind a name for each field of the payload",
            "156:13-156:14 error: `r` is bound by a pattern, so it cannot be modified = a binding is read-only; copy it into a "
            "local to change it"
        }
    );
}

TEST_CASE( "errors_error_union", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// §6.7. A union's members are enums, each written once and known where the union is written.
// `try` widens an error into a union holding all of it, and nothing else widens one union into
// another; `switch` over a union lists every member's variants, and nothing else.
enum A
{
    a1,
    a2
};

enum B
{
    b1( u64 n )
};

enum C
{
    c1
};

struct P
{
    i32 x;
};

result<u64, A | B> make_ab()
{
    return result::ok( 2 );
}

result<u64, C> make_c()
{
    return result::ok( 3 );
}

result<u64, A | i32> not_enum()
{
    return result::ok( 0 );
}

result<u64, A | P> struct_member()
{
    return result::ok( 0 );
}

result<u64, A | A> twice()
{
    return result::ok( 0 );
}

result<T, A | T> generic<T>( T v )
{
    return result::ok( v );
}

result<u64, A | B> lacks_c()
{
    u64 n = try make_c();
    return result::ok( n );
}

result<u64, A | C> lacks_b()
{
    u64 n = try make_ab();
    return result::ok( n );
}

void widened( A | B ab )
{
    A | B | C abc = ab;
}

void not_member()
{
    A | B e = C::c1;
}

void missing( A | B e )
{
    switch( e )
    {
    case A::a1:
        break;
    case B::b1( n ):
        break;
    }
}

void foreign( A | B e )
{
    switch( e )
    {
    case C::c1:
        break;
    default:
        break;
    }
}

void covered( A | B e )
{
    switch( e )
    {
    case A::a1:
    case A::a2:
        break;
    case B::b1( n ):
        break;
    default:
        break;
    }
}

void repeated( A | B e )
{
    switch( e )
    {
    case A::a1:
        break;
    case A::a1:
        break;
    default:
        break;
    }
}

// One type, so one signature.
void same( A | B e )
{
}

void same( B | A e )
{
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "35:17-35:20 error: `i32` cannot be a member of an error union = each member is an `enum`, matched by its variants",
            "40:17-40:18 error: `P` cannot be a member of an error union = each member is an `enum`, matched by its variants",
            "45:17-45:18 error: `A` is written twice",
            "50:15-50:16 error: `T` cannot be a member of an error union = a union's members are known where it is written, so "
            "none is a type parameter",
            "57:13-57:25 error: `try` passes on a `C` error, but `lacks_c` returns `result<u64, A | B>` = add `C` to `A | B`",
            "63:13-63:26 error: `try` passes on a `A | B` error, but `lacks_b` returns `result<u64, A | C>` = add `B` to `A | "
            "C`",
            "69:21-69:23 error: expected `A | B | C`, but got `A | B` = one union widens into another only through `try`",
            "74:15-74:20 error: expected `A | B`, but got `C`",
            "79:5-85:6 error: `switch` does not cover every variant of `A | B` = missing `A::a2`",
            "92:10-92:15 error: a `case` label must be a variant of `A | B`",
            "101:5-110:6 error: every variant of `A | B` is covered, so `default` can never run = remove it, so that adding a "
            "variant later is a compile error here",
            "119:10-119:15 error: `a1` is already covered = previous declaration is at: t.kl:117:10",
            "131:6-131:10 error: `same` is already declared with these parameters = previous declaration is at: t.kl:127:1"
        }
    );
}

TEST_CASE( "errors_extern", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// The FFI boundary is where the assertion belongs: every call to a C function needs an `unsafe`
// block, and the signature is checked exactly as any other function's would be.
extern i32 abs( i32 v );

i32 unguarded( i32 v )
{
    return abs( v );
}

i32 wrong_arity()
{
    i32 n = 0;

    unsafe
    {
        n = abs( 1, 2 );
    }

    return n;
}

i32 wrong_type()
{
    f64 d = 1.5;
    i32 n = 0;

    unsafe
    {
        n = abs( d );
    }

    return n;
}

// A body contradicts the declaration. One error, and it recovers as an ordinary definition - so
// `defined_here` below resolves, rather than cascading into an unknown name.
extern i32 defined_here()
{
    return 3;
}

i32 main()
{
    return defined_here();
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "7:12-7:15 error: calling `abs` needs an `unsafe` block = it is defined in C, so the compiler cannot check what it "
            "does with its arguments",
            "16:16-16:24 error: `abs` takes 1 argument, but 2 were given",
            "29:18-29:19 error: expected `i32`, but got `f64`",
            "38:1-38:2 error: an `extern` function cannot have a body = `extern` says the definition is in C: remove the body, "
            "or remove `extern` to define it here"
        }
    );
}

TEST_CASE( "errors_extern_types", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// What C cannot read by value: one error per annotation, with what to pass instead.
class handle { i32 fd; ~handle() {} };
enum shape { circle( f64 r ), dot };
struct pair<T> where T : copyable { T a; T b; };
struct label { str text; };
interface measured { f64 size() const; };

extern void close( move handle h );
extern handle open();
extern void name( str s );
extern void draw( shape s );
extern void both( pair<i32> p );
extern void tag( label l );
extern void weigh( measured* m );
extern void count( ref measured m );
extern void fill( i32[4] a );
extern const ref u8[] read_all();
extern void call( fn( str ) -> i32 cb );

// Behind a pointer, and a struct of what C can read, all cross.
struct point { i32 x; i32 y; i32[2] spare; };
extern point move_to( ref handle h, const ref label l, point to, handle* raw );

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "8:25-8:31 error: `handle` cannot be passed to C by value = `handle` is a class, and C cannot run its destructor: "
            "pass a pointer to it, or a struct of what C needs",
            "9:8-9:14 error: `handle` cannot be returned from C by value = `handle` is a class, and C cannot run its "
            "destructor: pass a pointer to it, or a struct of what C needs",
            "10:19-10:22 error: `str` cannot be passed to C by value = `str` is Keel's text, which Keel lays out: pass a "
            "`const u8[*]` and a `u64`",
            "11:19-11:24 error: `shape` cannot be passed to C by value = `shape` is an enum with a payload, which Keel lays "
            "out: pass a pointer to it, or a C-like enum and the payload's fields apart",
            "12:19-12:28 error: `pair<i32>` cannot be passed to C by value = `pair<i32>` is a generic instance, which C cannot "
            "name: declare a struct with its fields",
            "13:18-13:23 error: `label` cannot be passed to C by value = `label.text` is `str`, Keel's text, which Keel lays "
            "out: pass a `const u8[*]` and a `u64`",
            "14:20-14:29 error: `measured*` cannot be passed to C by value = `measured*` is reached through two pointers in C, "
            "the object and its table: pass the implementing type's own pointer",
            "15:24-15:32 error: `measured` cannot be passed to C = `measured` is reached through two pointers in C, the object "
            "and its table: pass the implementing type's own pointer",
            "16:19-16:25 error: `i32[4]` cannot be passed to C by value = C takes an array as a pointer: take `ref i32[4]`, or "
            "point at its elements with `i32[*]`",
            "17:8-17:22 error: a run cannot be returned from C = return a `u8[*]` and take the count through a `u64*`",
            "18:19-18:35 error: `fn(str) -> i32` cannot be passed to C by value = `str` is Keel's text, which Keel lays out: "
            "pass a `const u8[*]` and a `u64`"
        }
    );
}

TEST_CASE( "errors_failed_patterns", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A pattern that fails still binds its names. The arm reads them, and the one mistake is all that is reported.
enum Shape
{
    Dot,
    Circle( i32 r ),
    Rect( i32 w, i32 h ),
};

enum Other
{
    Circle( i32 r ),
};

struct Point
{
    i32 x;
};

i32 labels( Shape s )
{
    switch ( s )
    {
        case Shape::Square( a ):
            return a + 1;
        case Other::Circle( b ):
            return b + 1;
        case Shape::Circle:
            return 0;
        case Shape::Circle( c ):
            return c + 1;
        case Shape::Rect( w, h, d ):
            return w + h + d;
        case Shape::Dot( e ):
            return e + 1;
    }
}

i32 not_an_enum( Point p )
{
    switch ( p )
    {
        case Shape::Circle( r ):
            return r + 1;
        default:
            return 0;
    }
}

i32 unknown_type( Shapes s )
{
    switch ( s )
    {
        case Shape::Circle( r ):
            return r + 1;
        default:
            return 0;
    }
}

i32 unparsed( Shape s )
{
    switch ( s )
    {
        case Shape:: ::Circle( r ):
            return r + 1;
        default:
            return 0;
    }
}

i32 main()
{
    return labels( Shape::Dot );
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "23:14-23:27 error: `Shape` has no variant `Square`",
            "25:14-25:27 error: a `case` label must be a variant of `Shape`",
            "29:14-29:27 error: `Circle` is already covered = previous declaration is at: t.kl:27:14",
            "31:14-31:36 error: `Rect` carries 2 values, but 3 were bound = bind a name for each field of the payload",
            "33:14-33:29 error: `Dot` carries 0 values, but 1 was bound = write it without a pattern",
            "40:14-40:15 error: `switch` needs an `enum` or a number, but this is a `Point`",
            "49:19-49:25 error: unknown type `Shapes`",
            "64:21-64:21 error: expected an identifier, found `::`"
        }
    );
}

TEST_CASE( "errors_field_defaults", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// What a field's default may name: statics and other types, never the object it builds.
result<i32, i32> parse()
{
    return result::ok( 1 );
}

struct Point
{
    i32  x = 1;
    i32  y = x;
    bool b = 3;
    i32  z = this.x;
    i32  w = area();
    i32  s = Point::origin;
    i32  t = origin;
    static i32 origin = 0;

    i32 area() const
    {
        return x * y;
    }
};

class Parsed
{
    public i32 n = try parse();
};

struct Pair
{
    i32 a = 1;
    i32 b;
};

i32 main()
{
    Point p = Point { 1, 2, true, 3, 4, 5, 6 };
    Pair  q = Pair { 2 };      // positional lists every field
    Pair  r = Pair {};         // `b` has no default
    Pair  s = Pair { .b = 2 }; // accepted
    return p.x + q.a + r.a + s.a;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "10:14-10:15 error: a default cannot read the object it builds = it runs before `Point` holds a value",
            "11:14-11:15 error: expected `bool`, but got an integer literal",
            "12:14-12:18 error: a default cannot read the object it builds = it runs before `Point` holds a value",
            "13:14-13:18 error: a default cannot read the object it builds = it runs before `Point` holds a value",
            "26:20-26:31 error: a default cannot `try` = it runs where the object is built, and has no caller to pass the "
            "error to",
            "38:15-38:19 error: `Pair` has 2 fields, but 1 was given",
            "39:15-39:19 error: field `b` is not initialised"
        }
    );
}

TEST_CASE( "errors_field_moves", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN §8 per field. Where a field cannot be moved out on its own. A line ending `// error`
// reports exactly one error; every other line reports none.
class Buf
{
    u64* data;

    Buf( u64 n )
    {
        unsafe
        {
            data = alloc<u64>();
        }

        *data = n;
    }

    ~Buf()
    {
        unsafe
        {
            free( data );
        }
    }

    u64 get() const
    {
        return *data;
    }
};

struct S
{
    i32 x;
    i32 y;
};

class P
{
    public Buf a;
    public Buf b;
};

// A destructor, so its fields leave only with it.
class D
{
    public Buf v;

    ~D()
    {
    }
};

class M
{
    public Buf a;

    void give()
    {
        sink( move a ); // error
    }
};

void sink( move Buf b )
{
}

void out_of_a_destructor( move D d )
{
    sink( move d.v ); // error
}

void through_a_ref( ref P q )
{
    sink( move q.a ); // error
}

void through_a_pointer( P* q )
{
    sink( move q.a ); // error
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "59:20-59:21 error: a field cannot be moved on its own = it belongs to `this`, which a method only borrows",
            "69:16-69:19 error: a field cannot be moved out of `D` = its destructor needs every field; move the whole object "
            "instead",
            "74:16-74:19 error: cannot move out of a borrow = take it as `move P q` to own it",
            "79:16-79:19 error: a pointee cannot be moved = move the variable it points into instead"
        }
    );
}

TEST_CASE( "errors_field_pointers", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Every way a field type can be refused, in one file. The unit fixtures check that each message is
// produced; this checks the caret and the hint the author sees.
class Buffer
{
    public u8* ptr;

    ~Buffer()
    {
    }
};

class Holder
{
    public Buffer buf;
    public i32    n;
    i32           secret;
};

struct P
{
    i32 x;
};

struct Box<T> where T : copyable
{
    T v;
};

U loose<C, U>( field( C ) -> U p, const ref C obj )
{
    return p( obj );
}

i32 modes( field( ref P ) -> i32 a, field( P ) -> ref i32 b, field( P ) -> const ref i32 c, field( P ) -> const i32 d )
{
    return 0;
}

i32 scalar( field( i32 ) -> i32 a )
{
    return 0;
}

i32 main()
{
    auto a = &Holder::secret;
    auto b = &Holder::buf;
    auto c = &Box::v;
    auto d = &P::y;

    P  p;
    Holder h;

    field( P ) -> i32 x = &P::x;

    i32 e = x( ref p );
    i32 f = x( p, p );
    i32 g = x( h );

    field( Holder ) -> i32 wrong = &P::x;

    auto k = P::x;

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "31:12-31:20 error: `p` reads a `U` by copying it, which is not known to be copyable = add `where U : copyable`",
            "34:19-34:22 error: the aggregate of a field type takes no mode = one offset serves every object of the type, so "
            "write the type alone",
            "34:51-34:54 error: writing through a field type is not supported yet",
            "34:76-34:89 error: a field type that borrows is not supported yet",
            "34:107-34:112 error: a `const` here binds nothing, because a field type only reads = remove it",
            "39:20-39:23 error: `i32` has no fields = a field type reads a field of a `struct` or a `class`",
            "46:14-46:29 error: `secret` is private to `Holder` = only that type's own members may name it",
            "47:14-47:26 error: `buf` is a `Buffer`, which is not copyable, so reading it through a field type would copy it = "
            "a field type that borrows is not supported yet",
            "48:15-48:18 error: `Box` needs its type arguments here = write `Box< ... >::v`",
            "49:14-49:19 error: `P` has no member `y`",
            "56:16-56:21 error: `x` does not modify this argument = remove `ref`",
            "57:14-57:22 error: `x` reads a field of one object, but 2 were given",
            "58:16-58:17 error: expected `P`, but got `Holder`",
            "60:36-60:41 error: expected `field(Holder) -> i32`, but got `field(P) -> i32`",
            "62:14-62:18 error: `P` has no static field `x` = take its offset as `&P::x`, or read it as `value.x`"
        }
    );
}

TEST_CASE( "errors_fields", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN D9, D31 and §8 per field. A line ending `// error` reports exactly one error; every other
// line reports none.
class Buf
{
    u64* data;

    Buf( u64 n )
    {
        unsafe
        {
            data = alloc<u64>();
        }

        *data = n;
    }

    ~Buf()
    {
        unsafe
        {
            free( data );
        }
    }

    u64 get() const
    {
        return *data;
    }
};

struct S
{
    i32 x;
    i32 y;
};

class P
{
    public Buf a;
    public Buf b;
};

// A constructor, so it is assigned whole while it holds nothing.
class C
{
    public Buf b;
    public u64 n;

    C( u64 v )
    {
        b = Buf( v );
        n = v;
    }
};

// Only a destructor, which built whole covers too.
class E
{
    public Buf b;
    public u64 n;

    ~E()
    {
    }
};

// A constructor's field of a class built whole.
class Dd
{
    public C c;

    Dd()
    {
        c.n = 1; // error
    }
};

void sink( move Buf b )
{
}

void keep( move P p )
{
}

void both( move Buf b, ref P p )
{
}

void beside( move Buf b, ref Buf other )
{
}

i32 half_read()
{
    S s;
    s.x = 1;
    return s.y; // error
}

void half_written( out S s ) // error
{
    s.x = 1;
}

void whole_after_a_field( move P p )
{
    sink( move p.a );
    keep( move p ); // error
}

void maybe_moved( move P p, bool c )
{
    if( c )
    {
        sink( move p.a );
    }

    sink( move p.a ); // error
}

void aliased( move P p )
{
    both( move p.a, ref p ); // error
}

void disjoint( move P p )
{
    beside( move p.a, ref p.b );
}

void borrowed_then_moved( move P p )
{
    ref Buf r = p.a;
    sink( move p.a ); // error
    r.get();
}

void piecewise_with_a_constructor()
{
    C c;
    c.n = 1; // error
}

void piecewise_with_a_destructor()
{
    E e;
    e.n = 1; // error
}

void piecewise_out( out C c )
{
    c.n = 1; // error
}

void piecewise_maybe( bool k )
{
    C c;

    if( k )
    {
        c = C( 1 );
    }

    c.n = 1; // error
}

void whole_then_field()
{
    C c = C( 3 );
    c.n = 1;
}

void whole_out_then_field( out C c )
{
    c = C( 1 );
    c.n = 2;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "74:9-74:17 error: `c` is written one field at a time before it holds a value = assign the whole of `c` first",
            "78:21-78:22 warning: unused parameter `b` [-Wunused-parameter] = name it `_` if it is unused on purpose; it still "
            "drops when the function ends",
            "82:19-82:20 warning: unused parameter `p` [-Wunused-parameter] = name it `_` if it is unused on purpose; it still "
            "drops when the function ends",
            "86:21-86:22 warning: unused parameter `b` [-Wunused-parameter] = name it `_` if it is unused on purpose; it still "
            "drops when the function ends",
            "86:30-86:31 warning: unused parameter `p` [-Wunused-parameter] = name it `_` if it is unused on purpose",
            "90:23-90:24 warning: unused parameter `b` [-Wunused-parameter] = name it `_` if it is unused on purpose; it still "
            "drops when the function ends",
            "90:34-90:39 warning: unused parameter `other` [-Wunused-parameter] = name it `_` if it is unused on purpose",
            "98:12-98:15 error: `s.y` is used before it is initialised = assign it before reading it",
            "101:1-104:2 error: `s.y` is never assigned = an `out` parameter is the callee's promise to assign it",
            "109:11-109:17 error: `p` is used after `p.a` was moved = moved at 108:11",
            "119:11-119:19 error: `p.a` may already have been moved = moved at 116:15 on some path to here",
            "124:21-124:26 error: `p.a` is moved into this call, so the call cannot also borrow it = a call cannot take a "
            "value and borrow it at once",
            "135:11-135:19 error: `p.a` is moved while `r` still borrows it = `r` is used after this; it was bound at 134:5",
            "142:5-142:13 error: `c` is written one field at a time before it holds a value = assign the whole of `c` first",
            "148:5-148:13 error: `e` is written one field at a time before it holds a value = assign the whole of `e` first",
            "153:5-153:13 error: `c` is written one field at a time before it holds a value = assign the whole of `c` first",
            "165:5-165:13 error: `c` is written one field at a time, but may not hold a value yet = it is assigned on some "
            "paths to here, but not all - assign the whole of `c` first"
        }
    );
}

TEST_CASE( "errors_fixed_arrays", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D60: a `T[N]` holds N values inline. Its size is known at compile time, an index is a `u64`, and
// it moves only as a whole. Each mistake below is one diagnostic.
class Tok
{
    u8 b;

    public Tok()
    {
        b = 0;
    }

    ~Tok()
    {
    }
};

const u64 FOUR = 4;

// The size.
void empty( i32[0] a )
{
}

void unknown( u64 n )
{
    i32[n] a = [ 1 ];
}

void nothing( void[2] a )
{
}

// Indexing.
i32 past( i32[4] a )
{
    return a[ FOUR ];
}

void sliced( i32[4] a )
{
    const ref i32[] r = a[ 0..2 ];
}

u64 measured( i32[4] a )
{
    return a.length();
}

// Ownership.
void taken( move Tok[2] a )
{
    Tok t = move a[ 0 ];
}

void copied( move Tok[2] a )
{
    Tok t = a[ 0 ];
}

struct Holds
{
    Tok[2] toks;
};

// The literal.
void short_literal()
{
    i32[3] a = [ 1, 2 ];
}

void untyped()
{
    auto a = [ 1, 2 ];
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "20:17-20:18 error: an array's size must be at least 1",
            "26:9-26:10 error: an array's size must be known at compile time = write literals and `const` names, with "
            "arithmetic over them",
            "29:15-29:22 error: `void[2]` has no element to hold = name the element's type, as in `u8[2]`",
            "36:15-36:19 error: index 4 is past the end of `i32[4]` = its indices run from 0 to 3",
            "41:25-41:34 error: `i32[4]` cannot be sliced",
            "46:12-46:20 error: `i32[4]` has no method `length` = an array has `size()`, `swap(i, j)` and `replace(i, move v)`",
            "52:18-52:24 error: an element of an array cannot be moved out = the array moves only as a whole",
            "57:13-57:19 error: an owning value is transferred, not copied = it stays in the array, which moves only as a "
            "whole",
            "62:5-62:17 error: a struct cannot contain `Tok[2]`, which owns a resource = a struct is copied freely, so declare "
            "`Holds` as a class if it owns this",
            "68:16-68:24 error: `i32[3]` holds 3 values, but this lists 2 = a literal lists every element",
            "73:14-73:22 error: an array literal needs a type to fill = write its type on the variable, as in `i32[3] a = [1, "
            "2, 3];`"
        }
    );
}

TEST_CASE( "errors_fixed_memory", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D82: a `T[N]` is a `fixed_memory` of its own element only.
void flip<C, T>( ref C c ) where C : fixed_memory<T>
{
    c.swap( 0, c.size() - 1 );
}

i32 main()
{
    i32[3] a = [ 1, 2, 3 ];
    flip<i32[3], i64>( ref a );
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines { "10:10-10:16 error: `flip` needs a `C` that implements `fixed_memory<i64>`, but `i32[3]` does not" }
    );
}

TEST_CASE( "errors_fixed_memory_index", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D82: through a bound, `c[i]` under `const ref C` is not written, is never a slice, and needs a bound
// that declares `operator[]`.
void write<C, T>( const ref C c ) where C : fixed_memory<T>, where T : copyable
{
    c[0] = c[1];
}

void slice<C, T>( const ref C c ) where C : fixed_memory<T>
{
    const ref T[] s = c[0..1];
}

void plain<C>( const ref C c ) where C : hashable
{
    const ref i32 x = c[0];
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "5:5-5:9 error: `c` is `const` = remove `const` to modify it",
            "10:23-10:30 error: `C` cannot be sliced",
            "15:23-15:27 error: `C` has no `operator[]`"
        }
    );
}

TEST_CASE( "errors_float_literal_overflow", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A float literal beyond the largest `f64` is out of range, whatever type it is given.
f64 a = 1.8e308;
f32 b = 1e400;
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "2:9-2:16 error: `1.8e308` does not fit in `f64` = the largest `f64` is 1.7976931348623157e308",
            "3:9-3:14 error: `1e400` does not fit in `f64` = the largest `f64` is 1.7976931348623157e308"
        }
    );
}

TEST_CASE( "errors_fn_bounds", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// An `fn` bound takes a lambda or `&f` of its signature, each its own type; `f` is only called or passed on.
i32 apply<F>( F f, i32 x ) where F : fn( i32 ) -> i32 { return f( x ); }
void two<F>( F a, F b ) where F : fn() -> void { a(); b(); }
void first<T, F>( T a, F f ) where F : fn( T ) -> void { }
bool odd( i32 x ) { return x % 2 != 0; }

i32 undeduced() { first( 1, fn[]( x ) { } ); return 0; }
i32 returns() { return apply( fn[]( a ) -> bool { return true; }, 1 ); }
i32 arity() { return apply( fn[]( a, b ) -> i32 { return a; }, 1 ); }
i32 address() { return apply( &odd, 1 ); }
i32 literal() { return apply( 5, 1 ); }
void each() { two( fn[]() { }, fn[]() { } ); }

F keep<F>( F f ) where F : fn() -> void { return f; }
void hold<F>( F f ) where F : fn() -> void { F* p = nullptr; }
i32 add<F>( F f ) where F : fn() -> i32 { return f + 1; }
i32 member<F>( F f ) where F : fn() -> i32 { return f.x; }
i32 store<F>( F f ) where F : fn() -> i32 { fn() -> i32 g = f; return g(); }
struct S<F> where F : fn() -> void { i32 x; };
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "7:19-7:24 error: nothing here says what `T` is in `first` = write the type arguments, as in `first<i32, "
            "i32>(...)`",
            "8:31-8:65 error: expected `fn(i32) -> i32`, but got `fn(i32) -> bool`",
            "9:33-9:41 error: this lambda takes 2 parameters, but `fn(i32) -> i32` takes 1",
            "10:31-10:35 error: expected `fn(i32) -> i32`, but got `fn(i32) -> bool` = one signature is never widened into "
            "another, so the two have to match exactly",
            "11:31-11:32 error: expected `fn(i32) -> i32`, but got an integer literal",
            "12:32-12:42 error: each lambda has a type of its own, so this one cannot be `fn[…] at 12:20` = give it a type "
            "parameter of its own",
            "14:1-14:2 error: `F` is a function passed to the call, so it can only be a parameter's type = call it, or pass it "
            "on to a parameter whose type has an `fn` bound",
            "14:50-14:51 error: `f` is a function passed to this call, so it can only be called or passed on",
            "15:46-15:47 error: `F` is a function passed to the call, so it can only be a parameter's type = call it, or pass "
            "it on to a parameter whose type has an `fn` bound",
            "16:50-16:51 error: `f` is a function passed to this call, so it can only be called or passed on",
            "17:53-17:54 error: `f` is a function passed to this call, so it can only be called or passed on",
            "18:61-18:62 error: `f` is a function passed to this call, so it can only be called or passed on",
            "19:23-19:35 error: an `fn` bound is written on a function's type parameter = a type stores an `fn` value as a "
            "field of `fn` type"
        }
    );
}

TEST_CASE( "errors_for_container", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D74: a `for` over a type with no walk is refused, naming the index form. One error: the body and
// what follows are still checked.
i32 main()
{
    i32[*] xs = nullptr;
    for( const ref i32 x : xs )
    {
        i32 y = 1;
    }

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE( p.shown() == Lines { "6:28-6:30 error: `i32[*]` has no walk = count over it: `for(u64 i : 0..xs.size())`" } );
}

TEST_CASE( "errors_for_header", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// One mistake in a `for` header is one error: the header recovers at its own `)`, so the body, the
// rest of the class and its fields all still parse. Prefix `++` is refused with its postfix spelling.
class Counter
{
    Counter()
    {
        total = 0;
        count = 4;
    }

    void run()
    {
        for( u64 i = 0; i < count; ++i )
        {
            total = total + i;
        }
    }

    void stray()
    {
        for( u64 i = 0; i < count; i++ i )
        {
            total = total + i;
        }
    }

    u64 total;
    u64 count;
};

i32 main()
{
    i32 n = 1;
    --n;
    return n;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "13:36-13:39 error: `++` is written after the variable = write `i++`",
            "21:39-21:39 error: expected `)`, found `i`",
            "34:5-34:8 error: `--` is written after the variable = write `n--`"
        }
    );
}

TEST_CASE( "errors_forward_declaration", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A prototype is a parse error under D18. It must not also make the definition below look like a
// duplicate: the parser keeps only its name, as an error symbol, which the definition replaces.
i32 odd( i32 n );

i32 odd( i32 n )
{
    return n;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() == Lines { "3:17-3:18 error: expected `{`, found `;` = Keel has no forward declarations: write the "
                             "definition here, or `extern` if it is defined in C" }
    );
}

TEST_CASE( "errors_function_pointer_field", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A field holding a function is called by its name, and the call answers to the field's rules and to
// its signature's: a private field is refused outside its type, and the arguments are checked as a
// call through a variable checks them.
i32 twice( i32 a )
{
    return a * 2;
}

void bump( ref i32 v )
{
    v = v + 1;
}

struct Holder
{
    fn( i32 ) -> i32 cb;
    i32              n;

    // Not a function, so not callable by its bare name either.
    i32 bare() const
    {
        return n();
    }

    // No object, so no field to read.
    static i32 none()
    {
        return cb( 1 );
    }
};

class Guard
{
    fn() -> i32 hidden;

    Guard()
    {
        hidden = &one;
    }
};

i32 one()
{
    return 1;
}

struct Counter
{
    fn( ref i32 ) -> void step;
};

i32 main()
{
    Holder h = Holder { &twice, 0 };

    i32 a = h.cb();
    i32 b = h.cb( true );
    i32 c = h.cb<i32>( 1 );
    i32 d = h.n();

    Guard g = Guard();
    i32   e = g.hidden();

    Counter k = Counter { &bump };
    i32     v = 1;
    k.step( v );

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "22:16-22:17 error: `n` is not callable",
            "28:16-28:18 error: `cb` is a field, and there is no object here to read it from = take one as a parameter, or "
            "make this a method",
            "56:17-56:19 error: `cb` takes 1 argument, but 0 were given",
            "57:19-57:23 error: expected `i32`, but got `bool`",
            "58:17-58:22 error: `cb` is not a generic",
            "59:13-59:16 error: `n` is a field of `Holder`, not a method; it cannot be called",
            "62:15-62:23 error: `hidden` is private to `Guard` = only that type's own members may name it",
            "66:13-66:14 error: `step` may modify this argument = write `ref`"
        }
    );
}

TEST_CASE( "errors_function_pointer_modes", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// The parameter spellings that stay errors once modes are part of a function type. A mode on the
// *return* has its own fixture, and the refusal this file used to record for `&peek` is gone: a
// function returning a `const ref` has a type now.

i32 main()
{
    fn( const i32 ) -> i32     a;
    fn( ref const i32 ) -> i32 b;

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "7:9-7:14 error: a `const` here binds nothing, because a function type has no callee = remove it - whether the "
            "callee copies is its own business",
            "8:13-8:18 error: `const` comes before the mode = write `const ref T`"
        }
    );
}

TEST_CASE( "errors_function_pointer_returns", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A function type's return slot carries only `const ref` and refuses the rest: a `ref` result is
// for declarations, and `out` and `move` say how an *argument* travels. A bare `const` binds nothing, because a type has no callee.
void mutable_borrow( fn( i32 ) -> ref i32 p )
{
}

void assigns( fn( i32 ) -> out i32 p )
{
}

void transfers( fn( i32 ) -> move i32 p )
{
}

void promises( fn( i32 ) -> const i32 p )
{
}

// Accepted, and the reason the four above are each one error rather than two.
void borrows( fn( const ref i32 ) -> const ref i32 p )
{
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "3:35-3:38 error: a function type returns only `const ref` = write `const ref i32`",
            "7:28-7:31 error: `out` is not a return mode = `out` says how an argument travels, and a return is not an argument",
            "11:30-11:34 error: `move` is not a return mode = `move` says how an argument travels, and a return is not an "
            "argument",
            "15:29-15:34 error: a `const` here binds nothing, because a function type has no callee = remove it - whether the "
            "callee copies is its own business"
        }
    );
}

TEST_CASE( "errors_function_pointers", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Every way an address can fail to name one function, in one file. The unit fixtures check that
// each message is produced; this checks what the author actually sees - the caret, the hint, and
// that two neighbouring mistakes do not run into one another.

u8 narrow( u8 a )
{
    return a;
}

i32 plain( i32 a )
{
    return a;
}

i32 borrows( ref i32 a )
{
    return a;
}

i32 over( i32 a )
{
    return a;
}
f64 over( f64 a )
{
    return a;
}

T bounded<T>( T a ) where T : integral
{
    return a;
}

i32 unbounded<T>( T a )
{
    return 1;
}

i32 main()
{
    // An overload set with nothing to choose by, and one with an expectation nobody has.
    auto           a = &over;
    fn( i32 )->f64 b = &over;

    // A generic with no instance written, and one written wrongly twice over.
    auto c = &bounded;
    auto d = &bounded<i32, f64>;
    auto e = &bounded<f64>;

    // Type arguments where there are no type parameters, and type arguments on a set.
    auto           f = &plain<i32>;
    fn( i32 )->i32 g = &over<i32>;

    // A parameter travels as an address while a function type says by value. Only the unbounded
    // one is left: an owning type answers for itself, and a type parameter cannot.
    auto j = &unbounded<i32>;

    // A signature is not widened to another signature, however its values would widen.
    fn( i32 )->i32 k = &narrow;

    // Nor is a mode erased to reach one: the two differ in how the argument travels, so they are
    // two types and neither is convertible to the other.
    fn( i32 )->i32     o = &borrows;
    fn( ref i32 )->i32 r = &plain;

    // And there is no `fn( i32 ) -> i32*` to name, so a variable holding one has no address.
    fn( i32 )->i32 m = &plain;
    auto           n = &m;

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "42:24-42:29 error: `over` is overloaded, so its address names no one function = assigning it to a variable of one "
            "signature will choose between them",
            "43:24-43:29 error: no overload of `over` has the type `fn(i32) -> f64` = its overloads are `fn(i32) -> i32` and "
            "`fn(f64) -> f64`",
            "46:14-46:22 error: `bounded` is generic, so its address names no one function = write its type arguments, as in "
            "`&bounded<i32>`",
            "47:22-47:32 error: `bounded` takes 1 type argument, but 2 were given = `bounded` declares `T`",
            "48:23-48:26 error: `f64` is not `integral`, and `bounded` requires it of `T` = it has to be an integer",
            "51:30-51:35 error: `plain` is not generic, so it takes no type arguments = remove them",
            "52:24-52:34 error: `over` is overloaded, so its type arguments choose nothing = give the generic a name of its "
            "own, or drop the type arguments and choose by the signature",
            "56:14-56:29 error: `unbounded` takes `T`, which is not known to be copyable, so its address has no function type "
            "yet = add `where T : copyable` to `unbounded`, whose parameter is at t.kl:34:19",
            "59:24-59:31 error: expected `fn(i32) -> i32`, but got `fn(u8) -> u8` = one signature is never widened into "
            "another, so the two have to match exactly",
            "63:28-63:36 error: expected `fn(i32) -> i32`, but got `fn(ref i32) -> i32` = one signature is never widened into "
            "another, so the two have to match exactly",
            "64:28-64:34 error: expected `fn(ref i32) -> i32`, but got `fn(i32) -> i32` = one signature is never widened into "
            "another, so the two have to match exactly",
            "68:24-68:26 error: `fn(i32) -> i32` is already an address, so it has none of its own = drop the `&`"
        }
    );
}

TEST_CASE( "errors_functions_of", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// `@functions_of` refuses a struct of the wrong shape, a type no function has, and an `@located`
// match, which a list would otherwise leave out silently.

struct wrong
{
    i32 name;
    fn( i32 ) -> i32 run;
};

struct unmatched
{
    str name;
    fn( u8 ) -> u8 run;
};

struct traced_step
{
    str name;
    fn( i64 ) -> i64 run;
};

@located
i64 traced( i64 v )
{
    return v;
}

i32 main()
{
    wrong[1] a = @functions_of( wrong );
    unmatched[1] b = @functions_of( unmatched );
    traced_step[1] c = @functions_of( traced_step );

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "23:5-23:11 error: `traced` has the type `@functions_of` lists, but it is `@located`, so it has no address = "
            "remove `@located`, or give `traced` another parameter to leave it out",
            "30:18-30:40 error: `@functions_of` fills a struct of a `str` and a function type, which `wrong` is not = as in "
            "`struct entry { str name; fn(ref context) -> void body; };`",
            "31:22-31:48 error: no function in this package has the type `fn(u8) -> u8` = `@functions_of` lists the functions "
            "of this package's modules that the program imports"
        }
    );
}

TEST_CASE( "errors_generic_aggregates", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// The aggregate half of the generic surface. errors_generics.kl covers a generic's signature and
// the arguments a call site names; the mistakes here are made in a type *annotation*, in a field,
// or in the shape of the declaration itself - three places a call never reaches.

struct Box<T> where T : copyable
{
    T v;
};

struct Counted<T> where T : integral
{
    T n;
};

struct Plain
{
    i32 v;
};

enum Suit { Clubs, Hearts };

// A pointer to itself is finite, and is how a list is written - so this one is silent.
class Node<T> where T : copyable
{
    Node<T>* next;
};

// And a generic holding a *concrete* instantiation of another is ordinary containment:
// `Holder<i32>` needs `Box<i32>`, and `Box<i32>` needs nothing back.
struct Holder<T> where T : copyable
{
    Box<i32> b;
};

// The mirror of a generic call written without its type arguments. Inference is a decision of its
// own and is not taken here, so the message names the explicit form rather than guessing.
void written_bare()
{
    Box b;
}

// Two ways to be "not a generic", and they are not the same message: an aggregate that takes no
// type arguments can be told what to write instead, and an enum cannot.
void not_a_generic()
{
    Plain<i32> p;
    Suit<i32>  s;
}

// The count and the bounds are checked by the same helper a call site uses, but the caret lands on
// the annotation's argument list rather than on a callee.
void wrong_arguments()
{
    Box<i32, f64> many;
    Counted<f64>  unbounded;
}

// An unresolved name in either half. Reported once, and the aggregate is not interned as
// `Box<<error>>`, which would spell nothing and which every later diagnostic would name.
void unknown_names()
{
    Nope<i32> base;
    Box<Nope> argument;
}

// Two instantiations are two types, and the diagnostic has to say which - so this is also what
// pins an instantiated name rendering at all.
void two_instantiations()
{
    Box<i32> a;
    Box<f64> b;

    a = b;
}

// A field is read at the type argument, never at the parameter: `T` is true of the template and of
// no value anyone holds, so naming it here would name something the author never wrote.
void wrong_field_type()
{
    Box<i32> b;

    b.v = true;
}

// A by-value member of its own type has no size. One mistake, so one message: the growing argument
// below is the more basic half of the same fault and stays quiet about it.
struct Odd<T> where T : copyable
{
    Odd<T> inner;
};

struct Deep<T> where T : copyable
{
    Deep<Box<T>> inner;
};

// The other half. A pointer hides the cycle from the size rule entirely, and this still names an
// instance set that never finishes - each round builds a type argument out of the last.
class Grows<T> where T : copyable
{
    Grows<Box<T>>* next;
};

// And the cycle can close through a type *argument* rather than through the type named: `Ring`
// never names `Ring` - it names `Box`, whose argument does.
struct Ring<T> where T : copyable
{
    Box<Ring<Box<T>>>* link;
};

// A method signature is the same edge a field is. The cycle closes through a return type here and
// through a parameter below, and the two are separate declarations because one message is reported
// per generic however many ways round it goes.
class Returns<T> where T : copyable
{
    T v;

    Returns<Box<T>>* make()
    {
        unsafe { return alloc<Returns<Box<T>>>(); }
    }
};

class Takes<T> where T : copyable
{
    T v;

    void give( Takes<Box<T>>* p )
    {
    }
};

// And the shape that must stay silent: naming its own generic at the argument it was given is
// forwarding, not building, so the instance set is whatever was written and stops there. This is
// what every method of a container looks like.
class Same<T> where T : copyable
{
    T v;

    Same<T>* sibling( Same<T>* p )
    {
        return p;
    }
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "39:5-39:8 error: `Box` is generic, so its type arguments must be written = as in `Box<i32>`",
            "46:5-46:15 error: `Plain` is not a generic = it takes no type arguments, so write `Plain` on its own",
            "47:5-47:14 error: `Suit` is not a generic = it takes no type arguments, so write `Suit` on its own",
            "54:8-54:18 error: `Box` takes 1 type argument, but 2 were given = `Box` declares `T`",
            "55:13-55:16 error: `f64` is not `integral`, and `Counted` requires it of `T` = it has to be an integer",
            "62:5-62:14 error: unknown type `Nope`",
            "63:9-63:13 error: unknown type `Nope`",
            "73:9-73:10 error: expected `Box<i32>`, but got `Box<f64>`",
            "82:11-82:15 error: expected `i32`, but got `bool`",
            "87:1-90:3 error: `Odd` contains itself, so it has no size = Odd -> Odd; hold one through `box`",
            "92:1-95:3 error: `Deep` contains itself, so it has no size = Deep -> Deep; hold one through `box`",
            "101:5-101:25 error: `Grows` is instantiated with a bigger type argument each time round = `Grows` to `Grows`, and "
            "each round builds a type out of the last - so no set of instances ever finishes; forward the parameter itself, or "
            "take the built type as a second parameter",
            "108:5-108:29 error: `Ring` is instantiated with a bigger type argument each time round = `Ring` to `Ring`, and "
            "each round builds a type out of the last - so no set of instances ever finishes; forward the parameter itself, or "
            "take the built type as a second parameter",
            "118:5-118:21 error: `Returns` is instantiated with a bigger type argument each time round = `Returns` to "
            "`Returns`, and each round builds a type out of the last - so no set of instances ever finishes; forward the "
            "parameter itself, or take the built type as a second parameter",
            "128:16-128:30 error: `Takes` is instantiated with a bigger type argument each time round = `Takes` to `Takes`, "
            "and each round builds a type out of the last - so no set of instances ever finishes; forward the parameter "
            "itself, or take the built type as a second parameter"
        }
    );
}

TEST_CASE( "errors_generic_blocks", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// M12: what a block with parameters of its own may not be. One error per marked line.
interface Named
{
    i32 code() const;
};

struct Pair<A, B> where A : copyable, where B : copyable
{
    A a;
    B b;
};

implements Named for i32
{
    i32 code() const { return this; }
};

// A conditional block for this package's own type is allowed; an unconditional one is not.
implements<A, B> Named for Pair<A, B> where A : copyable & Named, where B : copyable
{
    i32 code() const { return 1; }
};

implements<A> Named for Pair<A, A> where A : copyable // error: could meet the block above
{
    i32 code() const { return 2; }
};

implements<T> Named for T // error: a bare parameter is every type
{
    i32 code() const { return 3; }
};

implements<A, B> equatable for Pair<A, i32> where A : copyable // error: `B` is not in the type
{
    bool operator==( const ref Pair<A, i32> other ) const { return true; }
};

implements<T> hashable for T* // error: every pointer is hashed by the compiler
{
    void hash( ref hasher h ) const { }
};

implements<A> equatable for Pair<A, u8> where A : copyable & numeric // error: not an interface
{
    bool operator==( const ref Pair<A, u8> other ) const { return true; }
};

i32 read<T>( const ref T x ) where T : Named
{
    return x.code();
}

struct Opaque
{
    i32 x;
};

implements<T> equatable for Pair<T, u8> where T : copyable & equatable
{
    bool operator==( const ref Pair<T, u8> other ) const { return this.a == other.a; }
};

i32 main()
{
    Pair<bool, i32> p = Pair<bool, i32> { true, 1 };
    Pair<Opaque, u8> q = Pair<Opaque, u8> { Opaque { 1 }, 2 };
    if( q == q ) // error: `Opaque` is not `equatable`, which the block asks of `T`
    {
        return 1;
    }
    return read( p ); // error: `bool` is not `Named`, which the block asks of `A`
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "24:15-24:20 error: `Pair<A, A>` could meet `Pair<A, B>` at one type, and both implement `Named` = the block at "
            "t.kl:19:28 implements it first; a `where` does not keep blocks apart, so narrow one's type",
            "29:25-29:26 error: `T` cannot implement an interface through a block = `T` alone would be every type; write the "
            "type `T` is part of",
            "34:15-34:16 error: `B` is not named in `Pair<A, i32>` = a block's parameters are bound by matching its type, so "
            "each must appear there",
            "39:28-39:30 error: a block cannot make every pointer `hashable` = the compiler compares and hashes a pointer by "
            "its address",
            "44:62-44:69 error: a block cannot be conditional on `numeric` yet = a block's `where` names interfaces, and "
            "`copyable` where its type requires it",
            "68:9-68:15 error: `Pair<Opaque, u8>` has no `operator==` = the block at t.kl:59:29 makes `Pair<T, u8>` implement "
            "`equatable` only where `T : equatable`, which `Opaque` is not",
            "72:18-72:19 error: `read` needs a `T` that implements `Named`, but `Pair<bool, i32>` does not = the block at "
            "t.kl:19:28 makes `Pair<A, B>` implement `Named` only where `A : Named`, which `bool` is not"
        }
    );
}

TEST_CASE( "errors_generic_conversions", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// What `cast` and `wrap` refuse when one end is a type parameter. The rule is "legal for every type
// the bound admits", so every error here is a whole admissible *set* being rejected on the strength
// of one member of it - and each message names that member, because `T` is not something an author
// can act on and `f64` is.
//
// The correct code at the top is as much of the test as the errors: a line about it appearing in
// stderr means the rule has started refusing conversions that are fine at every instantiation.

i32 fine_wrap<T>( T v ) where T : copyable & integral
{
    return wrap<i32>( v );
}

// Narrowing is checked at run time in each instance that needs it.
i8 fine_narrowing_cast<T>( T v ) where T : copyable & integral
{
    return cast<i8>( v );
}

f64 fine_cast<T>( T v ) where T : copyable & floating
{
    return cast<f64>( v );
}

// No `where` clause at all, so the parameter promises nothing and there is no set to check against.
// The help asks for the bound rather than naming a type, because there is no type to name.
i32 unbounded<T>( ref T v )
{
    return cast<i32>( v );
}

// A bound that exists but is not numeric. `copyable` is every type that is not owning, which is
// most structs - none of which convert to anything. This is the case a vacuous product would let
// through, because the admissible *numeric* types all convert to `f64` perfectly well.
f64 not_numeric<T>( T v ) where T : copyable
{
    return cast<f64>( v );
}

// `equatable` is the same hole wearing a bound the author did write: it admits `bool`, every `enum`
// and every pointer alongside the numbers.
i32 equatable_only<T>( T v ) where T : copyable & equatable
{
    return cast<i32>( v );
}

// `numeric` admits the floats, and float to int is refused for having no one obvious rounding.
i32 numeric_to_int<T>( T v ) where T : copyable & numeric
{
    return cast<i32>( v );
}

// The parameter as the target rather than the source. The witness is drawn from the admissible set
// on that side instead.
T into_integral<T>( f64 v ) where T : copyable & integral
{
    return cast<T>( v );
}

// `wrap` keeps the low bits of an integer, and there are no low bits to keep on the way to a float.
T wrap_into_float<T>( i32 v ) where T : copyable & floating
{
    return wrap<T>( v );
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "29:12-29:26 error: `cast` cannot convert `T` to `i32` = write `where T : numeric` to promise it is a number",
            "37:12-37:26 error: `cast` cannot convert `T` to `f64` = write `where T : numeric` to promise it is a number",
            "44:12-44:26 error: `cast` cannot convert `T` to `i32` = write `where T : numeric` to promise it is a number",
            "50:12-50:26 error: `cast` cannot convert `T` to `i32` = `T` may be `f32`, and rounding is not implied; this needs "
            "an explicit rounding function",
            "57:12-57:24 error: `cast` cannot convert `f64` to `T` = `T` may be `i8`, and rounding is not implied; this needs "
            "an explicit rounding function",
            "63:12-63:24 error: `wrap` cannot convert `i32` to `T` = `wrap` keeps the low bits of an integer; use `cast` here"
        }
    );
}

TEST_CASE( "errors_generic_enums", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A generic `enum` is instantiated, bounded and cycle-checked exactly as a generic aggregate is.
// One declaration per message, and a clean one at the end, since stderr is diffed whole.

class Buf
{
    i32 v;
    ~Buf() { }
};

struct Box<T> where T : copyable
{
    T v;
};

enum Opt<T> where T : copyable
{
    None,
    Some( T v )
};

enum Pair<A, B> where A : copyable where B : copyable
{
    Left( A a ),
    Right( B b )
};

// A payload holding its own enum by value has no size; that is reported, not the growing argument,
// which `errors_generic_enums_growth` covers through a pointer.
enum Bad<T> where T : copyable
{
    V( Bad<Box<T>> b )
};

// The same, with the argument built out of a generic `enum` rather than a struct.
enum Worse<T> where T : copyable
{
    V( Worse<Opt<T>> w )
};

i32 main()
{
    Opt<i32, f64> too_many;
    Pair<i32>     too_few;
    Opt           bare;
    Opt<Buf>      unbounded;

    // Two arguments are two types, and neither converts to the other.
    Opt<i32> a;
    Opt<f64> b;
    a = b;

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "29:1-32:3 error: `Bad` contains itself, so it has no size = Bad -> Bad; hold one through `box`",
            "35:1-38:3 error: `Worse` contains itself, so it has no size = Worse -> Worse; hold one through `box`",
            "42:8-42:18 error: `Opt` takes 1 type argument, but 2 were given = `Opt` declares `T`",
            "43:9-43:14 error: `Pair` takes 2 type arguments, but 1 was given = `Pair` declares `A` and `B`",
            "44:5-44:8 error: `Opt` is generic, so its type arguments must be written = as in `Opt<i32>`",
            "45:9-45:12 error: `Buf` is not `copyable`, and `Opt` requires it of `T` = it owns something, itself or through a "
            "field, so copying it would own that twice",
            "50:9-50:10 error: expected `Opt<i32>`, but got `Opt<f64>`"
        }
    );
}

TEST_CASE( "errors_generic_enums_growth", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D42 through a payload: `Bad` reaches `Bad` at a bigger argument each time round. Through a
// pointer, so it has a size and the growth is the only fault. The edge is recorded from the *enum*,
// so the cycle closes on the declaration.
struct Box<T> where T : copyable
{
    T v;
};

enum Opt<T> where T : copyable
{
    None,
    Some( T v )
};

enum Bad<T> where T : copyable
{
    V( Bad<Box<T>>* b )
};

// The growing argument built out of a generic `enum`, which counts as a construction over the
// parameter the way a struct does.
enum Worse<T> where T : copyable
{
    V( Worse<Opt<T>>* w )
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "17:8-17:22 error: `Bad` is instantiated with a bigger type argument each time round = `Bad` to `Bad`, and each "
            "round builds a type out of the last - so no set of instances ever finishes; forward the parameter itself, or take "
            "the built type as a second parameter",
            "24:8-24:24 error: `Worse` is instantiated with a bigger type argument each time round = `Worse` to `Worse`, and "
            "each round builds a type out of the last - so no set of instances ever finishes; forward the parameter itself, or "
            "take the built type as a second parameter"
        }
    );
}

TEST_CASE( "errors_generic_interfaces", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// M9.5: what a generic interface refuses. Each refusal is named in the comment above it.
interface sink<T>
{
    void put( T value );
};

interface counted_sink<T> implements sink<T>
{
    u64 count() const;
};

interface copy_sink<T> where T : copyable
{
    void take( T value );
};

// `self_loop` cannot require itself, whatever the arguments.
interface self_loop<T> implements self_loop<i32>
{
    void f( T value );
};

// `loop_b` cannot require `loop_a`, which already requires `loop_b`.
interface loop_a<T> implements loop_b<i32>
{
    void f( T value );
};

interface loop_b<U> implements loop_a<U>
{
    void g( U value );
};

interface Shape
{
    f64 area() const;
};

class Owner
{
    u8[*] bytes;

    public Owner() { unsafe { bytes = alloc<u8>( 1 ); } }
    ~Owner() { unsafe { free( bytes ); } }
};

// `sink` needs one type argument.
struct bare implements sink
{
    i32 v;
};

// `sink` takes one type argument, not two.
struct twice implements sink<i32, i32>
{
    i32 v;
};

// `sink<i64>` is written twice.
struct repeated implements sink<i64>, sink<i64>
{
    i32 v;

    void put( i64 value ) { }
};

// `sink<bool>` has no `put( bool )`; `sink<i64>` is met.
struct half implements sink<i64>, sink<bool>
{
    i32 v;

    void put( i64 value ) { }
};

// `counted_sink<i32>` requires `sink<i32>`, and this declares only `sink<i64>`.
struct unrefined implements counted_sink<i32>, sink<i64>
{
    i32 v;

    void put( i64 value ) { }
    u64 count() const { return 0; }
};

// An interface is never a type argument by value.
struct by_value implements sink<Shape>
{
    i32 v;
};

// `Owner` is not `copyable`, which `copy_sink`'s parameter requires.
struct owning implements copy_sink<Owner>
{
    i32 v;

    void take( Owner value ) { }
};

struct holder<T> where T : copyable
{
    T v;
};

// A field's type argument is checked as a parameter's is: `Owner` is not `copyable`.
struct keeps
{
    holder<Owner>* h;
};

struct int_sink implements sink<i64>
{
    i32 v;

    void put( i64 value ) { }
};

void wants_bool<T>( ref T s ) where T : sink<bool> { }

void wants_i32_ptr( ref sink<i32*> s ) { }

struct ptr_sink implements sink<i64*>
{
    i32 v;

    void put( i64* value ) { }
};

i32 main()
{
    int_sink s = int_sink { 0 };

    // `int_sink` is a `sink<i64>`, not a `sink<bool>`.
    wants_bool( ref s );

    // No variance: a `sink<i64*>` is not a `sink<i32*>`.
    ptr_sink p = ptr_sink { 0 };
    wants_i32_ptr( ref p );

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "18:35-18:49 error: `self_loop` cannot require itself",
            "29:32-29:41 error: `loop_b` cannot require `loop_a`, which already requires `loop_b`",
            "48:24-48:28 error: `sink` is generic, so its type arguments must be written = as in `sink<i32>`",
            "54:29-54:39 error: `sink` takes 1 type argument, but 2 were given = `sink` declares `T`",
            "60:39-60:48 error: `repeated` already implements `sink<i64>`",
            "68:35-68:45 error: `half`'s `put` does not match the one `sink<bool>` declares = `sink<bool>` wants `void "
            "put(bool value)`",
            "76:29-76:46 error: `unrefined`'s `put` does not match the one `sink<i32>` declares = `sink<i32>` wants `void "
            "put(i32 value)`",
            "85:33-85:38 error: `Shape` is an interface, so it cannot be held by value = borrow one as `const ref Shape` or "
            "`ref Shape`, or point at one with `Shape*`",
            "91:36-91:41 error: `Owner` is not `copyable`, and `copy_sink` requires it of `T` = it owns something, itself or "
            "through a field, so copying it would own that twice",
            "106:12-106:17 error: `Owner` is not `copyable`, and `holder` requires it of `T` = it owns something, itself or "
            "through a field, so copying it would own that twice",
            "132:17-132:22 error: `wants_bool` needs a `T` that implements `sink<bool>`, but `int_sink` does not = declare "
            "`int_sink` with `implements sink<bool>`",
            "136:20-136:25 error: expected `sink<i32*>`, but got `ptr_sink`"
        }
    );
}

TEST_CASE( "errors_generic_literals", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Which instance a struct literal names. `Box { r }` is a value of *some* `Box<T>`, and only the
// context says which - so each initialiser is checked against that instance's field rather than
// against the declaration's own `T`. The correct code at the top is as much of the test as the
// errors: a line about it appearing in stderr is the failure this fixture exists to catch.
//
// Two structs rather than two numbers, because i32 widens to f64 and an initialiser checked against
// the wrong one of those would still be accepted - which says nothing about whether substitution ran.

struct Red
{
    i32 v;
};

struct Blue
{
    i32 v;
};

// Only ever a type argument, never written as an annotation - so `Box<Green>` exists solely because
// substituting `Outer<Green>`'s field created it, and the name it interned under was decided there.
struct Green
{
    i32 v;
};

struct Box<T> where T : copyable
{
    T v;
};

// Two parameters, so a list that lines up by count can still be wrong by order.
struct Pair<A, B> where A : copyable where B : copyable
{
    A a;
    B b;
};

// A field that is itself a generic instance, so the substitution has to reach inside one rather
// than replace a bare `T`. `Box<T>` becomes `Box<Red>` here or the whole thing is unchecked.
struct Outer<T> where T : copyable
{
    Box<T> inner;
};

struct Plain
{
    i32 v;
};

// The argument position, where the expectation comes from a parameter rather than an annotation.
i32 takes( Box<Red> s )
{
    return s.v.v;
}

// The return position, which reaches check() by a third route again.
Box<Red> make( Red r )
{
    return Box { r };
}

i32 main()
{
    Red  r = Red { 1 };
    Blue b = Blue { 2 };

    // Correct, both forms: positional and named.
    Box<Red> ok    = Box { r };
    Box<Red> named = Box { .v = r };

    Pair<Red, Blue> both = Pair { r, b };

    // The field is a `Red` through this instance, so `b` is what is wrong - not `Box<T>`.
    Box<Red> wrong_field = Box { b };

    // ...and the same through the named form, which finds its field by a different route.
    Box<Red> wrong_named = Box { .v = b };

    // Count is right and order is not, which only a per-field substitution can tell apart.
    Pair<Red, Blue> swapped = Pair { b, r };

    // Correct: the field is a `Box<Red>` through this instance, not the declared `Box<T>`.
    Outer<Red> nested = Outer { ok };

    // ...and the same substitution is what makes this wrong.
    Box<Blue>  blue  = Box { b };
    Outer<Red> mixed = Outer { blue };

    // The field type here is reached only by substitution, so its rendering is this line's to check.
    Outer<Green> unspelled = Outer { r };

    // A different struct is a different type however it is expected to be read.
    Box<Red> foreign = Plain { 3 };

    // The mirror: the expectation belongs to another declaration, so it names no instance - but it
    // is still what the reader should be told about, and the open form must not reach the message.
    Plain mirror = Box { r };

    // Nothing here says which `Box`, so there is no instance for the literal to name.
    auto loose = Box { r };

    return takes( Box { r } ) + make( r ).v.v + named.v.v + both.a.v + nested.inner.v.v;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "74:34-74:35 error: expected `Red`, but got `Blue`",
            "77:39-77:40 error: expected `Red`, but got `Blue`",
            "80:38-80:39 error: expected `Red`, but got `Blue`",
            "80:41-80:42 error: expected `Blue`, but got `Red`",
            "87:32-87:36 error: expected `Box<Red>`, but got `Box<Blue>`",
            "90:38-90:39 error: expected `Box<Green>`, but got `Red`",
            "93:24-93:35 error: expected `Box<Red>`, but got `Plain`",
            "97:20-97:23 error: expected `Plain`, but got `Box`",
            "100:18-100:21 error: nothing here says which `Box` this is = write the type where the value lands, as in "
            "`Box<i32> x = ...`"
        }
    );
}

TEST_CASE( "errors_generic_methods", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A method's own type parameters: what a call writes or leaves to deduction, and the shapes that
// cannot have them. Each mistake is reported once, at the call or member that made it.
class Box<T> where T : copyable
{
    T value;

    Box( T v ) { value = v; }

    T get() const { return value; }

    U map<U>( fn( const ref T ) -> U f ) const { return f( value ); }

    U pick<U>( U a ) const where U : integral { return a; }

    U make<U>() const where U : numeric { return cast<U>( 0 ); }

    // Overloading one would mean binding `U` before the arguments chose between the two.
    V convert<V>( V v ) const where V : copyable { return v; }
    i32 convert( i32 v ) const { return v; }

    // An operator call writes no type arguments.
    bool operator==<U>( const ref Box<T> other ) const { return true; }

    // The aggregate's `T` is already in scope.
    V over<T, V>( V v ) const where V : copyable { return v; }

    // A clause names the method's own parameters, not the aggregate's.
    U bounded<U>( U u ) const where U : copyable, where T : numeric { return u; }

    // A clause with no parameters of its own to constrain.
    T plain() const where T : numeric { return value; }
};

T mk<T>() where T : numeric { return cast<T>( 0 ); }

i64 wide( const ref i64 x ) { return x; }

i32 main()
{
    Box<i32> b = Box<i32>( 7 );

    // Type arguments on a method that declares none.
    i32 g = b.get<i64>();

    // Written and deduced, a signature that is not the one the receiver needs.
    i64 w = b.map<i64>( &wide );
    i64 d = b.map( &wide );

    // A broken bound, written and deduced.
    f64 f = b.pick<f64>( 1.0 );
    f64 e = b.pick( 1.0 );

    // The wrong count.
    i32 c = b.pick<i32, i32>( 1 );

    // Nothing to deduce `U` from: no argument, and no expectation to read.
    b.make();

    // `mk` is not told what `.get()` is wanted for.
    i32 n = mk().get();

    // No one function to take the address of.
    auto a = &Box<i32>::map;

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "19:9-19:16 error: `convert` has type parameters of its own, so it cannot be overloaded = previous declaration is "
            "at: t.kl:18:5",
            "22:20-22:23 error: an operator cannot have type parameters of its own = it is called without any, so nothing "
            "could say what they are",
            "25:12-25:13 error: `T` is already declared in this scope = previous declaration is at: t.kl:3:11",
            "28:51-28:68 error: `T` is `Box`'s parameter, so its bounds go on `Box` = write the clause after `Box`'s "
            "parameters, or make this method a free generic function",
            "31:21-31:38 error: `T` is `Box`'s parameter, so its bounds go on `Box` = write the clause after `Box`'s "
            "parameters, or make this method a free generic function",
            "43:13-43:18 error: `get` is not a generic method",
            "46:25-46:30 error: expected `fn(const ref i32) -> i64`, but got `fn(const ref i64) -> i64` = one signature is "
            "never widened into another, so the two have to match exactly",
            "47:20-47:25 error: expected `fn(const ref i32) -> i64`, but got `fn(const ref i64) -> i64` = one signature is "
            "never widened into another, so the two have to match exactly",
            "50:20-50:23 error: `f64` is not `integral`, and `pick` requires it of `U` = it has to be an integer",
            "51:13-51:19 error: `f64` is not `integral`, and `pick` requires it of `U` = it has to be an integer",
            "54:19-54:29 error: `pick` takes 1 type argument, but 2 were given = `pick` declares `U`",
            "57:5-57:11 error: nothing here says what `U` is in `make` = write the type arguments, as in `make<i32>(...)`",
            "60:13-60:15 error: nothing here says what `T` is in `mk` = write the type arguments, as in `mk<i32>(...)`",
            "63:14-63:28 error: `map` has type parameters of its own, so its address names no one function = wrap the call you "
            "want in a function, and take that function's address"
        }
    );
}

TEST_CASE( "errors_generic_owning", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A generic class with a destructor owns at every instance, so each rule about owning types
// applies to its instances exactly as to a concrete class: no `copyable` bound admits one, and no
// struct may contain one - directly or through a class that holds one.

class Box<T>
{
    public u64 n;

    Box( u64 m )
    {
        n = m;
    }

    ~Box()
    {
    }
};

class Holder<T>
{
    public Box<T> inner;
};

T twice<T>( T a ) where T : copyable
{
    T b = a;
    return b;
}

struct Direct
{
    Box<u64> b;
};

struct Through
{
    Holder<u64> h;
};

u64 copies()
{
    Box<u64> c = twice<Box<u64>>( Box<u64>( 3 ) );
    return c.n;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "32:5-32:16 error: a struct cannot contain `Box<u64>`, which has a destructor = a struct is copied freely, so "
            "declare `Direct` as a class if it owns this",
            "37:5-37:19 error: a struct cannot contain `Holder<u64>`, which owns a resource = a struct is copied freely, so "
            "declare `Through` as a class if it owns this",
            "42:24-42:33 error: `Box<u64>` is not `copyable`, and `twice` requires it of `T` = it owns something, itself or "
            "through a field, so copying it would own that twice"
        }
    );
}

TEST_CASE( "errors_generic_owning_argument", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A generic class with no destructor owns exactly when its argument does, so each rule about
// owning types applies to `Wrap<Tracked>` and not to `Wrap<i32>`: a copy needs `move`, no
// `copyable` bound admits one, and no struct may contain one. An open `Wrap<T>` may own, so its
// function's address has no one spelling at an instance that would copy.

class Tracked
{
    public i32 n;

    Tracked( i32 m )
    {
        n = m;
    }

    ~Tracked()
    {
    }
};

class Wrap<T>
{
    public T v;

    Wrap( move T x )
    {
        v = move x;
    }
};

T twice<T>( T x ) where T : copyable
{
    return x;
}

i32 open<T>( Wrap<T> w )
{
    return 1;
}

// Owns through its argument.
struct Plain
{
    Wrap<Tracked> w;
};

// May own, depending on `T`.
struct Maybe<T>
{
    Wrap<T> w;
};

i32 main()
{
    Wrap<Tracked> w = Wrap<Tracked>( move Tracked( 1 ) );
    Wrap<Tracked> c = w;

    twice( w );

    // Neither owns, so neither is refused.
    Wrap<i32> i = Wrap<i32>( 2 );
    Wrap<i32> j = i;
    twice( i );

    auto p = &open<i32>;

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "43:5-43:21 error: a struct cannot contain `Wrap<Tracked>`, which owns a resource = a struct is copied freely, so "
            "declare `Plain` as a class if it owns this",
            "49:5-49:15 error: a struct cannot contain `Wrap<T>`, which may own a resource = a struct is copied freely, so "
            "declare `Maybe` as a class if it owns this",
            "55:23-55:24 error: an owning value is transferred, not copied = write `move w`",
            "57:12-57:13 error: `Wrap<Tracked>` is not `copyable`, and `twice` requires it of `T` = it owns something, itself "
            "or through a field, so copying it would own that twice",
            "64:14-64:24 error: `open` takes `Wrap<T>`, which is not known to be copyable, so its address has no function type "
            "yet = take it as `const ref Wrap<T>`, so every instance passes it the same way; the parameter is at t.kl:35:14"
        }
    );
}

TEST_CASE( "errors_generic_run_from_array", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A fixed array deduces a run's `T` from its element, but a `T` an earlier argument bound stays
// bound, so the array is refused naming both.
bool has<T>( T wanted, const ref T[] values ) where T : equatable
{
    for( u64 i : 0..values.size )
    {
        if( values[i] == wanted )
        {
            return true;
        }
    }
    return false;
}

i32 main()
{
    i32[2] numbers = [ 1, 2 ];
    if( has( true, numbers ) )
    {
        return 1;
    }
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines { "18:20-18:27 error: `T` cannot be both `bool` and `i32` = an earlier argument already made it `bool`" }
    );
}

TEST_CASE( "errors_generic_variants", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Which instance a variant path names. `Opt::Some( r )` is a value of *some* `Opt<T>`, and only the
// context says which - so a payload is checked against that instance's field rather than against
// the declaration's own `T`. The correct code at the top is as much of the test as the errors: a
// line about it appearing in stderr is the failure this fixture exists to catch.
//
// Two structs rather than two numbers, because i32 widens to f64 and a payload checked against the
// wrong one of those would still be accepted - which says nothing about whether substitution ran.

struct Red
{
    i32 v;
};

struct Blue
{
    i32 v;
};

// Only ever a type argument, never written as an annotation - so `Opt<Green>` exists solely because
// substituting `Nest<Green>`'s payload created it, and the name it interned under was decided there.
struct Green
{
    i32 v;
};

enum Opt<T> where T : copyable
{
    None,
    Some( T v )
};

// Two parameters, so a payload that lines up by count can still be wrong by order.
enum Pair<A, B> where A : copyable where B : copyable
{
    Both( A a, B b )
};

// A payload that is itself a generic instance, so the substitution has to reach inside one rather
// than replace a bare `T`. `Opt<T>` becomes `Opt<Red>` here or the whole thing is unchecked.
enum Nest<T> where T : copyable
{
    Inner( Opt<T> o )
};

enum Plain
{
    One,
    Two
};

// The pattern side, where the scrutinee names the instance and nothing has to be inferred. Reading
// `v.v` is the assertion: a binding left at the declaration's `T` has no fields to reach.
i32 unwrap( Opt<Red> s )
{
    switch( s )
    {
        case Opt::Some( v ):
            return v.v;
        case Opt::None:
            return 0;
    }
}

i32 main()
{
    Red  r = Red { 1 };
    Blue b = Blue { 2 };

    // Correct, both ways round: a payload-carrying variant and a payload-free one.
    Opt<Red> some = Opt::Some( r );
    Opt<Red> none = Opt::None;

    Pair<Red, Blue> both = Pair::Both( r, b );

    // The payload is a `Red` through this instance, so `b` is what is wrong - not `Opt<T>`.
    Opt<Red> wrong_payload = Opt::Some( b );

    // Count is right and order is not, which only a per-field substitution can tell apart.
    Pair<Red, Blue> swapped = Pair::Both( b, r );

    // Correct: the payload is an `Opt<Red>` through this instance, not the declared `Opt<T>`.
    Nest<Red> nested = Nest::Inner( some );

    // ...and the same substitution is what makes this wrong.
    Opt<Blue> blue    = Opt::Some( b );
    Nest<Red> mixed   = Nest::Inner( blue );

    // The payload type here is reached only by substitution, so its rendering is this line's to check.
    Nest<Green> unspelled = Nest::Inner( r );

    // A different enum is a different type however it is expected to be read.
    Opt<Red> foreign = Plain::One;

    // Nothing here says which `Opt`, so there is no instance for the path to name.
    auto loose = Opt::Some( r );

    return unwrap( some );
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "76:41-76:42 error: expected `Red`, but got `Blue`",
            "79:43-79:44 error: expected `Red`, but got `Blue`",
            "79:46-79:47 error: expected `Blue`, but got `Red`",
            "86:38-86:42 error: expected `Opt<Red>`, but got `Opt<Blue>`",
            "89:42-89:43 error: expected `Opt<Green>`, but got `Red`",
            "92:24-92:34 error: expected `Opt<Red>`, but got `Plain`",
            "95:18-95:27 error: nothing here says which `Opt` this is = write the type where the value lands, as in `Opt<i32> "
            "x = ...`"
        }
    );
}

TEST_CASE( "errors_generics", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A generic's signature types, so the mistakes here are about the type parameters themselves and
// about the arguments a call site names - not about the feature being unfinished.
T id<T>( T a ) where T : copyable
{
    return a;
}

// The parameters are scoped to their own declaration, so this one does not see `id`'s.
T stray( T a )
{
    return a;
}

void duplicated<T, T>( T a )
{
}

T empty<>( i32 a )
{
    return a;
}

T inline_bound<comparable T>( T a )
{
    return a;
}

i32 main()
{
    return 0;
}

// A generic's type arguments are deduced where the call says enough and written where it does
// not. Which is which, and what each failure reads like, is errors_inference.kl.

// And the mirror - a plain function given type arguments. After the call-site scan this is also
// what `a < b > ( c )` parses as, so it is the message that shape produces.
i32 not_a_generic( i32 a )
{
    return a;
}

i32 calls_the_wrong_thing()
{
    return not_a_generic<i32>( 1 );
}

// The argument is checked against the *substituted* parameter, so the diagnostic names `i32`
// rather than `T`, which the caller never wrote.
i32 wrong_argument()
{
    return id<bool>( 1 );
}

i32 too_many_type_arguments()
{
    return id<i32, f64>( 1 );
}

// D40. The bounds themselves: a name that is not one of them, a subject that is not a parameter of
// this declaration, and a clause with nothing to constrain.
void unknown_bound<T>( T a ) where T : Sortable
{
}

void wrong_subject<T>( T a ) where U : copyable
{
}

i32 no_type_parameters( i32 a ) where T : copyable
{
    return a;
}

void twice_over<T>( T a ) where T : copyable, where T : comparable
{
}

// Bounds are joined with `&`, because every one of them has to hold.
void wrong_separator<T>( T a ) where T : copyable | comparable
{
}

// The body is checked once, before any instance exists, so an operation `T` was not promised is
// rejected here rather than inside some expansion.
class Owner
{
    i32 x;

    Owner( i32 n ) { x = n; }

    ~Owner() { }
};

enum Suit { Clubs, Hearts };

bool unpromised_operator<T>( T a ) where T : copyable
{
    return a < a;
}

// Against a bool, because D41 lets a `comparable` `T` meet a concrete *number* - what is left
// is the operand that is not one.
bool mismatched_operands<T>( T a, bool b ) where T : copyable & comparable
{
    return a < b;
}

// And the call site's half: a type argument that does not deliver what the parameter was promised.
void breaks_a_bound()
{
    Owner o = Owner( 1 );
    id<Owner>( o );
}

// One mistake, not four: `integral` was stored with everything it implies, and only the bound the
// author wrote is a thing they can act on.
void needs_integral<T>( const ref T a ) where T : integral
{
}

void orders_an_enum()
{
    needs_integral<Suit>( Suit::Clubs );
}

// A generic forwarding its own `T` is told to widen its clause, not to change its type.
void needs_comparable<T>( const ref T a ) where T : comparable
{
}

void forwards<T>( const ref T a ) where T : equatable
{
    needs_comparable<T>( a );
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "9:1-9:2 error: unknown type `T`",
            "9:10-9:11 error: unknown type `T`",
            "14:20-14:21 error: `T` is already declared in this scope = previous declaration is at: t.kl:14:17",
            "18:1-18:2 error: unknown type `T`",
            "18:9-18:10 error: a generic needs at least one type parameter",
            "23:16-23:26 error: a type parameter is a name on its own here = write the bound in a `where` clause after the "
            "parameter list",
            "25:12-25:13 error: cannot return a borrowed value = the caller still owns it",
            "45:12-45:25 error: `not_a_generic` is not a generic",
            "52:12-52:25 error: expected `i32`, but got `bool`",
            "52:22-52:23 error: expected `bool`, but got an integer literal",
            "57:14-57:24 error: `id` takes 1 type argument, but 2 were given = `id` declares `T`",
            "62:40-62:48 error: unknown bound `Sortable` = a bound is an interface, or one of copyable, numeric, integral, "
            "floating and defaultable",
            "66:30-66:48 error: `U` is not a type parameter of `wrong_subject`",
            "70:33-70:51 error: a `where` clause needs type parameters = write them on the name, as in `T f<T>(T a) where T : "
            "copyable`",
            "75:47-75:67 error: duplicate where clause for type parameter `T`",
            "80:51-80:52 error: bounds are joined with `&` = every one of them has to hold, and `&` says so",
            "99:12-99:17 error: no operator `<` for `T` and `T` = `<` needs `comparable`; write `where T : comparable` on "
            "`unpromised_operator`",
            "106:12-106:17 error: no operator `<` for `T` and `bool` = `<` needs both operands to be the same type",
            "113:8-113:13 error: `Owner` is not `copyable`, and `id` requires it of `T` = it owns something, itself or through "
            "a field, so copying it would own that twice",
            "124:20-124:24 error: `Suit` is not `integral`, and `needs_integral` requires it of `T` = it has to be an integer",
            "134:22-134:23 error: `T` does not promise `comparable`, and `needs_comparable` requires it of `T` = add "
            "`comparable` to the `where` clause for `T` on `forwards`"
        }
    );
}

TEST_CASE( "errors_global_order", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A global is built in declaration order, so its initialiser may read only those declared before it.
i32 twice( i32 n )
{
    return n * 2;
}

i32 a = twice( b );
i32 b = twice( 3 );
i32 c = twice( c );

struct Counts
{
    i32 n;

    static i32 first = twice( Counts::second );
    static i32 second = twice( 4 );
};

i32 main()
{
    return a + b + c + Counts::first;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "7:16-7:17 error: `b` is built after `a` = a global may read only those declared before it",
            "9:16-9:17 error: `c` is read while it is being built = a global may read only those declared before it",
            "15:31-15:45 error: `second` is built after `first` = a global may read only those declared before it"
        }
    );
}

TEST_CASE( "errors_growing_instances", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A destructor or a field default runs for every instance of its aggregate, so one that names a
// bigger instance makes a new instance each time round.

i32 count<T>() { return 1; }

class Local<T>
{
    i32 n;
    Local( i32 k ) { n = k; }
    ~Local() { Local<Local<T>>* p = nullptr; n = 0; }
};

class Called<T>
{
    i32 n;
    Called( i32 k ) { n = k; }
    ~Called() { n = count<Called<Called<T>>>(); }
};

void helper<T>() { Through<Through<T>>* p = nullptr; }

class Through<T>
{
    i32 n;
    Through( i32 k ) { n = k; }
    ~Through() { helper<T>(); }
};

class Defaulted<T>
{
    i32 n = count<Defaulted<Defaulted<T>>>();
};

// An ordinary method is lowered only where it is called, so this one is finite.
class Method<T>
{
    i32 n;
    Method( i32 k ) { n = k; }
    public i32 get() const { return count<Method<Method<T>>>(); }
};

i32 main()
{
    Method<i32> m = Method<i32>( 1 );
    return m.get() - 1;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "10:16-10:45 error: `Local` is instantiated with a bigger type argument each time round = `Local` to `~Local` to "
            "`Local`, and each round builds a type out of the last - so no set of instances ever finishes; forward the "
            "parameter itself, or take the built type as a second parameter",
            "17:21-17:47 error: `Called` is instantiated with a bigger type argument each time round = `Called` to `~Called` "
            "to `Called`, and each round builds a type out of the last - so no set of instances ever finishes; forward the "
            "parameter itself, or take the built type as a second parameter",
            "20:20-20:53 error: `Through` is instantiated with a bigger type argument each time round = `Through` to "
            "`~Through` to `helper` to `Through`, and each round builds a type out of the last - so no set of instances ever "
            "finishes; forward the parameter itself, or take the built type as a second parameter",
            "31:13-31:45 error: `Defaulted` is instantiated with a bigger type argument each time round = `Defaulted` to "
            "`Defaulted`, and each round builds a type out of the last - so no set of instances ever finishes; forward the "
            "parameter itself, or take the built type as a second parameter"
        }
    );
}

TEST_CASE( "errors_hash", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D40: the compiler's own hash is no method, so `hash()` is reached only through a `hashable`
// bound. An enum whose own `equatable` block replaces the compiler's loses the compiler's `hashable`
// with it, and a bound needing one names the block. `index` stands in for `kl::map`.
enum Suit
{
    Hearts,
    Spades,
};

enum Colour
{
    Red,
    Crimson,
};

implements equatable for Colour
{
    bool operator==( const ref Colour other ) const
    {
        return cast<u8>( this ) == cast<u8>( other );
    }
};

struct index<K> where K : hashable & copyable
{
    K key;
};

void fine( index<Suit> i )
{
}

void refused( index<Colour> i )
{
}

i32 main()
{
    i32  x = 1;
    Suit s = Suit::Hearts;

    x.hash();
    s.hash();

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "33:21-33:27 error: `index` needs a `K` that implements `hashable`, but `Colour` does not = `Colour`'s own "
            "`equatable` at t.kl:16:12 drops the compiler's `equatable` and `hashable`; write `implements hashable for Colour` "
            "too",
            "42:5-42:6 error: `i32` has no methods = `hash` is reached only as a `hashable`: through a `where T : hashable` "
            "parameter",
            "43:5-43:6 error: `Suit` has no methods = `hash` is reached only as a `hashable`: through a `where T : hashable` "
            "parameter"
        }
    );
}

TEST_CASE( "errors_implements_for", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// M9.5: a type declared elsewhere conforms in a block. Each mistake is reported once, where it was
// made; the package rules, and a block's reach into another package's type, are in
// sema/implements_for_packages.
interface Hashable
{
    u64 hash() const;
};

struct Pair<T> where T : copyable
{
    T a;
};

// A block holds only the interface's methods.
implements Hashable for u32
{
    u64 hash() const { return 1; }
    u64 rehash() const { return 2; }
};

// What it leaves out is missing, and the help says where it goes.
implements Hashable for i64
{
};

// A type conforms once.
implements Hashable for bool
{
    u64 hash() const { return 3; }
};

implements Hashable for bool
{
    u64 hash() const { return 4; }
};

// A struct or class of this package says what it implements where it is declared.
struct Q
{
    i32 v;
};

implements Hashable for Q
{
    u64 hash() const { return 5; }
};

// Only a built-in type, or a type with no type parameters.
implements Hashable for i32*
{
    u64 hash() const { return 6; }
};

implements Hashable for Pair<i32>
{
    u64 hash() const { return 7; }
};

implements Hashable for Hashable
{
    u64 hash() const { return 8; }
};

// The type is written by its name, as in any other type's members.
implements Hashable for u8
{
    u64 hash() const
    {
        self copy = this;
        return 9;
    }
};

// What is implemented must be an interface.
implements i32 for u16
{
};

// The block's methods are reached only as the interface.
enum Colour
{
    Red,
};

implements Hashable for Colour
{
    u64 hash() const { return 10; }
};

implements Hashable for i32
{
    u64 hash() const { return 11; }
};

i32 main()
{
    Colour c = Colour::Red;
    i32    x = 1;
    c.hash();
    x.hash();
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "18:9-18:15 error: `Hashable` has no method `rehash` = a block holds only the methods of the interface it "
            "implements",
            "22:12-22:20 error: `i64` implements `Hashable` but has no method `hash` = add `u64 hash() const` to the block",
            "32:12-32:20 error: `bool` already implements `Hashable` = the block at t.kl:27:25 implements it first",
            "43:25-43:26 error: `Q` is declared in this package, so a block cannot be written for it = write `implements "
            "Hashable` where `Q` is declared",
            "59:25-59:33 error: `Hashable` cannot implement an interface through a block = a block is for a built-in type, a "
            "`struct`, `class` or `enum`, an array, or a pointer to one",
            "69:9-69:13 error: `self` is written only inside an `interface` = write `u8`, the type's own name",
            "75:12-75:15 error: `i32` is not an interface = only an interface can be implemented",
            "99:5-99:6 error: `Colour` has no methods = `hash` is reached only as a `Hashable`: through a `where T : Hashable` "
            "parameter, or a `Hashable*`",
            "100:5-100:6 error: `i32` has no methods = `hash` is reached only as a `Hashable`: through a `where T : Hashable` "
            "parameter, or a `Hashable*`"
        }
    );
}

TEST_CASE( "errors_implicit_constructor", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Declaring any constructor removes the implicit `C()`.
class Made
{
    i32 n = 1;

    Made( i32 v )
    {
        n = v;
    }

    public i32 get() const
    {
        return n;
    }
};

i32 main()
{
    Made m = Made();
    return m.get();
}
)kl",
        Stage::Reported
    );

    REQUIRE( p.shown() == Lines { "19:18-19:20 error: `Made` takes 1 argument, but 0 were given" } );
}

TEST_CASE( "errors_inference", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN §6.7. A call's type arguments are deduced from the argument types and, for whatever they
// leave open, from the expectation at the call site. These are the calls where that is not enough,
// and each one has its own message: nothing said what a parameter is, or two things said different.

struct Box<T> where T : copyable
{
    T v;
};

T id<T>( T a ) where T : copyable
{
    return a;
}

T zeroed<T>() where T : integral
{
    return wrap<T>( 0 );
}

T both<T>( T a, T b ) where T : copyable
{
    return a;
}

T unbox<T>( Box<T> b ) where T : copyable
{
    return b.v;
}

U second<T, U>( T a ) where T : copyable, where U : integral
{
    return wrap<U>( 0 );
}

i32 ordered<T>( T a ) where T : comparable
{
    return 1;
}

struct Plain
{
    i32 v;
};

// A literal contributes nothing - its own type is what is being deduced - and in statement
// position nothing expects anything either, so there is nothing left to read.
void nothing_says_what_t_is()
{
    id( 1 );
}

// The same, where the parameter appears only in the result: there is no argument to read at all.
void only_in_the_result()
{
    zeroed();
}

// Two arguments that cannot agree. The message names the disagreement rather than reporting no
// match, which would be true and useless.
i32 arguments_disagree( i32 a, bool b )
{
    return both( a, b );
}

// And the same disagreement one level down, because deduction is structural.
i32 arguments_disagree_inside( Box<i32> a, Box<bool> b )
{
    both( a, b );

    return 0;
}

// All or nothing: one parameter is settled by the argument and the other by nobody. A partial list
// is a rule nobody can recite, so the whole call goes back to the explicit form.
void one_of_two()
{
    i32 a = 1;

    second( a );
}

// The argument does not have the parameter's shape, so it deduces nothing - and a `T` nothing
// deduced is the same failure as a `T` nothing mentioned.
i32 wrong_shape( i32 a )
{
    return unbox( a );
}

// A bound is a promise about the type argument however it was arrived at, and the argument that
// chose the type is what the message underlines.
i32 deduced_breaks_a_bound( Plain p )
{
    return ordered( p );
}

// Two generics that both match: one takes the whole `Box<i32>` as `T`, the other takes what is
// inside it. Neither is more specific - ranking how well each fits is the thing this refuses - so
// only the type arguments can say which was meant.
bool which<T>( T a ) where T : copyable
{
    return true;
}

bool which<T>( Box<T> a ) where T : copyable
{
    return false;
}

bool two_generics_match( Box<i32> b )
{
    return which( b );
}

// A struct literal without type arguments takes its instance from the expectation, and here the
// expectation is what is being deduced - so it says nothing, and the help asks for `Box<i32> { 1 }`.
void a_struct_literal_says_nothing()
{
    unbox( Box { 1 } );
}

struct Pair<T, U> where T : copyable, where U : copyable
{
    T a;
    U b;
};

T twice<T, U>( Pair<T, U> x, Pair<T, U> y ) where T : copyable, where U : copyable
{
    return x.a;
}

// Two parameters disagreeing at once. Both are true, and only one is reported - so which one has
// to be the first as declared, or the message would depend on how a map happened to hash.
i32 two_disagreements( Pair<i32, bool> one, Pair<bool, i32> two )
{
    twice( one, two );

    return 0;
}

// An argument that selection already typed is not typed again. `ext` needs an `unsafe` block, and
// saying so twice would be one mistake told twice.
extern i32 ext( i32 a );

T chosen<T>( T a, i32 b ) where T : copyable
{
    return a;
}

T chosen<T>( Box<T> a, i32 b ) where T : copyable
{
    return a.v;
}

i32 typed_once()
{
    return chosen( ext( 1 ), 2 );
}

// The arguments decide and the expectation only fills what they left, so this is an ordinary
// mismatch about the result rather than a second deduction that disagrees.
bool expectation_does_not_override( i32 a )
{
    return id( a );
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "49:5-49:7 error: nothing here says what `T` is in `id` = write the type arguments, as in `id<i32>(...)`",
            "55:5-55:11 error: nothing here says what `T` is in `zeroed` = write the type arguments, as in `zeroed<i32>(...)`",
            "62:21-62:22 error: `T` cannot be both `i32` and `bool` = an earlier argument already made it `i32`",
            "68:14-68:15 error: `T` cannot be both `Box<i32>` and `Box<bool>` = an earlier argument already made it `Box<i32>`",
            "79:5-79:11 error: nothing here says what `U` is in `second` = write the type arguments, as in `second<i32, "
            "i32>(...)`",
            "86:19-86:20 error: expected `Box<i32>`, but got `i32`",
            "93:21-93:22 error: `ordered` needs a `T` that implements `comparable`, but `Plain` does not = declare `Plain` "
            "with `implements equatable, comparable`",
            "111:12-111:22 error: this call to `which` is ambiguous = more than one matches: `(T)` and `(Box<T>)`; write the "
            "type arguments, as in `which<i32>(...)`",
            "118:5-118:10 error: nothing here says what `T` is in `unbox` = write the literal's type arguments, as in "
            "`Box<i32> { ... }`",
            "136:17-136:20 error: `T` cannot be both `i32` and `bool` = an earlier argument already made it `i32`",
            "157:20-157:23 error: calling `ext` needs an `unsafe` block = it is defined in C, so the compiler cannot check "
            "what it does with its arguments",
            "164:12-164:19 error: expected `bool`, but got `i32`"
        }
    );
}

TEST_CASE( "errors_interface_members", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// An interface holds method signatures only, and every one is public.
interface With_ctor { With_ctor(); };
interface With_dtor { ~With_dtor(); };
interface With_body_ctor { With_body_ctor() { } i32 f() const; };
interface With_public { public i32 f() const; };
interface With_private { private i32 f() const; };
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "2:23-2:35 error: an `interface` holds only method signatures",
            "3:23-3:36 error: an `interface` holds only method signatures",
            "4:28-4:48 error: an `interface` holds only method signatures",
            "5:25-5:31 error: an interface's methods are public = remove it",
            "6:26-6:33 error: an interface's methods are public = remove it"
        }
    );
}

TEST_CASE( "errors_interfaces", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// M9.5: what an interface refuses. Its methods are signatures, never generic; an
// implementer answers each with a public method of the same signature, `const` where the
// interface's is; an interface is never held by value; and a bound is met only by a declared
// `implements`. Each mistake is reported once, where it was made.
interface Shape
{
    f64 area() const;
    void grow( f64 by );
    i32 make<U>( U u );
};

interface Named
{
    i32 id() const;
};

struct Plain
{
    i32 n;
};

struct Dot implements Shape, Named, Shape
{
    i32 n;

    f64 area() { return 0.0; }
    void grow( f32 by ) { }
};

class Hidden implements Named
{
    i32 n;

    private i32 id() const { return n; }
};

struct Wrong implements Plain
{
    i32 n;
};

Shape global_shape;

void by_value( Shape s ) { }

f64 unknown<T>( const ref T t ) where T : Shape { return t.perimeter(); }

f64 unbounded<T>( const ref T t ) { return t.area(); }

f64 bounded<T>( const ref T t ) where T : Shape { return t.area(); }

void read_only( const ref Shape s ) { s.grow( 1.0 ); }

i32 main()
{
    Plain p = Plain { 1 };
    bounded( p );
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "9:13-9:16 error: an interface's method cannot have type parameters = a call through the interface reaches one "
            "compiled method, which cannot be every instance",
            "22:23-22:28 error: `Dot`'s `area` must be `const`, as `Shape` declares it = a caller holding a `const ref Shape` "
            "may call it",
            "22:23-22:28 error: `Dot`'s `grow` does not match the one `Shape` declares = `Shape` wants `void grow(f64 by)`",
            "22:30-22:35 error: `Dot` implements `Named` but has no method `id` = add `i32 id() const` to `Dot`",
            "22:37-22:42 error: `Dot` already implements `Shape`",
            "30:25-30:30 error: `Hidden`'s `id` is private, but `Named` makes it callable by anyone = make `id` public",
            "37:25-37:30 error: `Plain` is not an interface = only an interface can be implemented",
            "42:1-42:6 error: `Shape` is an interface, so it cannot be held by value = borrow one as `const ref Shape` or `ref "
            "Shape`, or point at one with `Shape*`",
            "44:16-44:21 error: `Shape` is an interface, so it cannot be held by value = borrow one as `const ref Shape` or "
            "`ref Shape`, or point at one with `Shape*`",
            "46:58-46:69 error: `T` has no method `perimeter`",
            "48:44-48:45 error: `T` has no methods = a `where` clause naming an interface gives `T` that interface's methods",
            "52:39-52:40 error: `s` is `const` = remove `const` to modify it",
            "57:14-57:15 error: `bounded` needs a `T` that implements `Shape`, but `Plain` does not = declare `Plain` with "
            "`implements Shape`"
        }
    );
}

TEST_CASE( "errors_intrinsic", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Each refusal of `@intrinsic`, one error apiece.

@intrinsic
f64 own_root( f64 x );

@intrinsic
f64 with_body( f64 x ) { return x; }

@intrinsic
i32 counted = 1;

i32 main() { return 0; }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "4:1-4:23 error: only the prelude's square roots are `@intrinsic` = write the body, or declare it `extern`",
            "6:1-6:11 error: `@intrinsic` marks a function with no body = the compiler gives its body",
            "9:1-9:11 error: `@intrinsic` marks a function with no body = the compiler gives its body"
        }
    );
}

TEST_CASE( "errors_joined_text", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// What `str + textual` gives, and where its `joined_text` is refused. A line ending `// error`
// reports exactly one error; every other line reports none.
class Name implements textual
{
    str text;

    public Name() { text = "nm"; }

    public const ref u8[] as_text() const { return text.as_text(); }
};

class Label implements from<str>
{
    u64 n;

    public Label( u64 n ) { this.n = n; }
    public static Label from( move str text ) { return Label( text.size ); }
};

class Owned implements from<joined_text>
{
    u64 n;

    public Owned( u64 n ) { this.n = n; }
    public static Owned from( move joined_text text ) { return Owned( text.size() ); }
};

i32 main()
{
    Name name = Name();
    u64  count = 3;

    auto joined = "a " + name;
    auto more = joined + "b";
    auto none = "a " + count; // error
    i32 wrong = "a " + name; // error
    Label label = "a " + name; // error
    joined.push( 1 ); // error
    Owned bare = joined; // error
    Owned moved = move more;
    Owned built = "c " + name;

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "35:24-35:29 error: expected `textual`, but got `u64`",
            "36:17-36:28 error: expected `i32`, but got `joined_text`",
            "37:19-37:30 error: expected `Label`, but got `joined_text`",
            "38:5-38:16 error: `joined_text` has no method `push`",
            "39:18-39:24 error: an owning value is transferred, not copied = write `move joined`"
        }
    );
}

TEST_CASE( "errors_keyword_members", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A keyword where a member name belongs. Each of these aborted the compiler until the parser was
// taught that an access with no name is not an access - the same rule a declaration with no name
// already followed.
struct Point { i32 x; };

enum Colour { Red, Green };

i32 main()
{
    Point p = Point { 1 };
    Point* q = &p;

    i32 a = p.this;

    q.if = p;

    Colour c = Colour::this;

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "13:15-13:19 error: expected an identifier, found `this` = `this` is a keyword, so it cannot be used as a name",
            "15:7-15:9 error: expected an identifier, found `if` = `if` is a keyword, so it cannot be used as a name",
            "17:24-17:28 error: expected an identifier, found `this` = `this` is a keyword, so it cannot be used as a name"
        }
    );
}

TEST_CASE( "errors_lambda_captures", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// What a lambda may reach: only what it captures, each capture once, a bare one read-only.
void run<F>( F f ) where F : fn() -> void { f(); }

i32 g = 0;

struct P
{
    i32 v;

    void bump() { v++; }

    void member() { run( fn[]() { v = 1; } ); }
    void through_this() { run( fn[]() { this.v = 1; } ); }
    void field_capture() { run( fn[ v ]() { } ); }
    void read_only_this() { run( fn[ this ]() { v = 1; } ); }
    void read_only_call() { run( fn[ this ]() { bump(); } ); }
    i32 peek() const { run( fn[ ref this ]() { } ); return v; }
};

void uncaptured() { i32 x = 0; run( fn[]() { x += 1; } ); }
void uncaptured_param( i32 x ) { run( fn[]() { x += 1; } ); }
void nested() { i32 x = 0; run( fn[ x ]() { run( fn[]() { i32 y = x; } ); } ); }
void global() { run( fn[ g ]() { } ); }
void twice() { i32 x = 0; run( fn[ x, ref x ]() { } ); }
void shadowed() { i32 x = 0; run( fn[ x ]() { i32 x = 1; } ); }
void read_only() { i32 x = 0; run( fn[ x ]() { x = 2; } ); }
void moved() { i32 x = 0; run( fn[ x ]() { i32 z = move x; } ); }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "12:35-12:36 error: `v` is a member, and this lambda does not capture `this` = capture it: `fn[this]` reads "
            "through it, `fn[ref this]` writes through it",
            "13:41-13:45 error: `this` is not captured by this lambda = capture it: `fn[this]` reads through it, `fn[ref "
            "this]` writes through it",
            "14:37-14:38 error: `v` is a member, so a lambda reaches it through `this` = capture `this`: `fn[this]` reads it, "
            "`fn[ref this]` writes it",
            "15:49-15:50 error: `this` is captured read-only, so its object cannot be modified = capture it as `ref this`",
            "16:49-16:53 error: `this` is captured read-only, so `bump`, which may modify the object, cannot be called = "
            "capture it as `ref this`",
            "17:37-17:41 error: a `const` method cannot modify its object = remove `const` from `peek` so it can modify its "
            "object",
            "20:46-20:47 error: `x` is not captured by this lambda = capture it: `fn[x]` reads it, `fn[ref x]` writes it",
            "21:48-21:49 error: `x` is not captured by this lambda = capture it: `fn[x]` reads it, `fn[ref x]` writes it",
            "22:67-22:68 error: `x` is not captured by this lambda = capture it: `fn[x]` reads it, `fn[ref x]` writes it",
            "23:26-23:27 error: `g` is not a local, so it cannot be captured = a lambda names globals, functions and types "
            "without capturing them",
            "24:43-24:44 error: `x` is already declared in this scope = previous declaration is at: t.kl:24:36",
            "25:51-25:52 error: `x` shadows an outer declaration = previous declaration is at: t.kl:25:39",
            "26:48-26:49 error: `x` is captured read-only, so it cannot be modified = capture it as `ref x`",
            "27:57-27:58 error: a capture cannot be moved = `x` belongs to where the lambda is written; a lambda only copies "
            "or borrows it"
        }
    );
}

TEST_CASE( "errors_lambda_positions", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A lambda that captures is passed straight to a call; an `fn` value holds no captures.
i32 apply( fn() -> i32 f ) { return f(); }

struct Holder { fn() -> i32 f; };

void held() { i32 k = 1; auto a = fn[ k ]() -> i32 { return k; }; }
void typed() { i32 k = 1; fn() -> i32 e = fn[ k ]() -> i32 { return k; }; }
i32 passed() { i32 k = 1; return apply( fn[ k ]() -> i32 { return k; } ); }
void in_field() { i32 k = 1; Holder h = Holder { fn[ k ]() -> i32 { return k; } }; }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "6:35-6:65 error: a lambda that captures can only be passed straight to a call = pass it as an argument, to a "
            "parameter whose type is bound by an `fn` type",
            "7:43-7:73 error: a lambda that captures can only be passed straight to a call = an `fn() -> i32` value holds no "
            "captures; take a type parameter bound by it",
            "8:41-8:71 error: a lambda that captures can only be passed straight to a call = an `fn() -> i32` value holds no "
            "captures; take a type parameter bound by it",
            "9:50-9:80 error: a lambda that captures can only be passed straight to a call = an `fn() -> i32` value holds no "
            "captures; take a type parameter bound by it"
        }
    );
}

TEST_CASE( "errors_lambda_types", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A lambda's types come from what it says or the `fn` type expected of it.
void untyped() { auto b = fn[]( x ) -> i32 { return x; }; }
void count() { fn( i32 ) -> i32 d = fn[]( i32 p, i32 q ) -> i32 { return p; }; }
void parameter() { fn( i32 ) -> i32 f = fn[]( i64 p ) -> i32 { return 0; }; }
void unsaid() { auto c = fn[]() { return 3; }; }
void nothing() { fn() -> void v = fn[]() { return 3; }; }
void result() { fn() -> i32 r = fn[]() -> bool { return true; }; }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "2:33-2:34 error: `x` has no type = write one; only a lambda passed where an `fn` type is expected may leave it "
            "out",
            "3:41-3:57 error: this lambda takes 2 parameters, but `fn(i32) -> i32` takes 1",
            "4:41-4:75 error: expected `fn(i32) -> i32`, but got `fn(i64) -> i32` = one signature is never widened into "
            "another, so the two have to match exactly",
            "5:42-5:43 error: this lambda returns `void`, so `return` cannot take a value = say what it returns: `-> i32` "
            "after its parameters",
            "6:51-6:52 error: this lambda returns `void`, so `return` cannot take a value",
            "7:33-7:64 error: expected `fn() -> i32`, but got `fn() -> bool` = one signature is never widened into another, so "
            "the two have to match exactly"
        }
    );
}

TEST_CASE( "errors_literal_spans", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A refused literal underlines what is wrong with it: the type's name, or the initialisers at fault.
struct Point
{
    i32 x;
    i32 y;
};

class Counter
{
    i32 v;

    Counter( i32 a )
    {
        v = a;
    }
};

class Hidden
{
    i32 v;
};

struct Box<T> where T : copyable
{
    T v;
};

i32 main()
{
    Counter c = Counter { 1 };
    Hidden  h = Hidden { 1 };
    auto    b = Box { 1 };
    Point   w = Box { 1 };
    auto    n = Nowhere { 1 };
    Point   f = Point { 1 };
    Point   m = Point { .x = 1 };
    Point   t = Point { 1, 2, 3, 4 };
    Point   x = Point { .x = 1, 2 };
    Point   s = Point {
        1,
        2,
        3,
    };
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "30:17-30:24 error: `Counter` has a constructor, so it cannot be built from a literal = write `Counter(...)`",
            "31:17-31:23 error: `Hidden` keeps `v` private, so it cannot be built from a literal = give it a constructor, and "
            "build it by calling that",
            "32:17-32:20 error: nothing here says which `Box` this is = write the type where the value lands, as in `Box<i32> "
            "x = ...`",
            "33:17-33:20 error: expected `Point`, but got `Box`",
            "34:17-34:24 error: `Nowhere` is not declared",
            "35:17-35:22 error: `Point` has 2 fields, but 1 was given",
            "36:17-36:22 error: field `y` is not initialised",
            "37:31-37:35 error: `Point` has 2 fields, but 4 were given",
            "38:33-38:34 error: an initialiser list is either all positional or all named = give every field a name, or none "
            "of them",
            "42:9-42:10 error: `Point` has 2 fields, but 3 were given"
        }
    );
}

TEST_CASE( "errors_literals", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Literal range checking is bidirectional: the target type is what a literal is measured against.
i32 main()
{
    u8  small = 300;
    i32 low = -2147483649;
    u32 negative = -1;
    u64 huge = 18446744073709551616;
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "4:17-4:20 error: `300` does not fit in `u8`",
            "5:15-5:26 error: `-2147483649` does not fit in `i32`",
            "6:20-6:22 error: `-1` does not fit in `u32`",
            "7:16-7:36 error: integer literal is too large = the largest value that fits is 18446744073709551615"
        }
    );
}

TEST_CASE( "errors_located", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Each refusal of `@located` and `@caller()` the checker makes, one error apiece.

interface Shape
{
    @located
    f64 area() const;
};

interface Named
{
    i32 id() const;
};

class Square implements Named
{
    i32 n;
    public Square() { n = 1; }
    @located
    public i32 id() const { return n; }
    @located
    public static i32 make() { return 2; }
};

implements Named for i64
{
    @located
    i32 id() const { return 3; }
};

@located
void traced() { }

@located
void picked( i32 a ) { }
void picked( f64 a ) { }

void plain()
{
    location at = @caller();
}

@located
void wrong_location()
{
    panic( "no", 3 );
}

@located
i32 main()
{
    fn() -> void p = &traced;
    fn() -> i32 q = &Square::make;
    fn( i32 ) -> void r = &picked;
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "6:5-6:22 error: an interface's method cannot be `@located` = a call through the interface has no caller to pass "
            "on",
            "14:25-14:30 error: `Square`'s `id` cannot be `@located`, since it answers `Named` = a call through `Named` passes "
            "no location; move the `@located` work into a method `Named` does not declare",
            "24:12-24:17 error: `i64`'s `id` cannot be `@located`, since it answers `Named` = a call through `Named` passes no "
            "location; move the `@located` work into a method `Named` does not declare",
            "39:19-39:28 error: `@caller()` is only meaningful inside an `@located` function = write `@located` on the line "
            "above `plain`",
            "45:18-45:19 error: expected `location`, but got an integer literal",
            "49:1-55:2 error: `main` cannot be `@located` = nothing in the program calls it, so it has no caller",
            "51:22-51:29 error: `traced` is `@located`, so it has no address = a call through a pointer has no caller to pass "
            "on; call it directly, or wrap it in a function that is not `@located`",
            "52:21-52:34 error: `make` is `@located`, so it has no address = a call through a pointer has no caller to pass "
            "on; call it directly, or wrap it in a function that is not `@located`",
            "53:27-53:34 error: `picked` is `@located`, so it has no address = a call through a pointer has no caller to pass "
            "on; call it directly, or wrap it in a function that is not `@located`"
        }
    );
}

TEST_CASE( "errors_main_arguments_bytes", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Each argument is a `str`, not a run of bytes.
i32 main( const ref u8[] args )
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE( p.shown() == Lines { "2:9-2:32 error: `main` takes no parameters, or `const ref str[] args`" } );
}

TEST_CASE( "errors_main_arguments_own_str", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Only the prelude's `str` qualifies: a program's own, of the same name, does not.
class str
{
    public u64 size;
};

i32 main( const ref str[] args )
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE( p.shown() == Lines { "7:9-7:33 error: `main` takes no parameters, or `const ref str[] args`" } );
}

TEST_CASE( "errors_main_arguments_ref", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// `main` reads its arguments; it cannot be given a `ref` to change them.
i32 main( ref str[] args )
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE( p.shown() == Lines { "2:9-2:27 error: `main` takes no parameters, or `const ref str[] args`" } );
}

TEST_CASE( "errors_main_arguments_two", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// The count travels inside the run; there is no `argc`.
i32 main( const ref str[] args, u64 count )
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE( p.shown() == Lines { "2:9-2:44 error: `main` takes no parameters, or `const ref str[] args`" } );
}

TEST_CASE( "errors_main_arguments_unknown_type", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// An unknown element type is reported once, as unknown, not again as the wrong `main`.
i32 main( const ref strr[] args )
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE( p.shown() == Lines { "2:21-2:25 error: unknown type `strr`" } );
}

TEST_CASE( "errors_main_arguments_value", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// The arguments are borrowed from the runtime, never moved into `main`.
i32 main( str[] args )
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() == Lines { "2:11-2:16 error: `str[]` is a run, so it is reached only through `ref` = borrow it as `const ref "
                             "str[]`, or point at its elements with `str[*]`" }
    );
}

TEST_CASE( "errors_main_signature", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// C's main returns int, so Keel's must return `i32` or the generated C cannot be called; and it
// takes nothing or its arguments. Reported here rather than left to the C compiler.
u8 main( i32 argc )
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "3:1-3:3 error: `main` must return `i32`, but got `u8`",
            "3:8-3:20 error: `main` takes no parameters, or `const ref str[] args`"
        }
    );
}

TEST_CASE( "errors_many_pointers", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Every refusal a many-item pointer brings, one to a function so each is reported on its own. What
// is accepted is in codegen/many_pointers.kl.
struct Point
{
    i32 x;
};

i32 ungated_index( i32[*] p )
{
    return p[ 0 ];
}

i32[*] ungated_offset( i32[*] p )
{
    return p + 1;
}

void ungated_step( i32[*] p )
{
    p++;
}

i32 single_indexed( i32* p )
{
    return p[ 0 ];
}

i32* single_offset( i32* p )
{
    return p + 1;
}

i32 not_a_pointer( i32 v )
{
    return v[ 0 ];
}

i32 no_star( i32[*] p )
{
    return *p;
}

i32 no_dot( Point[*] p )
{
    return p.x;
}

i32[*] pointer_first( i32[*] p )
{
    return 1 + p;
}

i32 bad_index( i32[*] p )
{
    i32 r = 0;

    unsafe
    {
        r = p[ true ];
    }

    return r;
}

i32[*] bad_count()
{
    i32[*] p = nullptr;

    unsafe
    {
        p = alloc<i32>( 1.5 );
    }

    return p;
}

i32* no_implicit( i32[*] p )
{
    i32* q = p;

    return q;
}

bool no_ordering( i32[*] p, i32[*] q )
{
    return p < q;
}

void no_backwards( i32[*] p )
{
    p -= 1;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "10:12-10:18 error: indexing needs an `unsafe` block = nothing checks that the index is inside the allocation",
            "15:12-15:17 error: pointer arithmetic needs an `unsafe` block = nothing checks that the result is inside the "
            "allocation",
            "20:5-20:9 error: pointer arithmetic needs an `unsafe` block = nothing checks that the result is inside the "
            "allocation",
            "25:12-25:18 error: `i32*` points at one `i32`, so it cannot be indexed = a many-item pointer, `i32[*]`, can be",
            "30:12-30:13 error: `i32*` points at one `i32`, so it has no arithmetic = a many-item pointer, `i32[*]`, can be "
            "offset",
            "35:12-35:18 error: `i32` cannot be indexed",
            "40:12-40:14 error: `i32[*]` points at many `i32`, so `*` does not say which = write `p[0]`",
            "45:12-45:15 error: `.` does not reach through a many-item pointer = write `p[0].x`",
            "50:12-50:17 error: the pointer goes on the left of `+` = write `p + 1`",
            "59:16-59:20 error: an index must be an integer, but got `bool`",
            "71:25-71:28 error: `alloc` needs an integer count, but got `f64`",
            "79:14-79:15 error: expected `i32*`, but got `i32[*]`",
            "86:12-86:17 error: no operator `<` for `i32[*]` and `i32[*]`",
            "91:5-91:12 error: no operator `-=` for `i32[*]`"
        }
    );
}

TEST_CASE( "errors_method_as_field", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A method named without a call is not a field: read, assigned or compounded, the error says what
// it is, and a static one is called through its type.

class Cat
{
    i32 lives;
    public Cat() { lives = 9; }
    public i32 count() const { return lives; }
    public void speak() {}
    public static i32 boom() { return 1; }
};

i32 main()
{
    Cat c = Cat();
    c.speak = 1;
    c.speak += 1;
    i32 n = c.count;
    i32 m = c.boom;
    return n + m;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "16:5-16:12 error: `speak` is a method of `Cat`, not a field = a method is only called: `c.speak()`",
            "17:5-17:12 error: `speak` is a method of `Cat`, not a field = a method is only called: `c.speak()`",
            "18:13-18:20 error: `count` is a method of `Cat`, not a field = a method is only called: `c.count()`",
            "19:13-19:19 error: `boom` is a method of `Cat`, not a field = a method is only called: `Cat::boom()`"
        }
    );
}

TEST_CASE( "errors_method_pointers", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Every way a method's address can fail to name one function, in one file. The unit fixtures check
// that each message is produced; this checks the caret and the hint the author sees.
struct C
{
    i32 n;

    static i32 make()
    {
        return 1;
    }

    i32 add( i32 by )
    {
        return n + by;
    }

    i32 pick( i32 a )
    {
        return a;
    }

    i32 pick( f64 a )
    {
        return 2;
    }
};

class Vault
{
    i32 v;

    private i32 hidden()
    {
        return 1;
    }

    private static i32 sealed()
    {
        return 1;
    }
};

struct Box<T> where T : copyable
{
    T v;

    T value() const
    {
        return v;
    }
};

struct Open<T>
{
    T* v;

    i32 keep( T a ) const
    {
        return 1;
    }
};

i32 main()
{
    auto a = &Vault::sealed;
    auto b = &Vault::hidden;
    auto c = &C::nope;
    auto e = &Box::value;
    auto f = &C::pick;
    auto k = &Open<i32>::keep;

    fn( ref C, bool )->i32 g = &C::pick;

    auto h = C::add;

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "65:14-65:28 error: `sealed` is private to `Vault` = only that type's own members may name it",
            "66:14-66:28 error: `hidden` is private to `Vault` = only that type's own members may name it",
            "67:14-67:22 error: `C` has no member `nope`",
            "68:15-68:18 error: `Box` needs its type arguments here = write `Box< ... >::value`",
            "69:14-69:22 error: `pick` is overloaded, so its address names no one function = assigning it to a variable of one "
            "signature will choose between them",
            "70:14-70:30 error: `keep` takes `T`, which is not known to be copyable, so its address has no function type yet = "
            "add `where T : copyable` to `Open`; the parameter of `keep` is at t.kl:57:15",
            "72:32-72:40 error: no overload of `pick` has the type `fn(ref C, bool) -> i32` = its overloads are `fn(ref C, "
            "i32) -> i32` and `fn(ref C, f64) -> i32`",
            "74:14-74:20 error: `add` is a function, not a value = take its address as `&C::add`, or call it as "
            "`value.add(...)`"
        }
    );
}

TEST_CASE( "errors_methods", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN D29/D32. A method without a trailing `const` takes its receiver as `ref T` and may write the
// object, so calling one needs a receiver that may be written - which is check_writable's question,
// asked of the object rather than of an assignment. Every rule that already refuses a write refuses
// this too, each with its own message.
class B
{
    u64 n;

    B( u64 x )
    {
        n = x;
    }

    ~B()
    {
    }

    u64 read() const
    {
        return n;
    }

    void bump()
    {
        n = n + 1;
    }
};

struct P
{
    i32 x;

    // A bare field name is `this.field` written implicitly, so what it is rooted in is the
    // receiver. Without that the field is not a binding, every question answers no, and a `const`
    // method writes its own object in silence.
    void writes_its_object() const
    {
        x = 1;
    }

    i32 wrong_return_type() const
    {
        return true;
    }
};

u64 mutates_a_borrow( B b )
{
    b.bump();

    return 0;
}

i32 mutates_a_const()
{
    const B c = B( 1 );

    c.bump();

    return 0;
}

i32 calls_a_missing_method()
{
    P p = P { 1 };

    return p.nope();
}

i32 calls_a_field()
{
    P p = P { 1 };

    return p.x();
}

i32 calls_on_a_primitive()
{
    i32 n = 1;

    return n.foo();
}

// A constructor shares the type's name and is reached by calling the type; a destructor is never
// called at all. Matching either by name would let this resolve.
i32 calls_a_constructor()
{
    B b = B( 1 );

    b.B( 2 );

    return 0;
}

// A sibling is callable by bare name, and the receiver's mode carries through: the constness
// question is asked of the *enclosing* method's receiver, there being no object to ask it of.
struct Counter
{
    i32 v;

    i32 read() const
    {
        return v;
    }

    void add( i32 b )
    {
        v = v + b;
    }

    i32 calls_a_mutating_sibling() const
    {
        add( 1 );

        return v;
    }
};

// Everything below is accepted, and the fixture would be worth much less without it.

u64 reads_a_borrow( B b )
{
    return b.read();
}

i32 reads_a_const()
{
    const B c = B( 1 );

    return wrap<i32>( c.read() );
}

i32 writes_through_a_mutable_method()
{
    P p = P { 1 };

    return p.x;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "38:9-38:10 error: a `const` method cannot modify its object = remove `const` from `writes_its_object` so it can "
            "modify its object",
            "43:16-43:20 error: expected `i32`, but got `bool`",
            "49:5-49:6 error: `b` is borrowed, so it cannot be modified = take it as `ref B b` to modify it",
            "58:5-58:6 error: `c` is `const` = remove `const` to modify it",
            "67:12-67:18 error: `P` has no method `nope`",
            "74:12-74:15 error: `x` is a field of `P`, not a method; it cannot be called",
            "81:12-81:13 error: `i32` has no methods",
            "90:5-90:8 error: `B` has no method `B`",
            "113:9-113:12 error: a `const` method cannot call `add`, which may modify the object = remove `const` from "
            "`calls_a_mutating_sibling`, or add it to `add`"
        }
    );
}

TEST_CASE( "errors_mixed_comparison", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// What D41 does not make legal. It moves the line for comparison alone, and only for numbers: a
// result still has to land somewhere, and a value still has to be a magnitude before it is ranked.
enum Colour
{
    Red,
    Green
};

// What a comparison against a number needs from a `T` is that it *is* one, and only `numeric`
// says so. `equatable` promises equality and `comparable` promises ordering; both are satisfied by
// exactly the numbers today only because D33's operators are not shipped, and a type that orders
// itself will satisfy `comparable` without comparing against an `i32`.
bool equated<T>( T a, i32 x ) where T : equatable
{
    return a == x;
}

bool ordered<T>( T a, i32 x ) where T : comparable
{
    return a < x;
}

i32 main()
{
    i32   small = 1;
    u32   unsigned_small = 1;
    i64   wide = 1;
    u64   unsigned_wide = 1;
    f64   real = 1.5;
    bool  flag = true;
    i32*  address = &small;
    Colour shade = Colour::Red;

    // Arithmetic keeps D5's rule: these pairs compare, but their sum has no type to be.
    u32 summed = small + unsigned_small;
    i64 scaled = wide * unsigned_wide;
    f64 mixed = wide - real;

    // A literal that does not fit is a warning rather than an error (D41 gives it an answer), so
    // the refusals here are all about what a value *is*, never about the range it lands in.

    // A bool is not a magnitude, whichever side it is on.
    bool counted = flag == small;
    bool inverted = small != flag;
    bool ranked = flag < small;

    // Neither is an address or a variant. D30 leaves no route from either to a number.
    bool located = address == small;
    bool shaded = shade == small;
    bool ordered = address < address;

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "15:12-15:18 error: no operator `==` for `T` and `i32` = `T` must be a number to compare against `i32`; write "
            "`where T : numeric` on `equated`",
            "20:12-20:17 error: no operator `<` for `T` and `i32` = `T` must be a number to compare against `i32`; write "
            "`where T : numeric` on `ordered`",
            "35:18-35:40 error: no operator `+` for `i32` and `u32`",
            "36:18-36:38 error: no operator `*` for `i64` and `u64`",
            "37:17-37:28 error: no operator `-` for `i64` and `f64`",
            "43:20-43:33 error: no operator `==` for `bool` and `i32`",
            "44:21-44:34 error: no operator `!=` for `i32` and `bool`",
            "45:19-45:31 error: no operator `<` for `bool` and `i32`",
            "48:20-48:36 error: no operator `==` for `i32*` and `i32` = only pointers of the same type can be compared",
            "49:19-49:33 error: no operator `==` for `Colour` and `i32` = only values of the same `enum` can be compared",
            "50:20-50:37 error: no operator `<` for `i32*` and `i32*`"
        }
    );
}

TEST_CASE( "errors_move_and_borrow", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// One call cannot take a value and borrow it: the callee would own it and read it through the
// borrow, and could destroy it first.
class Buffer
{
    public u64 size;

    Buffer()
    {
        size = 0;
    }

    ~Buffer()
    {
    }

    public void absorb( move Buffer _ )
    {
    }

    public bool operator==( move Buffer other ) const
    {
        return size == other.size;
    }
};

void move_then_ref( move Buffer _, ref Buffer _ )
{
}

void ref_then_move( ref Buffer _, move Buffer _ )
{
}

void read_then_move( const ref Buffer _, move Buffer _ )
{
}

void sized( u64 _, move Buffer _ )
{
}

i32 main()
{
    Buffer b = Buffer();
    move_then_ref( move b, ref b );

    Buffer c = Buffer();
    ref_then_move( ref c, move c );

    Buffer d = Buffer();
    read_then_move( d, move d );

    Buffer e = Buffer();
    e.absorb( move e );

    Buffer f = Buffer();
    _ = f == move f;

    Buffer g = Buffer();
    ref Buffer r = g;
    ref_then_move( ref r, move g );

    // The size is copied before the call, and `h` is not borrowed.
    Buffer h = Buffer();
    sized( h.size, move h );

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "45:28-45:33 error: `b` is moved into this call, so the call cannot also borrow it = a call cannot take a value "
            "and borrow it at once",
            "48:20-48:25 error: `c` is moved into this call, so the call cannot also borrow it = a call cannot take a value "
            "and borrow it at once",
            "51:21-51:22 error: `d` is moved into this call, so the call cannot also borrow it = a call cannot take a value "
            "and borrow it at once",
            "54:5-54:6 error: `e` is moved into this call, so the call cannot also borrow it = a call cannot take a value and "
            "borrow it at once",
            "57:9-57:20 error: `f` is moved into this call, so the call cannot also borrow it = a call cannot take a value and "
            "borrow it at once",
            "57:14-57:20 warning: `move` is not needed on an operator's operand [-Wneedless-move] = it only reads the value; "
            "drop `move`",
            "61:20-61:25 error: `g` is moved into this call, so the call cannot also borrow it = a call cannot take a value "
            "and borrow it at once"
        }
    );
}

TEST_CASE( "errors_move_unowned", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// `move` takes what the function owns. A borrow, `this`, and a field reached through `this` are
// the caller's, and the caller destroys them too.
class Buffer
{
    public u64 size;

    Buffer()
    {
        size = 0;
    }

    ~Buffer()
    {
    }
};

void take( move Buffer b )
{
}

void from_ref( ref Buffer b )
{
    take( move b );
}

void from_const_ref( const ref Buffer b )
{
    take( move b );
}

void from_binding( move Buffer b )
{
    ref Buffer r = b;
    take( move r );
}

class Holder
{
    public Buffer kept;

    Holder()
    {
        kept = Buffer();
    }

    ~Holder()
    {
    }

    void give()
    {
        take( move kept );
    }

    Buffer give_back()
    {
        return move kept;
    }

    void give_self()
    {
        take_holder( move this );
    }
};

void take_holder( move Holder h )
{
}

void generic_take<T>( move T x )
{
}

void from_generic_ref<T>( ref T x )
{
    generic_take( move x );
}

// Owned, so moved.
void owned( move Buffer b )
{
    take( move b );
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "23:16-23:17 error: cannot move out of a borrow = take it as `move Buffer b` to own it",
            "28:16-28:17 error: cannot move out of a borrow = take it as `move Buffer b` to own it",
            "34:16-34:17 error: cannot move out of a borrow = move the variable it refers to instead",
            "52:20-52:24 error: a field cannot be moved on its own = it belongs to `this`, which a method only borrows",
            "57:21-57:25 error: a field cannot be moved on its own = it belongs to `this`, which a method only borrows",
            "62:27-62:31 error: cannot move out of a borrow = a method borrows its object; move the object where the method is "
            "called instead",
            "76:24-76:25 error: cannot move out of a borrow = take it as `move T x` to own it"
        }
    );
}

TEST_CASE( "errors_moves", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN §8. Move checking is flow-sensitive over the KIR CFG, so what is reported depends on the
// paths that reach a use, not on the order the statements are written.
void sink( i32 _ )
{
}

// The straightforward case: read after the value has gone.
i32 after()
{
    i32 a = 1;
    sink( move a );
    sink( a );
    return 0;
}

// Moving twice is itself a use of something already gone, which is why the report runs before the
// state changes rather than instead of it.
i32 twice()
{
    i32 a = 1;
    sink( move a );
    sink( move a );
    return 0;
}

// Moved on one path only. The compiler is refusing an ambiguity here rather than reporting a
// certainty, and the message has to say which - that distinction is the whole reason the analysis
// carries a fourth state.
i32 maybe( i32 c )
{
    i32 a = 1;

    if ( c == 0 )
    {
        sink( move a );
    }

    sink( a );
    return 0;
}

// Moved on every path is a certainty again, not a maybe.
i32 both( i32 c )
{
    i32 a = 1;

    if ( c == 0 )
    {
        sink( move a );
    }
    else
    {
        sink( move a );
    }

    sink( a );
    return 0;
}

// A back edge carries the move round: the second iteration reads what the first one took.
i32 looped()
{
    i32 a = 1;
    i32 i = 0;

    while ( i < 3 )
    {
        sink( move a );
        i = i + 1;
    }

    return 0;
}

// Nothing below here is an error, and the fixture would be worth much less without them: they are
// what shows the analysis is not simply refusing every program that says `move`.

// The arm that did not move is untouched by the one that did.
i32 other_arm( i32 c )
{
    i32 a = 1;

    if ( c == 0 )
    {
        sink( move a );
    }
    else
    {
        sink( a );
    }

    return 0;
}

// Assigning again brings it back, and clears the move that was recorded against it.
i32 revived()
{
    i32 a = 1;
    sink( move a );
    a = 2;
    sink( a );
    return 0;
}

// The read happens before the write, so a self-referential assignment is legal. Parenthesised
// because a marker takes the whole expression to its boundary.
i32 reads_before_writing()
{
    i32 a = 1;
    a = ( move a ) + 1;
    return a;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "12:11-12:12 error: `a` is used after it was moved = moved at 11:11",
            "22:11-22:17 error: `a` is used after it was moved = moved at 21:11",
            "38:11-38:12 error: `a` may already have been moved = moved at 35:15 on some path to here",
            "56:11-56:12 error: `a` is used after it was moved = moved at 49:15",
            "68:15-68:21 error: `a` may already have been moved = moved here in an earlier turn of the loop",
            "110:9-110:19 warning: `move` is not needed on an operator's operand [-Wneedless-move] = it only reads the value; "
            "drop `move`"
        }
    );
}

TEST_CASE( "errors_moves_named", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// An uninitialised local reported once, and a moved part named. A line ending `// error` reports
// exactly one error; every other line reports none.
class Buf
{
    u64* data;

    Buf( u64 n )
    {
        unsafe
        {
            data = alloc<u64>();
        }

        *data = n;
    }

    ~Buf()
    {
        unsafe
        {
            free( data );
        }
    }

    u64 get() const
    {
        return *data;
    }
};

class Inner
{
    public Buf n;
};

class Outer
{
    public Inner a;
    public Buf b;
};

void sink( move Buf _ ) { }
void consume( move Outer _ ) { }
u64 look( const ref Outer o ) { return o.b.get(); }

void read_three_times()
{
    i32 x;
    i32 y = x + 1; // error
    i32 z = x + 2;
    _ = y + z + x;
}

void whole_after_its_fields( move Outer o )
{
    sink( move o.a.n );
    sink( move o.b );
    consume( move o ); // error
}

void field_under_a_loan( move Outer o )
{
    const ref Outer r = o;
    sink( move o.a.n ); // error
    _ = look( r );
}

i32 main() { return 0; }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "49:13-49:14 error: `x` is used before it is initialised = give it a value at its declaration",
            "58:14-58:20 error: `o` is used after `o.a` was moved = moved at 56:11",
            "64:11-64:21 error: `o.a.n` is moved while `r` still borrows `o` = `r` is used after this; it was bound at 63:5"
        }
    );
}

TEST_CASE( "errors_nameless_enum_parts", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// An enum or a variant with no name is not one, so the parser leaves it out; a payload field with
// no name is still one, and only a duplicate's report reads that name. Each is reported once, by
// the parser, and nothing after it is disturbed - the switch below covers Shape as one variant,
// so the `return` after it is never reached.

enum : u8
{
    Red,
};

enum Shape
{
    Circle( f64 radius ),
    ( f64 side ),
};

enum Pair
{
    Both( f64, f64 ),
};

i32 area( Shape s )
{
    switch( s )
    {
        case Shape::Circle( r ):
            return 1;
    }

    return 0;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "6:5-6:5 error: expected an identifier, found `:`",
            "13:26-13:26 error: expected an identifier, found `(`",
            "19:11-19:14 error: a payload field needs a name = write one after its type, as in `Circle(f64 radius)`",
            "30:5-30:14 warning: this statement is never reached [-Wunreachable-code] = the one before it always leaves the "
            "block"
        }
    );
}

TEST_CASE( "errors_nameless_parameter", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A parameter with no name is a parameter: its type counts towards the signature, so a call checks
// and an overload compares as if it had one. A keyword where the name goes is reported once.
// No call names an overloaded pair, since a duplicate's calls are a separate cascade.

i32 lone( i32 )
{
    return 0;
}

i32 keyword( i32 this )
{
    return 0;
}

i32 twice( i32 a )
{
    return a;
}

i32 twice( i32 )
{
    return 0;
}

i32 second( i32 a, i32 )
{
    return a;
}

i32 second( i32 a, i32 b )
{
    return b;
}

class Counter
{
    i32 n;

    Counter( i32 start )
    {
        n = start;
    }

    Counter( i32 )
    {
        n = 0;
    }

    i32 add( i32 by )
    {
        return n + by;
    }

    i32 add( i32 )
    {
        return n;
    }
};

i32 main()
{
    return lone( 1 ) + keyword( 2 );
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "10:18-10:22 error: expected an identifier, found `this` = `this` is a keyword, so it cannot be used as a name",
            "20:5-20:10 error: `twice` is already declared with these parameters = previous declaration is at: t.kl:15:1",
            "30:5-30:11 error: `second` is already declared with these parameters = previous declaration is at: t.kl:25:1",
            "44:5-44:12 error: `Counter` is already declared with these parameters = previous declaration is at: t.kl:39:5",
            "54:9-54:12 error: `add` is already declared with these parameters = previous declaration is at: t.kl:49:5"
        }
    );
}

TEST_CASE( "errors_never", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// `never` is a return type and nothing else, a `never` function may not return, a call of one is a
// statement and has no value, `panic` takes a `str`, and both names are taken. One refusal per
// declaration.
class never
{
    i32 x;
};

struct Holds
{
    never n;
};

enum Payload
{
    one( never n )
};

never stop()
{
    panic( "stop" );
}

void parameter( never n )
{
}

void local()
{
    never n;
}

void pointer()
{
    never* p = nullptr;
}

void argument()
{
    result<never, i32> r = result::err( 1 );
}

never returns()
{
    return;
}

i32 initialised()
{
    i32 x = panic( "no" );
    return x;
}

i32 returned()
{
    return stop();
}

void inferred()
{
    auto x = stop();
}

i32 arm( bool c )
{
    return c ? 1 : stop();
}

void takes( i32 v )
{
}

void passed()
{
    takes( stop() );
}

void not_a_str()
{
    panic( 5 );
}

void named()
{
    i32 panic = 1;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "4:7-4:12 error: `never` is the name of a builtin type = a declaration may not take one",
            "11:5-11:10 error: a field cannot be `never` = only a function returns `never`",
            "16:10-16:15 error: a field cannot be `never` = only a function returns `never`",
            "24:17-24:22 error: a parameter cannot be `never` = only a function returns `never`",
            "30:5-30:10 error: a variable cannot be `never` = only a function returns `never`",
            "35:5-35:11 error: `never` is only a return type",
            "40:12-40:17 error: a type argument cannot be `never` = only a function returns `never`",
            "45:5-45:12 error: a `never` function cannot return = end it in `panic`, or a call of another `never` function",
            "50:13-50:26 error: expected `i32`, but got `never` = it never produces a value; call it as a statement",
            "56:12-56:18 error: expected `i32`, but got `never` = it never produces a value; call it as a statement",
            "61:14-61:20 error: a variable cannot be `never` = only a function returns `never`",
            "66:20-66:26 error: expected `i32`, but got `never` = it never produces a value; call it as a statement",
            "75:12-75:18 error: expected `i32`, but got `never` = it never produces a value; call it as a statement",
            "80:12-80:13 error: expected `str`, but got an integer literal",
            "85:9-85:14 error: expected an identifier, found `panic` = `panic` is a keyword, so it cannot be used as a name"
        }
    );
}

TEST_CASE( "errors_never_ends", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A `never` function that can reach its end is refused. Found over the lowered graph, which a
// program with other errors never reaches, so it has a fixture of its own.
never ends( bool c )
{
    if( c )
    {
        panic( "c" );
    }
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() == Lines { "3:1-9:2 error: this function returns `never`, but can reach its end = end every path in `panic`, "
                             "a call of another `never` function, or a loop that never exits" }
    );
}

TEST_CASE( "errors_non_interface_bounds", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A known type that is not an interface is refused as one; only a name nothing declares is unknown.
struct Plain { i32 x; };
enum Colour { red, green };

void on_struct<T>( T t ) where T : Plain {}
void on_builtin<T>( T t ) where T : i32 {}
void on_enum<T>( T t ) where T : Colour {}
void among<T>( T t ) where T : copyable & Plain {}
void on_parameter<T, U>( T t, U u ) where T : U, where U : copyable {}
void on_nothing<T>( T t ) where T : Nope {}

i32 main() { return 0; }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "5:36-5:41 error: `Plain` is not an interface = a bound is an interface, or one of copyable, numeric, integral, "
            "floating and defaultable",
            "6:37-6:40 error: `i32` is not an interface = a bound is an interface, or one of copyable, numeric, integral, "
            "floating and defaultable",
            "7:34-7:40 error: `Colour` is not an interface = a bound is an interface, or one of copyable, numeric, integral, "
            "floating and defaultable",
            "8:43-8:48 error: `Plain` is not an interface = a bound is an interface, or one of copyable, numeric, integral, "
            "floating and defaultable",
            "9:47-9:48 error: `U` is not an interface = a bound is an interface, or one of copyable, numeric, integral, "
            "floating and defaultable",
            "10:37-10:41 error: unknown bound `Nope` = a bound is an interface, or one of copyable, numeric, integral, "
            "floating and defaultable"
        }
    );
}

TEST_CASE( "errors_numeric_limits", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A limit belongs to a number type or a parameter bounded `numeric`, is a value rather than a
// function, and has the type it names.
T plain<T>() { return T::max; }
i32 main()
{
    i32 a = i32::max();
    i32 b = i32::biggest;
    bool c = bool::max;
    i8 d = i32::max;
    i32 e = i32::max + 1;
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "3:23-3:29 error: `T` has no `max` = a `where T : numeric` clause gives it `::max` and `::min`",
            "6:13-6:21 error: `i32::max` is a value, not a function = write `i32::max` without the `()`",
            "7:13-7:25 error: `i32` has no member `biggest` = a number type has only `i32::max` and `i32::min`",
            "8:14-8:18 error: `bool` is a builtin type, and has no members",
            "9:12-9:20 error: expected `i8`, but got `i32` = write `cast<i8>(i32::max)`",
            "10:13-10:25 error: `2147483648` does not fit in `i32`"
        }
    );
}

TEST_CASE( "errors_open_address_arguments", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// `&f` of an overloaded or generic `f` passed to a call that is refused: the refusal is the one error.

U apply<T, U>( fn( T ) -> U f, T x ) where T : copyable, where U : copyable
{
    return f( x );
}

i64 widen( i32 x )
{
    return cast<i64>( x );
}

i64 widen( i64 x )
{
    return x;
}

T id<T>( T x ) where T : copyable
{
    return x;
}

i64 take( fn( i32 ) -> i64 f )
{
    return f( 1 );
}

i64 take( fn( i64 ) -> i64 f )
{
    return f( 2 );
}

// A literal deduces nothing, so `T` is open.
void undeducible()
{
    i64 r = apply( &widen, 3 );
    i64 s = apply( &id, 3 );
}

void miscounted()
{
    i64 r = apply<i32>( &widen, 3 );
    i64 s = apply<i32>( &id, 3 );
}

// Both take `&widen`, so the call is what is ambiguous.
void both_take_it()
{
    i64 r = take( &widen );
}

// Outside a call, nothing chooses.
void alone()
{
    auto f = &widen;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "36:13-36:18 error: nothing here says what `T` is in `apply` = write the type arguments, as in `apply<i32, "
            "i32>(...)`",
            "37:13-37:18 error: nothing here says what `T` is in `apply` = write the type arguments, as in `apply<i32, "
            "i32>(...)`",
            "42:18-42:23 error: `apply` takes 2 type arguments, but 1 was given = `apply` declares `T` and `U`",
            "43:18-43:23 error: `apply` takes 2 type arguments, but 1 was given = `apply` declares `T` and `U`",
            "49:13-49:27 error: this call to `take` is ambiguous = more than one matches: `(fn(i32) -> i64)` and `(fn(i64) -> "
            "i64)`; write a type the call can be told by",
            "55:14-55:20 error: `widen` is overloaded, so its address names no one function = assigning it to a variable of "
            "one signature will choose between them"
        }
    );
}

TEST_CASE( "errors_operator_arithmetic", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// M9.6: what `a op b` and `-a` need when an operand is a user type. The left operand's type must
// declare the operator, the right must fit its parameter, and the result is what it returns. A
// built-in left operand never calls the right's operator, and a string literal on the left adopts
// only through `from<str>`.
struct vec
{
    f64 x;

    vec operator*( f64 by ) const { return vec { x * by }; }
    vec operator-( const ref vec other ) const { return vec { x - other.x }; }
};

struct plain
{
    i32 x;
};

i32 main()
{
    vec   v = vec { 1.0 };
    vec   w = vec { 2.0 };
    plain p = plain { 1 };
    f64   k = 2.0;

    vec sum  = v + w;
    vec both = v * w;
    vec neg  = -v;
    vec flip = ~v;
    vec left = 2.0 * v;
    vec held = k * v;
    i32 n    = v - w;

    plain glued = "ab" + p;
    vec   moved = v << 1;
    bool  not_v = !v;

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "25:16-25:21 error: `vec` has no `operator+` = declare `vec operator+(const ref vec other) const` in it",
            "26:20-26:21 error: expected `f64`, but got `vec`",
            "27:16-27:18 error: `vec` has no `operator-` = declare `vec operator-() const` in it",
            "28:16-28:18 error: `vec` has no `operator~` = declare `vec operator~() const` in it",
            "29:16-29:23 error: `*` on `f64` cannot take a `vec` = `vec`'s `operator*` is called on a `vec` on the left; write "
            "`v * 2.0` if the operation allows it",
            "30:16-30:21 error: `*` on `f64` cannot take a `vec` = `vec`'s `operator*` is called on a `vec` on the left; write "
            "`v * k` if the operation allows it",
            "31:16-31:21 error: expected `i32`, but got `vec`",
            "33:26-33:27 error: expected `textual`, but got `plain`",
            "34:19-34:25 error: no operator `<<` for `vec` and `i32` = both operands must be integers",
            "35:19-35:21 error: no operator `!` for `vec` = the operand must be `bool`"
        }
    );
}

TEST_CASE( "errors_operator_bounds", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// M9.6: a `T` has an operator only through a bound that requires it, and an implementer must match
// the requirement or declare it.
interface addable
{
    self operator+( const ref self other ) const;
};

interface negatable
{
    self operator-() const;
};

struct wrong implements addable
{
    i32 x;

    wrong operator+( i32 other ) const { return wrong { x + other }; }
};

struct missing implements negatable
{
    i32 x;
};

T unbounded<T>( T a, T b ) where T : copyable
{
    return a + b;
}

T other_bound<T>( T a, T b ) where T : copyable & negatable
{
    a += b;
    return a * b;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "13:25-13:32 error: `wrong`'s `operator+` does not match the one `addable` declares = `addable` wants `wrong "
            "operator+(const ref wrong other) const`",
            "20:27-20:36 error: `missing` implements `negatable` but has no method `operator-` = add `missing operator-() "
            "const` to `missing`",
            "27:12-27:17 error: no operator `+` for `T` and `T` = `+` needs `numeric`; write `where T : numeric` on "
            "`unbounded`",
            "32:5-32:12 error: no operator `+=` for `T`",
            "33:12-33:17 error: no operator `*` for `T` and `T` = `*` needs `numeric`; write `where T : numeric` on "
            "`other_bound`"
        }
    );
}

TEST_CASE( "errors_operator_compare", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// What `operator<=>` must be - a `const` method returning `ordering`, taking one more of its own
// type, declared once - and what `<=>` and the comparisons need from their operands.
struct returns_bool
{
    i32 x;

    bool operator<=>( const ref returns_bool other ) const
    {
        return true;
    }
};

struct not_const
{
    i32 x;

    ordering operator<=>( const ref not_const other )
    {
        return ordering::equal;
    }
};

struct other_type
{
    i32 x;

    ordering operator<=>( i32 other ) const
    {
        return ordering::equal;
    }
};

struct two_parameters
{
    i32 x;

    ordering operator<=>( const ref two_parameters p, const ref two_parameters q ) const
    {
        return ordering::equal;
    }
};

struct plain
{
    i32 x;
};

struct version
{
    i32 major;

    ordering operator<=>( const ref version other ) const
    {
        return major <=> other.major;
    }
};

enum Colour
{
    Red,
    Green
};

ordering by_bound<T>( T a, T b ) where T : equatable
{
    return a <=> b;
}

i32 main()
{
    plain   one = plain { 1 };
    plain   two = plain { 2 };
    version v   = version { 1 };
    version w   = version { 2 };

    if( one < two )
    {
        return 1;
    }

    ordering o = one <=> two;

    if( v == w )
    {
        return 2;
    }

    if( v < 5 )
    {
        return 3;
    }

    i32  n = v <=> w;
    bool b = true <=> false;
    bool c = Colour::Red <=> Colour::Green;

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "7:5-7:9 error: `operator<=>` must return `ordering`",
            "17:14-17:25 error: `operator<=>` must be `const` = write `const` after its parameter list; comparing reads both "
            "operands",
            "27:27-27:36 error: `operator<=>` on `other_type` must take another `other_type` = write the parameter as `const "
            "ref other_type`",
            "37:25-37:83 error: `operator<=>` takes one parameter, the right-hand operand",
            "66:12-66:19 error: no operator `<=>` for `T` and `T` = `<=>` needs `comparable`; write `where T : comparable` on "
            "`by_bound`",
            "76:9-76:18 error: `plain` has no `operator<=>` = declare `ordering operator<=>(const ref plain other) const` in "
            "it",
            "81:18-81:29 error: `plain` has no `operator<=>` = declare `ordering operator<=>(const ref plain other) const` in "
            "it",
            "83:9-83:15 error: `version` has no `operator==` = declare `bool operator==(const ref version other) const` in it",
            "88:13-88:14 error: expected `version`, but got an integer literal",
            "93:14-93:21 error: expected `i32`, but got `ordering`",
            "94:14-94:28 error: no operator `<=>` for `bool` and `bool`",
            "95:14-95:43 error: no operator `<=>` for `Colour` and `Colour`"
        }
    );
}

TEST_CASE( "errors_operator_compound", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// M9.6: what `a op= b` needs on a user type: an `operator op=`, or an `operator op` returning `a`'s
// own type for the fallback `a = a op b`, and a target that may be written. Shifts, `++` and `--`
// stay with the numbers.
struct plain
{
    i32 x;
};

struct meters
{
    f64 m;

    speed operator/( f64 s ) const { return speed { m / s }; }
};

struct speed
{
    f64 per_second;
};

struct counter
{
    i32 n;

    void operator+=( i32 by ) { n += by; }
    counter operator-( i32 by ) const { return counter { n - by }; }
};

void add_through( const ref counter c )
{
    c += 1;
}

i32 main()
{
    plain p = plain { 1 };
    p += plain { 2 };

    meters m = meters { 10.0 };
    m /= 2.0;

    const counter fixed = counter { 0 };
    fixed += 1;
    fixed -= 1;

    counter c = counter { 0 };
    c += counter { 1 };
    c <<= 1;
    c++;
    c--;

    i32 n = 0;
    n += c;

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "31:5-31:6 error: `c` is `const` = remove `const` to modify it",
            "37:5-37:22 error: no operator `+=` for `plain` = declare `operator+=`, or `operator+` returning `plain`",
            "40:5-40:14 error: `m /= 2.0` is `m = m / 2.0`, but `/` on `meters` gives `speed` = declare `void operator/=(f64 "
            "other)` on `meters`",
            "43:5-43:10 error: `fixed` is `const` = remove `const` to modify it",
            "44:5-44:10 error: `fixed` is `const` = remove `const` to modify it",
            "47:10-47:23 error: expected `i32`, but got `counter`",
            "48:5-48:13 error: no operator `<<=` for `counter`",
            "49:5-49:9 error: no operator `++` for `counter` = `++` and `--` are kept for the built-in numbers; write `c += 1`",
            "50:5-50:9 error: no operator `--` for `counter` = `++` and `--` are kept for the built-in numbers; write `c -= 1`",
            "53:10-53:11 error: expected `i32`, but got `counter`"
        }
    );
}

TEST_CASE( "errors_operator_declarations", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// M9.6: what each new operator must be. A binary one is `const`, takes one operand by value or
// `const ref`, and returns a value; a compound assignment is not `const`, takes one operand, and
// returns `void`; unary `-` and `~` are `const` and take none, and `-` with one is subtraction.
// Each is declared once, though `operator-()` and `operator-( x )` are two.
struct binary_not_const
{
    i32 x;

    binary_not_const operator+( const ref binary_not_const other ) { return other; }
};

struct binary_two_operands
{
    i32 x;

    binary_two_operands operator*( i32 a, i32 b ) const { return binary_two_operands { a * b }; }
};

struct binary_no_operand
{
    i32 x;

    binary_no_operand operator/() const { return binary_no_operand { x }; }
};

struct binary_by_ref
{
    i32 x;

    binary_by_ref operator&( ref binary_by_ref other ) const { return binary_by_ref { x }; }
};

struct binary_by_move
{
    i32 x;

    binary_by_move operator|( move binary_by_move other ) const { return binary_by_move { x }; }
};

struct binary_void
{
    i32 x;

    void operator^( i32 other ) const { }
};

struct compound_const
{
    i32 x;

    void operator+=( i32 by ) const { }
};

struct compound_returns
{
    i32 x;

    compound_returns operator*=( i32 by ) { return compound_returns { x * by }; }
};

struct compound_no_operand
{
    i32 x;

    void operator%=() { }
};

struct compound_by_out
{
    i32 x;

    void operator-=( out i32 by ) { by = 0; }
};

struct unary_with_operand
{
    i32 x;

    unary_with_operand operator~( i32 bits ) const { return unary_with_operand { x }; }
};

struct minus_two_operands
{
    i32 x;

    minus_two_operands operator-( i32 a, i32 b ) const { return minus_two_operands { a }; }
};

struct unary_not_const
{
    i32 x;

    unary_not_const operator-() { return unary_not_const { 0 - x }; }
};

struct unary_void
{
    i32 x;

    void operator~() const { }
};

struct twice
{
    i32 x;

    twice operator+( i32 by ) const { return twice { x + by }; }
    twice operator+( const ref twice other ) const { return twice { x + other.x }; }
    twice operator-() const { return twice { 0 - x }; }
    twice operator-() const { return twice { x }; }
};

struct both_minuses
{
    i32 x;

    both_minuses operator-() const { return both_minuses { 0 - x }; }
    both_minuses operator-( const ref both_minuses other ) const { return both_minuses { x - other.x }; }
};

struct static_operator
{
    i32 x;

    static static_operator operator*( i32 by ) { return static_operator { by }; }
};

struct generic_operator
{
    i32 x;

    generic_operator operator+<U>( U by ) const { return generic_operator { x }; }
};

interface badly_required
{
    void operator+( const ref self other ) const;
    self operator-=( i32 by );
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "9:22-9:31 error: `operator+` must be `const` = write `const` after its parameter list; `a + b` reads both "
            "operands",
            "16:34-16:50 error: `operator*` takes one parameter, the right-hand operand",
            "23:32-23:34 error: `operator/` takes one parameter, the right-hand operand",
            "30:30-30:53 error: `operator&` takes its right operand by value or `const ref` = `a & b` has nowhere to write a "
            "`ref`, `out` or `move`",
            "37:31-37:56 error: `operator|` takes its right operand by value or `const ref` = `a | b` has nowhere to write a "
            "`ref`, `out` or `move`",
            "44:5-44:9 error: `operator^` must return a value = `a ^ b` is an expression; to change `a` in place, declare "
            "`operator^=`",
            "51:10-51:20 error: `operator+=` cannot be `const` = it changes its left operand",
            "58:5-58:21 error: `operator*=` must return `void` = `a *= b` is a statement and changes `a` in place",
            "65:20-65:22 error: `operator%=` takes one parameter, the right-hand operand",
            "72:22-72:32 error: `operator-=` takes its right operand by value or `const ref` = `a -= b` has nowhere to write a "
            "`ref`, `out` or `move`",
            "79:33-79:45 error: `operator~` takes no parameter = `~a` has one operand, the object",
            "86:33-86:49 error: `operator-` takes no parameter, for `-a`, or one, for `a - b`",
            "93:21-93:30 error: `operator-` must be `const` = write `const` after its parameter list; `-a` reads its operand",
            "100:5-100:9 error: `operator~` must return a value",
            "108:11-108:20 error: `twice` already has an `operator+` = previous declaration is at: t.kl:107:5",
            "110:11-110:20 error: `twice` already has an `operator-` = previous declaration is at: t.kl:109:5",
            "125:28-125:37 error: an operator cannot be `static` = it is called on its left operand",
            "132:31-132:34 error: an operator cannot have type parameters of its own = it is called without any, so nothing "
            "could say what they are",
            "137:5-137:9 error: `operator+` must return a value = `a + b` is an expression; to change `a` in place, declare "
            "`operator+=`",
            "138:5-138:9 error: `operator-=` must return `void` = `a -= b` is a statement and changes `a` in place"
        }
    );
}

TEST_CASE( "errors_operator_equal", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// What `operator==` must be - a `const` method returning `bool`, taking one more of its own type,
// declared once - and what using `==` on an aggregate needs.
struct returns_i32
{
    i32 x;

    i32 operator==( const ref returns_i32 other ) const
    {
        return 0;
    }
};

struct not_const
{
    i32 x;

    bool operator==( const ref not_const other )
    {
        return true;
    }
};

struct other_type
{
    i32 x;

    bool operator==( i32 other ) const
    {
        return true;
    }
};

struct declared_twice
{
    i32 x;

    bool operator==( const ref declared_twice other ) const
    {
        return true;
    }

    bool operator==( declared_twice other ) const
    {
        return true;
    }
};

struct is_static
{
    i32 x;

    static bool operator==( const ref is_static other )
    {
        return true;
    }
};

struct two_parameters
{
    i32 x;

    bool operator==( const ref two_parameters p, const ref two_parameters q ) const
    {
        return true;
    }
};

class hidden
{
    hidden()
    {
        x = 0;
    }

    private bool operator==( const ref hidden other ) const
    {
        return true;
    }

    private i32 x;
};

struct plain
{
    i32 x;
};

struct point
{
    i32 x;

    bool operator==( const ref point other ) const
    {
        return x == other.x;
    }
};

i32 main()
{
    plain  one   = plain { 1 };
    plain  two   = plain { 2 };
    hidden h     = hidden();
    hidden k     = hidden();
    point  p     = point { 1 };

    if( one == two )
    {
        return 1;
    }

    if( h == k )
    {
        return 2;
    }

    if( one == 5 )
    {
        return 3;
    }

    if( p == 5 )
    {
        return 4;
    }

    if( p != one )
    {
        return 5;
    }

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "7:5-7:8 error: `operator==` must return `bool`",
            "17:10-17:20 error: `operator==` must be `const` = write `const` after its parameter list; comparing reads both "
            "operands",
            "27:22-27:31 error: `operator==` on `other_type` must take another `other_type` = write the parameter as `const "
            "ref other_type`",
            "42:10-42:20 error: `declared_twice` already has an `operator==` = previous declaration is at: t.kl:37:5",
            "52:17-52:27 error: an operator cannot be `static` = it is called on its left operand",
            "62:20-62:78 error: `operator==` takes one parameter, the right-hand operand",
            "106:9-106:19 error: `plain` has no `operator==` = declare `bool operator==(const ref plain other) const` in it",
            "111:9-111:15 error: `operator==` is private to `hidden` = only that type's own members may name it",
            "116:9-116:17 error: `plain` has no `operator==` = declare `bool operator==(const ref plain other) const` in it",
            "121:14-121:15 error: expected `point`, but got an integer literal",
            "126:14-126:17 error: expected `point`, but got `plain`"
        }
    );
}

TEST_CASE( "errors_operator_index", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// What `operator[]` must be - a `const` method taking one index by value or `const ref` and
// returning the element by `ref` or `const ref`, declared once.
struct returns_value
{
    i32 x;

    i32 operator[]( u64 index ) const
    {
        return x;
    }
};

struct returns_reference
{
    i32 x;

    const ref i32 operator[]( u64 index ) const
    {
        return x;
    }
};

struct returns_void_pointer
{
    i32 x;

    void* operator[]( u64 index ) const
    {
        return nullptr;
    }
};

struct not_const
{
    i32 x;

    i32* operator[]( u64 index )
    {
        return &x;
    }
};

struct ref_index
{
    i32 x;

    i32* operator[]( ref u64 index ) const
    {
        return nullptr;
    }
};

struct two_indices
{
    i32 x;

    i32* operator[]( u64 row, u64 column ) const
    {
        return nullptr;
    }
};

struct declared_twice
{
    i32 x;

    i32* operator[]( u64 index ) const
    {
        return nullptr;
    }

    const i32* operator[]( u64 index ) const
    {
        return nullptr;
    }
};

struct is_static
{
    i32 x;

    static i32* operator[]( u64 index )
    {
        return nullptr;
    }
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "7:5-7:8 error: `operator[]` returns `ref` or `const ref` = `v[i]` is the element it refers to",
            "27:5-27:10 error: `operator[]` returns `ref` or `const ref` = `v[i]` is the element it refers to",
            "37:5-37:9 error: `operator[]` returns `ref` or `const ref` = write `ref i32`, so a borrow of the element is "
            "checked",
            "37:10-37:20 error: `operator[]` must be `const` = write `const` after its parameter list; a `const` object is "
            "indexed too",
            "47:5-47:9 error: `operator[]` returns `ref` or `const ref` = write `ref i32`, so a borrow of the element is "
            "checked",
            "47:22-47:35 error: `operator[]` takes its index by value or `const ref` = `v[i]` has nowhere to write a `ref`, "
            "`out` or `move`",
            "57:5-57:9 error: `operator[]` returns `ref` or `const ref` = write `ref i32`, so a borrow of the element is "
            "checked",
            "57:20-57:43 error: `operator[]` takes one parameter, the index",
            "67:5-67:9 error: `operator[]` returns `ref` or `const ref` = write `ref i32`, so a borrow of the element is "
            "checked",
            "72:16-72:26 error: `declared_twice` already has an `operator[]` = previous declaration is at: t.kl:67:5",
            "82:17-82:27 error: an operator cannot be `static` = it is called on its left operand"
        }
    );
}

TEST_CASE( "errors_operator_index_uses", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// `v[i]` is a place, writable exactly when `v` is and its `operator[]` does not return `const ref`.
class buffer
{
    buffer()
    {
        unsafe
        {
            data = alloc<i32>( 4 );
        }
    }

    ~buffer()
    {
        unsafe
        {
            free( data );
        }
    }

    public ref i32 operator[]( u64 index ) const
    {
        unsafe
        {
            return data[index];
        }
    }

    public const ref i32 first() const
    {
        return this[0];
    }

    private i32[*] data;
};

class read_only
{
    read_only()
    {
        x = 0;
    }

    public const ref i32 operator[]( u64 index ) const
    {
        return x;
    }

    private i32 x;
};

class hidden
{
    hidden()
    {
        x = 0;
    }

    private ref i32 operator[]( u64 index ) const
    {
        return x;
    }

    private i32 x;
};

struct plain
{
    i32 x;
};

const ref i32 element( const ref buffer values )
{
    return values[0];
}

void write( const ref buffer values )
{
    values[0] = 1;
}

buffer make()
{
    return buffer();
}

i32 main()
{
    buffer values = buffer();
    const buffer fixed = buffer();
    read_only reads = read_only();
    hidden secret = hidden();
    plain p = plain { 1 };

    ref i32 bound = values[0];
    const ref i32 seen = values[1];
    fixed[0] = 2;
    reads[0] = 3;
    make()[0] = 4;
    i32 a = secret[0];
    i32 b = p[0];
    i32 c = values[true];

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "78:5-78:14 error: `values` is `const` = remove `const` to modify it",
            "96:5-96:13 error: `fixed` is `const` = remove `const` to modify it",
            "97:5-97:13 error: `read_only`'s `operator[]` only reads, so this cannot be modified = it returns `const ref`",
            "98:5-98:11 error: this value is a temporary, so writing to it has no effect = assign it to a variable and modify "
            "that",
            "99:13-99:22 error: `operator[]` is private to `hidden` = only that type's own members may name it",
            "100:13-100:17 error: `plain` has no `operator[]` = declare `ref T operator[]( u64 index ) const` in it",
            "101:20-101:24 error: expected `u64`, but got `bool`"
        }
    );
}

TEST_CASE( "errors_out", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN D31. `out` is the fourth marker: the callee assigns the caller's variable, and both sides
// say so. The *type* errors only - definite assignment lives in errors_out_assignment.kl, because
// it is reported after lowering and a type error here would stop the driver before it ran.
struct Point
{
    i32 x;
};

void init( out i32 n )
{
    n = 1;
}

// An owning `out` parameter is accepted: the caller drops what the place held just before the
// call. Kept so the line numbers below stay put.
class B
{
    u64 n;

    B( u64 x )
    {
        n = x;
    }

    ~B()
    {
    }
};

void owning( out B b )
{
}

i32 no_variable()
{
    init( out 1 );

    return 0;
}

i32 writes_a_const()
{
    const i32 k = 1;

    init( out k );

    return k;
}

i32 missing_marker()
{
    i32 x = 0;

    init( x );

    return x;
}

i32 local_mode()
{
    out i32 x = 1;

    return x;
}

// Everything below is accepted, and the fixture would be worth much less without it.

void both_arms( out i32 n, i32 c )
{
    if( c == 0 )
    {
        n = 1;
    }
    else
    {
        n = 2;
    }
}

void before_the_branch( out i32 n, i32 c )
{
    n = 0;

    if( c == 0 )
    {
        n = 1;
    }
}

// Forwarding: the callee's promise to assign is what the marker at that call site means.
void forwards( out i32 n )
{
    init( out n );
}

void builds( out Point p )
{
    p.x = 1;
}

i32 main()
{
    i32 x = 0;

    forwards( out x );

    Point v = Point { 0 };

    builds( out v );

    return x + v.x;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "36:15-36:16 error: `out` needs a variable to assign to",
            "45:15-45:16 error: `k` is `const` = remove `const` to modify it",
            "54:11-54:12 error: `init` assigns this argument = write `out`",
            "61:5-61:8 error: `out` is not a binding mode = `out` is how a callee assigns a caller's variable; write the type "
            "on its own"
        }
    );
}

TEST_CASE( "errors_out_assignment", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN D31. "The callee must assign it" is a promise to the caller rather than advice, so a path
// that returns without writing an `out` parameter is an error. A *must* analysis - assigned on
// every path - which is why it is a pass of its own: §8's move lattice has Uninitialised as its
// join identity, so there `Live` joined with `Uninitialised` is `Live`, and a value assigned on one
// branch would look assigned on all of them.
//
// Its own fixture, and the reason is the one already recorded for errors_owning_copies.kl: these
// are reported after lowering, so a *type* error anywhere in the file stops the driver before this
// pass ever runs and every diagnostic below vanishes from the golden.
void skips( out i32 n )
{
}

void one_arm_only( out i32 n, i32 c )
{
    if( c == 0 )
    {
        n = 1;
    }
}

// The case a *may* analysis gets wrong: the body assigns it, and the loop can still run zero times.
void loop_body_only( out i32 n, i32 c )
{
    while( c > 0 )
    {
        n = 1;
        c = c - 1;
    }
}

void early_return( out i32 n, i32 c )
{
    if( c == 0 )
    {
        return;
    }

    n = 1;
}

// One error per parameter, not one per function.
void two_of_them( out i32 a, out i32 b )
{
    a = 1;
}

// Everything below is accepted, and the fixture would be worth much less without it.

void init( out i32 n )
{
    n = 1;
}

void both_arms( out i32 n, i32 c )
{
    if( c == 0 )
    {
        n = 1;
    }
    else
    {
        n = 2;
    }
}

void before_the_branch( out i32 n, i32 c )
{
    n = 0;

    if( c == 0 )
    {
        n = 1;
    }
}

// Before a loop, which may run zero times and so cannot be relied on.
void before_the_loop( out i32 n, i32 c )
{
    n = 0;

    while( n < c )
    {
        n = n + 1;
    }
}

// Forwarding: the callee's promise to assign is what the marker at that call site means.
void forwards( out i32 n )
{
    init( out n );
}

// The pass asks whether a place was written, never what was written into it. An `out` parameter of
// pointer type is where that shows: `nullptr` satisfies it as completely as a real address would,
// because "the callee assigned it" is the whole of what `out` promises.
void to_null( out i32* p )
{
    p = nullptr;
}

// And the consequence, deliberate rather than missed: a raw pointer is outside §8's structural
// rule, so this hands back the address of a local and nothing objects. `ref` is the spelling that
// cannot dangle; `T*` is the one that can.
void escapes( out i32* p )
{
    i32 local = 7;

    p = &local;
}

i32 main()
{
    i32 x = 0;

    forwards( out x );

    return x;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "10:1-12:2 error: `n` is never assigned = an `out` parameter is the callee's promise to assign it",
            "14:1-20:2 error: `n` is not assigned on every path out of this function = an `out` parameter is the callee's "
            "promise to assign it",
            "23:1-30:2 error: `n` is not assigned on every path out of this function = an `out` parameter is the callee's "
            "promise to assign it",
            "36:9-36:16 error: `n` is never assigned = an `out` parameter is the callee's promise to assign it",
            "43:1-46:2 error: `b` is never assigned = an `out` parameter is the callee's promise to assign it"
        }
    );
}

TEST_CASE( "errors_out_owning_aliased", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D54. An `out` argument of an owning type destroys what its place held before the call, so no
// other argument may reach that object: the callee would see it destroyed under the other name.
class Buf
{
    public u64 n;

    Buf( u64 x )
    {
        n = x;
    }

    ~Buf()
    {
    }
};

void changes( out Buf a, ref Buf _ )
{
    a = Buf( 1 );
}

void reads( out Buf a, const ref Buf b )
{
    a = Buf( b.n );
}

void ints( out i32 a, ref i32 _ )
{
    a = 1;
}

i32 main()
{
    Buf v = Buf( 1 );

    changes( out v, ref v );
    reads( out v, v );

    ref Buf r = v;

    changes( out v, ref r );

    // Owns nothing, so nothing is destroyed: accepted.
    i32 x = 0;

    ints( out x, ref x );

    return x;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "36:21-36:26 error: one object is passed twice to this call, which could change it = `out v` at 36:14 is the same "
            "object; the callee would see it under two names",
            "37:19-37:20 error: one object is passed twice to this call, which could change it = `out v` at 37:12 is the same "
            "object; the callee would see it under two names",
            "41:21-41:26 error: one object is passed twice to this call, which could change it = `out v` at 41:14 is the same "
            "object; the callee would see it under two names"
        }
    );
}

TEST_CASE( "errors_overload_widening", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN §6.7 and D31. A call that matches nothing exactly widens, and what widening refuses: a borrow
// takes exactly its type, and two candidates each closer for one argument are ambiguous.

void read( const ref i64 a )
{
}

void fill( out i64 a )
{
    a = 1;
}

void near( i32 a, i64 b )
{
}

void near( i64 a, i32 b )
{
}

i32 main()
{
    i32 whole = 1;
    i32 unset;
    i16 small = 1;

    // A `const ref` is the caller's variable, and an `i32` is not an `i64`.
    read( whole );

    // Nor is an `out`, which would write eight bytes into four.
    fill( out unset );

    // Each is closer for one argument and further for the other.
    near( small, small );

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "28:11-28:16 error: cannot borrow `i32` as `const ref i64` = a borrow is the variable itself, so its type must "
            "match exactly",
            "31:11-31:20 error: cannot borrow `i32` as `out i64` = a borrow is the variable itself, so its type must match "
            "exactly",
            "34:5-34:25 error: this call to `near` is ambiguous = more than one matches: `(i32, i64)` and `(i64, i32)`; write "
            "a type the call can be told by"
        }
    );
}

TEST_CASE( "errors_overloads", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN §6.7. What overloading refuses, and it is one question throughout: could a call site say
// which of these it meant? Two that it could never tell apart are refused where they are written,
// and a call that cannot be told apart is refused where it is made.

// Identical parameters. The oldest of these rules, now asked of the signature rather than the name.
void same( i32 a )
{
}

void same( i32 b )
{
}

// A return type is not something a call site writes, so it cannot be what tells two apart.
void returns_differ( i32 a )
{
}

i32 returns_differ( i32 a )
{
    return a;
}

// A `const ref` takes no marker at the call, exactly like a bare parameter - so `unmarked( x )`
// means both of these and neither is reachable.
void unmarked( i32 a )
{
}

void unmarked( const ref i32 a )
{
}

// An `extern` keeps its C name, because that is the name someone else defined it under.
extern void outside( i32 a );
extern void outside( f64 a );

// A program has one entry point, and it is spelled `main` in C too.
i32 main()
{
    i32 whole = 1;
    f64 fraction = 1.0;

    // No candidate takes two arguments.
    pick( whole, whole );

    // No candidate takes a `bool`, and nothing widens into or out of one.
    pick( true );

    // A literal reaches only a type that holds its value, so one past the largest `i64` reaches
    // neither of these.
    ambiguous( 9223372036854775808 );

    // Nor can it choose between two whose difference is one it does not carry.
    ambiguous( fraction );

    // A method, asked the same questions.
    Pair p = Pair( 1 );
    p.get( true );
    p.get( whole, whole );

    return 0;
}

void pick( i32 a )
{
}

void pick( f64 a )
{
}

void ambiguous( i32 a )
{
}

void ambiguous( i64 a )
{
}

class Pair
{
    i32 x;

    Pair( i32 v )
    {
        x = v;
    }

    // Two methods that differ only in their receiver. A call writes the object, never the mode, so
    // `p.count()` means both.
    i32 count()
    {
        return x;
    }

    i32 count() const
    {
        return x;
    }

    i32 get( i32 n )
    {
        return x + n;
    }
};

class Slot<T> where T : copyable
{
    T held;

    Slot( T v )
    {
        held = v;
    }

    // `Slot<i32>` makes these one signature, and a method call writes no type argument that could
    // say which was meant - so the pair is refused here rather than at that instantiation.
    i32 put( T a )
    {
        return 0;
    }

    i32 put( i32 a )
    {
        return 1;
    }
};
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "10:6-10:10 error: `same` is already declared with these parameters = previous declaration is at: t.kl:6:1",
            "19:5-19:19 error: `returns_differ` is already declared with these parameters = two of one name must differ in "
            "their parameters, not only in what they return",
            "30:6-30:14 error: `unmarked` is already declared with these parameters = previous declaration is at: t.kl:26:1",
            "36:13-36:20 error: `outside` is defined in C, so it cannot be overloaded = previous declaration is at: t.kl:35:1",
            "45:5-45:25 error: no `pick` takes 2 arguments = the ones declared take `(i32)` and `(f64)`",
            "48:5-48:17 error: no `pick` matches these arguments = the ones declared take `(i32)` and `(f64)`",
            "52:5-52:37 error: no `ambiguous` matches these arguments = the ones declared take `(i32)` and `(i64)`",
            "55:5-55:26 error: no `ambiguous` matches these arguments = the ones declared take `(i32)` and `(i64)`",
            "59:12-59:16 error: expected `i32`, but got `bool`",
            "60:10-60:26 error: `get` takes 1 argument, but 2 were given",
            "97:9-97:14 error: `count` is already declared with these parameters = `const` binds the receiver, which a call "
            "site does not write, so it cannot tell two apart",
            "124:9-124:12 error: `put` is already declared, and a type argument could make the two identical = previous "
            "declaration is at: t.kl:119:5"
        }
    );
}

TEST_CASE( "errors_owning_conditional", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D31. A conditional hands on whichever arm it picks, so a named owning arm is written `move`:
// without it the caller's variable was taken silently, even by a call that only borrows.
class Buf
{
    u64 n;

    Buf( u64 m )
    {
        n = m;
    }

    ~Buf()
    {
    }
};

enum Holder
{
    Full( Buf b ),
    Empty
};

u64 peek( Buf b )
{
    return 0;
}

void initialises( bool c )
{
    Buf a = Buf( 1 );
    Buf b = Buf( 2 );
    Buf x = c ? a : b;
}

void assigns( bool c )
{
    Buf a = Buf( 1 );
    Buf x = Buf( 2 );
    x     = c ? a : Buf( 3 );
}

// The path with no expected type.
void infers( bool c )
{
    Buf  a = Buf( 1 );
    auto x = c ? Buf( 2 ) : a;
}

Buf returns( bool c, move Buf a, move Buf b )
{
    return c ? a : b;
}

// A borrow takes nothing, yet the arm was moved into a temporary and dropped with it.
u64 lends( bool c )
{
    Buf a = Buf( 1 );
    Buf b = Buf( 2 );

    return peek( c ? a : b );
}

Holder wraps( bool c )
{
    Buf a = Buf( 1 );

    return Holder::Full( c ? a : Buf( 2 ) );
}

void enums( bool c )
{
    Holder a = Holder::Empty;
    Holder b = Holder::Empty;
    Holder x = c ? a : b;
}

// A pattern binding cannot be moved at all, so its help says why rather than asking for `move`.
Buf binds( bool c, Holder h )
{
    switch( h )
    {
        case Holder::Full( b ):
            return c ? b : Buf( 0 );
        case Holder::Empty:
            return Buf( 0 );
    }
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "32:17-32:18 error: an owning value is transferred, not copied = write `move a`",
            "32:21-32:22 error: an owning value is transferred, not copied = write `move b`",
            "39:17-39:18 error: an owning value is transferred, not copied = write `move a`",
            "46:29-46:30 error: an owning value is transferred, not copied = write `move a`",
            "51:16-51:17 error: an owning value is transferred, not copied = write `move a`",
            "51:20-51:21 error: an owning value is transferred, not copied = write `move b`",
            "60:22-60:23 error: an owning value is transferred, not copied = write `move a`",
            "60:26-60:27 error: an owning value is transferred, not copied = write `move b`",
            "67:30-67:31 error: an owning value is transferred, not copied = write `move a`",
            "74:20-74:21 error: an owning value is transferred, not copied = write `move a`",
            "74:24-74:25 error: an owning value is transferred, not copied = write `move b`",
            "83:24-83:25 error: an owning value is transferred, not copied = `b` still belongs to what holds it"
        }
    );
}

TEST_CASE( "errors_owning_copies", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN D31. An owning value is transferred, not copied, and the transfer is written down - in an
// initialiser and an assignment as much as at a call. Its own fixture rather than part of
// errors_moves.kl, because these are type errors: the driver stops before lowering when one is
// reported, so the move analysis would never run and its diagnostics would vanish from that file.
class Owned
{
    u64 id;

    Owned( u64 n )
    {
        id = n;
    }

    ~Owned()
    {
    }
};

i32 copies()
{
    Owned a = Owned( 1 );
    Owned c = a;

    return 0;
}

i32 infers()
{
    Owned a = Owned( 1 );
    auto  c = a;

    return 0;
}

i32 assigns()
{
    Owned a = Owned( 1 );
    Owned c = Owned( 2 );

    c = a;

    return 0;
}

// Everything below is accepted, and the fixture would be worth much less without it: it is what
// shows the rule is not simply refusing every use of an owning type.

i32 transfers()
{
    Owned a = Owned( 1 );
    Owned c = move a;

    return 0;
}

// A temporary has no other owner, so handing it over is the only thing that can happen to it, and
// no variable is left behind for a reader to wonder about.
i32 temporaries()
{
    Owned d = Owned( 3 );

    d = Owned( 4 );

    return 0;
}

// Copying is what a non-owning type is for.
struct Point
{
    i32 x;
};

i32 plain()
{
    Point a = Point { 1 };
    Point c = a;

    return c.x;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "22:15-22:16 error: an owning value is transferred, not copied = write `move a`",
            "30:15-30:16 error: an owning value is transferred, not copied = write `move a`",
            "40:9-40:10 error: an owning value is transferred, not copied = write `move a`"
        }
    );
}

TEST_CASE( "errors_owning_escapes", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// An owning value leaves somewhere only by `move`, and only a local or a parameter may be moved or
// returned bare. A pointee, a field and an element reached through `[]` stay where they are, so
// returning one would copy it and moving one would leave it counted twice.
class owned
{
    owned( u64 n )
    {
        id = n;
    }

    ~owned()
    {
    }

    private u64 id;
};

class box
{
    box()
    {
        unsafe
        {
            data = alloc<owned>( 1 );
            data[0] = owned( 1 );
        }
        kept = owned( 2 );
    }

    public ref owned operator[]( u64 index ) const
    {
        assert( index == 0 );

        unsafe
        {
            return data[index];
        }
    }

    public owned leak() const
    {
        return kept;
    }

    // A raw slot is this class's own storage, and taking from it is still a transfer.
    public owned take_raw()
    {
        unsafe
        {
            return data[0];
        }
    }

    private owned[*] data;
    private owned kept;
};

owned first( owned* p )
{
    return *p;
}

T first_of<T>( T* p )
{
    return *p;
}

owned make()
{
    owned made = owned( 3 );
    return made;
}

i32 main()
{
    box b = box();
    owned taken = move b[0];
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "42:16-42:20 error: an owning value is transferred, not copied = `kept` still belongs to what holds it",
            "50:20-50:27 error: an owning value is transferred, not copied = write `move data[0]`",
            "60:12-60:14 error: an owning value is transferred, not copied = `*p` still belongs to what holds it",
            "65:12-65:14 error: an owning value is transferred, not copied = `*p` still belongs to what holds it",
            "77:24-77:28 error: an element reached through `[]` cannot be moved out = it stays in its container; take it out "
            "through one of the container's methods"
        }
    );
}

TEST_CASE( "errors_owning_globals", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Nothing drops a file-scope variable or a static field, so neither may own anything: an enum or a
// `result` that carries an owning payload is refused, initialised or not. A copyable one is zero,
// its first variant, as an integer global is. An error union's zero tag need not name one of its
// members, so a union is refused whatever it holds.
class Buf
{
    u64* p;

    ~Buf()
    {
    }
};

enum Holds
{
    one( Buf b ),
    none
};

enum Plain
{
    a,
    b( u64 n )
};

Holds held;
Holds initialised = Holds::none;
result<Buf, i32> wrapped;
Holds | Plain united;

struct Registry
{
    i32 count;

    static Holds last;
    static Plain kind;
};

Plain fine;
result<u64, i32> copied;
Plain | Bad copyable;

enum Bad
{
    nope
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "26:1-26:12 error: a file-scope variable of `Holds` needs an initialiser = an enum has no default: which variant "
            "is the program's to say",
            "28:1-28:26 error: a file-scope variable of `result<Buf, i32>` needs an initialiser = an enum has no default: "
            "which variant is the program's to say",
            "29:1-29:22 error: a file-scope variable of `Holds | Plain` needs an initialiser = its zero need not be one of its "
            "members",
            "35:12-35:23 error: a static field of `Holds` needs an initialiser = an enum has no default: which variant is the "
            "program's to say",
            "41:1-41:22 error: a file-scope variable of `Plain | Bad` needs an initialiser = its zero need not be one of its "
            "members"
        }
    );
}

TEST_CASE( "errors_owning_payloads", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN D30 and D7. An enum owns exactly when a payload does, so it is moved rather than copied, and
// a pattern binding borrows the payload in place: nothing may take it out of the enum.
class Buf
{
    u64 n;

    Buf( u64 m )
    {
        n = m;
    }

    ~Buf()
    {
    }
};

enum Holder
{
    Full( Buf b ),
    Empty
};

enum Option<T>
{
    Some( T value ),
    None
};

// Owning through a payload is owning: a struct may not hold one.
struct Pair
{
    Holder h;
};

void copies( Holder h )
{
    Holder c = h;
}

// The variant takes its payload over, so a named one is moved in.
Holder wraps()
{
    Buf b = Buf( 1 );

    return Holder::Full( b );
}

Buf returns_binding( Holder h )
{
    switch( h )
    {
        case Holder::Full( b ):
            return b;
        case Holder::Empty:
            return Buf( 0 );
    }
}

void moves_binding( Holder h )
{
    switch( h )
    {
        case Holder::Full( b ):
            Buf taken = move b;
            return;
        case Holder::Empty:
            return;
    }
}

void copies_binding( Holder h )
{
    switch( h )
    {
        case Holder::Full( b ):
            Buf copied = b;
            return;
        case Holder::Empty:
            return;
    }
}

// An unbounded `T` may own, so the generic is refused before any instance exists.
T unwrap<T>( Option<T> o, move T fallback )
{
    switch( o )
    {
        case Option::Some( value ):
            return value;
        case Option::None:
            return move fallback;
    }
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "32:5-32:14 error: a struct cannot contain `Holder`, which owns a resource = a struct is copied freely, so declare "
            "`Pair` as a class if it owns this",
            "37:16-37:17 error: an owning value is transferred, not copied = take it as `move Holder h` to own it",
            "45:26-45:27 error: an owning value is transferred, not copied = write `move b`",
            "53:20-53:21 error: an owning value is transferred, not copied = `b` still belongs to what holds it",
            "64:30-64:31 error: a binding cannot be moved = `b` is part of the value being matched, which still owns it",
            "76:26-76:27 error: an owning value is transferred, not copied = `b` still belongs to what holds it",
            "89:20-89:25 error: an owning value is transferred, not copied = `value` still belongs to what holds it"
        }
    );
}

TEST_CASE( "errors_param_default_overloads", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D79: an overload with defaults that some call could not tell apart from another is refused.
i32 f( i32 a ) { return a; }
i32 f( i32 a, i32 b = 0 ) { return a + b; }

i32 g( i32 a, f64 b = 0.0 ) { return a; }
i32 g( i32 a, i64 b = 0 ) { return a; }

// Accepted: the first parameter tells every call apart.
i32 h( i64 a, i32 b = 0 ) { return b; }
i32 h( f64 a, i32 b = 0 ) { return b; }

struct S
{
    i32 n;

    i32 m( i32 a = 0 ) const { return a; }
    i32 m() const { return 1; }
};

i32 main()
{
    const i64 wide = 1;
    return h( wide ) + h( 1.5, 2 );
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "3:5-3:6 error: a call of `f` with 1 argument could mean either declaration = remove a default, or change a "
            "parameter's type",
            "6:5-6:6 error: a call of `g` with 1 argument could mean either declaration = remove a default, or change a "
            "parameter's type",
            "17:9-17:10 error: a call of `m` with 0 arguments could mean either declaration = remove a default, or change a "
            "parameter's type"
        }
    );
}

TEST_CASE( "errors_param_default_pointer", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D79: a default is not part of a function's type, so `&f` has every parameter.
i32 one( i32 a, i32 b = 2 ) { return a + b; }

i32 two( i32 a, i32 b = 2 ) { return a + b; }
i32 two( f64 a ) { return 0; }

i32 main()
{
    fn( i32, i32 ) -> i32 full  = &one;
    fn( i32 ) -> i32      short = &one;
    fn( i32 ) -> i32      over  = &two;
    return full( 1, 2 ) + short( 1 ) + over( 1 );
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "10:35-10:39 error: a default is not part of `one`'s type, so `&one` is `fn(i32, i32) -> i32` = call `one` from a "
            "function of type `fn(i32) -> i32` instead",
            "11:35-11:39 error: no overload of `two` has the type `fn(i32) -> i32` = its overloads are `fn(i32, i32) -> i32` "
            "and `fn(f64) -> i32`, and a default is not part of a type"
        }
    );
}

TEST_CASE( "errors_param_defaults", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D79: where a parameter's default is refused, and what it may not read.
i32 later( i32 a = 1, i32 b ) { return a + b; }

void writes( ref i32 n = 0 ) { }
void fills( out i32 n = 0 ) { n = 1; }

i32 reads_another( i32 a, i32 b = a ) { return b; }

extern i32 abs( i32 v = 0 );

interface sized
{
    u64 size( u64 extra = 0 ) const;
};

interface shape
{
    f64 area( f64 scale ) const;
};

struct Square implements shape
{
    f64 side;

    f64 area( f64 scale = 2.0 ) const { return side * side * scale; }

    f64 scaled( f64 by = side ) const { return by; }
    f64 again( f64 by = this.side ) const { return by; }
    f64 called( f64 by = area() ) const { return by; }

    bool operator==( const ref Square other = Square { 1.0 } ) const { return side == other.side; }
};

i32 two( i32 a, i32 b = 2 ) { return a + b; }

i32 main( const ref str[] args = [ "keel" ] )
{
    two();
    two( 1, 2, 3 );
    return args.size == 0 ? 1 : 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "2:23-2:28 error: a parameter after a default needs one too = give `b` a default, or move it before `a`",
            "4:26-4:27 error: a `ref` parameter cannot have a default = it needs a place the caller names",
            "5:25-5:26 error: an `out` parameter cannot have a default = it needs a place the caller names",
            "7:35-7:36 error: `a` is not declared = a default cannot read another parameter",
            "9:25-9:26 error: an `extern` cannot have a default parameter = pass the value at each call instead",
            "13:27-13:28 error: an interface's method cannot have a default parameter = pass the value at each call instead",
            "21:26-21:31 error: `Square`'s `area` cannot have a default, since it answers `shape` = a call through `shape` "
            "would not see it; pass the value at each call instead",
            "27:26-27:30 error: a default cannot read the object it is called on = it runs at the caller, before the call",
            "28:25-28:29 error: a default cannot read the object it is called on = it runs at the caller, before the call",
            "29:26-29:30 error: a default cannot read the object it is called on = it runs at the caller, before the call",
            "31:47-31:61 error: an operator cannot have a default parameter = pass the value at each call instead",
            "36:34-36:44 error: `main` cannot have a default parameter = pass the value at each call instead",
            "38:8-38:10 error: `two` takes 1 to 2 arguments, but 0 were given",
            "39:8-39:19 error: `two` takes 1 to 2 arguments, but 3 were given"
        }
    );
}

TEST_CASE( "errors_parenthesised_span", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// An expression built on a parenthesised operand is underlined from its `(`.
i32 main()
{
    i8  z = ( 100 ) * 2; // error
    i8  w = 2 * ( 100 ); // error
    i32 q = ( -2147483647 - 1 ) / -1; // error

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "4:13-4:24 error: `200` does not fit in `i8`",
            "5:13-5:24 error: `200` does not fit in `i8`",
            "6:13-6:37 error: `2147483648` does not fit in `i32`"
        }
    );
}

TEST_CASE( "errors_payload_aliases", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D54: a pattern binding borrows its payload until its last use, and a change to what was matched
// breaks it whichever name the change is made through.
class Buf
{
    u64* data;

    Buf( u64 n )
    {
        unsafe { data = alloc<u64>(); }
        *data = n;
    }

    ~Buf()
    {
        unsafe { free( data ); }
    }

    u64 get() const
    {
        return *data;
    }
};

enum Holder
{
    Full( Buf b ),
    Empty
};

class Wh
{
    public Holder held;

    Wh()
    {
        held = Holder::Full( Buf( 3 ) );
    }
};

void clear( ref Holder h )
{
    h = Holder::Empty;
}

// The scrutinee is an alias; the assignment names the object itself.
u64 switch_on_alias()
{
    Holder h = Holder::Full( Buf( 1 ) );
    ref Holder r = h;

    switch( r )
    {
        case Holder::Full( b ):
            h = Holder::Empty;
            return b.get();
        case Holder::Empty:
            return 0;
    }
}

u64 assign_alias()
{
    Holder h = Holder::Full( Buf( 1 ) );
    ref Holder r = h;

    switch( h )
    {
        case Holder::Full( b ):
            r = Holder::Empty;
            return b.get();
        case Holder::Empty:
            return 0;
    }
}

u64 lend_alias()
{
    Holder h = Holder::Full( Buf( 1 ) );
    ref Holder r = h;

    switch( h )
    {
        case Holder::Full( b ):
            clear( ref r );
            return b.get();
        case Holder::Empty:
            return 0;
    }
}

// The scrutinee is a field; the assignment replaces its owner through an alias.
u64 assign_alias_of_owner()
{
    Wh w = Wh();
    ref Wh r = w;

    switch( w.held )
    {
        case Holder::Full( b ):
            r = Wh();
            return b.get();
        case Holder::Empty:
            return 0;
    }
}

void take( move Holder _ )
{
}

u64 replaces( move Holder h )
{
    switch( h )
    {
        case Holder::Full( b ):
            h = Holder::Empty;
            return b.get();
        case Holder::Empty:
            return 0;
    }
}

u64 lends( move Holder h )
{
    switch( h )
    {
        case Holder::Full( b ):
            clear( ref h );
            return b.get();
        case Holder::Empty:
            return 0;
    }
}

u64 gives( move Holder h )
{
    switch( h )
    {
        case Holder::Full( b ):
            take( move h );
            return b.get();
        case Holder::Empty:
            return 0;
    }
}

class Keeper
{
    Holder held;

    Keeper( move Holder h )
    {
        held = move h;
    }

    void clear()
    {
        held = Holder::Empty;
    }

    u64 peek()
    {
        switch( held )
        {
            case Holder::Full( b ):
                clear();
                return b.get();
            case Holder::Empty:
                return 0;
        }
    }
};

// Accepted: `b` is not used after the assignment.
u64 assign_after_last_use()
{
    Holder h = Holder::Full( Buf( 1 ) );

    switch( h )
    {
        case Holder::Full( b ):
            u64 n = b.get();
            h = Holder::Empty;
            return n;
        case Holder::Empty:
            return 0;
    }
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "54:13-54:31 error: `h` is assigned while `b` still borrows from it = `b` is used after this; it was bound at "
            "53:28",
            "69:13-69:31 error: `h` is assigned while `b` still borrows from it = `b` is used after this; it was bound at "
            "68:28",
            "84:20-84:25 error: this could change `h` while `b` still borrows from it = `b` is used after this; it was bound "
            "at 83:28",
            "100:13-100:22 error: `w` is assigned while `b` still borrows from it = `b` is used after this; it was bound at "
            "99:28",
            "116:13-116:31 error: `h` is assigned while `b` still borrows from it = `b` is used after this; it was bound at "
            "115:28",
            "128:20-128:25 error: this could change `h` while `b` still borrows from it = `b` is used after this; it was bound "
            "at 127:28",
            "140:19-140:25 error: `h` is moved while `b` still borrows `h.b` = `b` is used after this; it was bound at 139:28",
            "166:17-166:24 error: this could change `this` while `b` still borrows from it = `b` is used after this; it was "
            "bound at 165:32"
        }
    );
}

TEST_CASE( "errors_payload_names", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A payload field is named; the switch that reads one stays quiet.
enum List
{
    empty,
    link( i32, box<List> ),
};

enum Shape
{
    circle( f64 ),
    square( f64 side, f64 ),
};

i32 head( const ref List l )
{
    switch( l )
    {
        case List::empty: return 0;
        case List::link( v, rest ): return v;
    }
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "5:11-5:14 error: a payload field needs a name = write one after its type, as in `Circle(f64 radius)`",
            "10:13-10:16 error: a payload field needs a name = write one after its type, as in `Circle(f64 radius)`",
            "11:23-11:26 error: a payload field needs a name = write one after its type, as in `Circle(f64 radius)`"
        }
    );
}

TEST_CASE( "errors_pointer_to_const", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A pointer to const, refused one way to a function: writing through it, dropping its `const`,
// releasing it, and the two spellings of `const` that mean something else. The address of a
// read-only place is one, which is what the last two catch.
struct Point
{
    i32 x;
};

class Counter
{
    Counter()
    {
        n = 0;
    }

    public void bump()
    {
        n += 1;
    }

    public i32 peek() const
    {
        i32* p = &n;

        return *p;
    }

    private i32 n;
};

void writes_through_it( const i32* q )
{
    *q = 1;
}

void writes_a_field_through_it( const Point* q )
{
    q.x = 1;
}

void writes_an_element_through_it( const i32[*] q )
{
    unsafe
    {
        q[ 0 ] = 1;
    }
}

void calls_a_method_that_writes( const Counter* q )
{
    q.bump();
}

void drops_its_const( const i32* q )
{
    i32* w = q;
}

void adds_const_two_levels_down( i32** raw )
{
    const i32** q = raw;
}

void frees_it( const u8[*] q )
{
    unsafe
    {
        free( q );
    }
}

void writes_after_the_const()
{
    i32 const* q = nullptr;
}

void writes_const_twice()
{
    const i32 const* q = nullptr;
}

i32 takes_the_address_of_a_const()
{
    const i32 k = 1;
    i32*      p = &k;

    return *p;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "23:18-23:20 error: expected `i32*`, but got `const i32*` = a pointer to `const` never converts back to one that "
            "writes",
            "33:5-33:7 error: this is reached through a `const i32*`, so it cannot be modified = a pointer to `const` only "
            "reads what it points at",
            "38:5-38:8 error: this is reached through a `const Point*`, so it cannot be modified = a pointer to `const` only "
            "reads what it points at",
            "45:9-45:15 error: this is reached through a `const i32[*]`, so it cannot be modified = a pointer to `const` only "
            "reads what it points at",
            "51:5-51:6 error: `bump` may modify its object, which is reached through a `const Counter*` = a pointer to `const` "
            "can call only `const` methods",
            "56:14-56:15 error: expected `i32*`, but got `const i32*` = a pointer to `const` never converts back to one that "
            "writes",
            "61:21-61:24 error: expected `const i32**`, but got `i32**`",
            "68:9-68:18 error: `free` cannot release a `const u8[*]`, which points to `const` = if it came from `alloc`, "
            "`cast<u8[*]>` it back first",
            "74:9-74:14 error: `const` goes before the type = write `const i32*`",
            "79:15-79:20 error: `const` is written twice = both apply to `i32`; a `const` after the `*` makes the pointer "
            "`const`",
            "85:19-85:21 error: expected `i32*`, but got `const i32*` = a pointer to `const` never converts back to one that "
            "writes"
        }
    );
}

TEST_CASE( "errors_private_in_public", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A public declaration's signature may not name a module-private type; its body may.
private struct inner { i32 x; };
private enum shade { light, dark };
private interface quiet { i32 q() const; };

inner make() { return inner { 1 }; }
i32 read( inner* p ) { return p.x; }
shade[2] pair = [ shade::light, shade::dark ];

struct Outer { inner held; };
enum Wrapped { Some( inner value ), None };
interface Maker { inner made() const; };

class Holder
{
    private inner kept;
    public Holder( inner i ) { kept = i; }
    public inner get() const { return kept; }
    private inner peek() const { return kept; }
};

// Accepted: private signatures, and a public body.
private inner helper() { return inner { 2 }; }
i32 body() { return helper().x; }

i32 main() { return body() - 2; }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "6:1-6:6 error: `inner` is private to `t`, but `make` is not = make `make` private, or `inner` public",
            "7:11-7:16 error: `inner` is private to `t`, but `read` is not = make `read` private, or `inner` public",
            "8:1-8:6 error: `shade` is private to `t`, but `pair` is not = make `pair` private, or `shade` public",
            "10:16-10:21 error: `inner` is private to `t`, but `Outer::held` is not = make `Outer` private, or `inner` public",
            "11:22-11:27 error: `inner` is private to `t`, but `Wrapped::Some` is not = make `Wrapped` private, or `inner` "
            "public",
            "12:19-12:24 error: `inner` is private to `t`, but `Maker::made` is not = make `Maker` private, or `inner` public",
            "17:20-17:25 error: `inner` is private to `t`, but `Holder`'s constructor is not = make the constructor private, "
            "or `inner` public",
            "18:12-18:17 error: `inner` is private to `t`, but `Holder::get` is not = make `Holder::get` private, or `inner` "
            "public"
        }
    );
}

TEST_CASE( "errors_private_overload", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A private overload that alone takes the arguments is named, not the public one's arity.
class Acct
{
    i32 v;
    public Acct() { v = 0; }
    private Acct( i32 x ) { v = x; }
    public i32 pick() { return v; }
    private i32 pick( i32 x ) { return x; }
};

i32 main()
{
    Acct a = Acct( 3 );
    Acct b = Acct();
    return b.pick( 3 );
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "13:14-13:18 error: `Acct`'s constructor taking 1 argument is private",
            "15:12-15:18 error: `pick` is private to `Acct` = only that type's own members may name it"
        }
    );
}

TEST_CASE( "errors_ptr_init", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// `ptr_init( p, v )` needs `unsafe`, a `T*` that does not point to `const`, and a `T`; an owning
// value is moved in, as at any other write.
class Owner
{
    u8 held;

    public Owner() { held = 0; }
    ~Owner() { }
};

void outside( i32* p )
{
    ptr_init( p, 1 );
}

void many( i32[*] slots )
{
    unsafe
    {
        ptr_init( slots, 1 );
    }
}

void not_a_pointer( i32 n )
{
    unsafe
    {
        ptr_init( n, 1 );
    }
}

void read_only( const i32* p )
{
    unsafe
    {
        ptr_init( p, 1 );
    }
}

void wrong_type( i32* p )
{
    unsafe
    {
        ptr_init( p, true );
    }
}

void copied( Owner* p, Owner o )
{
    unsafe
    {
        ptr_init( p, o );
    }
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "13:5-13:21 error: `ptr_init` needs an `unsafe` block = the compiler cannot tell that no value is there yet",
            "20:19-20:24 error: `ptr_init` needs a `T*`, but got `i32[*]` = a `T[*]` slot is raw storage, so `p[i] = v` "
            "already drops nothing",
            "28:19-28:20 error: `ptr_init` needs a `T*`, but got `i32` = it writes the first value a `T*` points at",
            "36:19-36:20 error: `ptr_init` cannot write through a `const i32*`, which points to `const`",
            "44:22-44:26 error: expected `i32`, but got `bool`",
            "52:22-52:23 error: an owning value is transferred, not copied = take it as `move Owner o` to own it"
        }
    );
}

TEST_CASE( "errors_read_move", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// `move` where the value is only read: a condition, an operator's operand, a compound assignment's
// value. A line ending `// warning` warns exactly once, a line ending `// error` reports exactly one
// error, and every other line reports neither.
class Buf
{
    u64 n;

    Buf( u64 v )
    {
        n = v;
    }

    ~Buf()
    {
    }

    bool operator==( const ref Buf other ) const
    {
        return n == other.n;
    }
};

void conditions( bool b, bool c, bool d )
{
    if( move b ) // warning
    {
    }
    while( move c ) // warning
    {
    }
    i32 e = ( move d ) ? 1 : 2; // warning
    _ = e;
}

void operands( i32 a, i32 b, i32 c, Buf x, move Buf y )
{
    i32 d = ( move a ) + 1; // warning
    i32 e = -move b; // warning
    bool f = x == ( move y ); // warning
    d += move c; // warning
    _ = e;
    _ = f;
    _ = d;
}

// `move` takes all of `a > 0`, which is not a variable.
void whole( i32 a )
{
    if( move a > 0 ) // error
    {
    }
}

// A move that hands the value on is still accepted.
Buf kept( move Buf x, bool b )
{
    Buf y = move x;
    bool c = b ? true : false;
    _ = c;
    return move y;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "25:9-25:15 warning: `move` is not needed in a condition [-Wneedless-move] = it only reads the value; drop `move`",
            "28:12-28:18 warning: `move` is not needed in a condition [-Wneedless-move] = it only reads the value; drop `move`",
            "31:13-31:23 warning: `move` is not needed in a condition [-Wneedless-move] = it only reads the value; drop `move`",
            "37:13-37:23 warning: `move` is not needed on an operator's operand [-Wneedless-move] = it only reads the value; "
            "drop `move`",
            "38:14-38:20 warning: `move` is not needed on an operator's operand [-Wneedless-move] = it only reads the value; "
            "drop `move`",
            "39:19-39:29 warning: `move` is not needed on an operator's operand [-Wneedless-move] = it only reads the value; "
            "drop `move`",
            "40:10-40:16 warning: `move` is not needed on a compound assignment's value [-Wneedless-move] = it only reads the "
            "value; drop `move`",
            "49:14-49:19 error: only a variable or an owned temporary can be moved = `move` takes the whole expression after it"
        }
    );
}

TEST_CASE( "errors_redeclaration", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(i32 duplicated()
{
    return 1;
}

i32 duplicated()
{
    return 2;
}

i32 twice( i32 p, i32 p )
{
    i32 local = 1;
    i32 local = 2;
    return local;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "6:5-6:15 error: `duplicated` is already declared with these parameters = previous declaration is at: t.kl:1:1",
            "11:23-11:24 error: `p` is already declared in this scope = previous declaration is at: t.kl:11:12",
            "14:9-14:14 error: `local` is already declared in this scope = previous declaration is at: t.kl:13:5"
        }
    );
}

TEST_CASE( "errors_ref_bindings", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN D32. A `ref` binding is initialised at its declaration and never reseated. That is what
// makes it safe with no lifetime analysis: the referent was declared before the binding, so it is
// in the same scope or an enclosing one and outlives it. A temporary is the one thing that would
// not, and binding to one is refused here rather than tracked.
i32 make()
{
    return 1;
}

i32 uninitialised()
{
    ref i32 r;

    return 0;
}

i32 binds_a_call_result()
{
    ref i32 r = make();

    return r;
}

i32 binds_a_literal()
{
    ref i32 r = 1;

    return r;
}

// The binding is the variable itself, so there is no conversion step for a widened copy to live in.
i32 binds_a_narrower_type()
{
    u8 x = 1;

    ref i32 r = x;

    return r;
}

// A local already owns what it holds, and `out` needs definite assignment in a callee - neither is
// a binding mode.
i32 uses_move()
{
    move i32 m = 1;

    return m;
}

i32 uses_out()
{
    out i32 o = 1;

    return o;
}

class B
{
    public u64 n;

    B( u64 x )
    {
        n = x;
    }

    ~B()
    {
    }
};

// A mutable binding onto something held read-only would launder the borrow.
i32 rebinds_a_read_only_borrow( B b )
{
    ref B r = b;

    return 0;
}

// Everything below is accepted, and the fixture would be worth much less without it.

struct Point
{
    i32 x;
};

i32 binds_places()
{
    i32 a = 1;

    ref i32 r = a;

    Point p = Point { 2 };

    ref i32 piece = p.x;

    i32* addr = &a;

    ref i32 pointee = *addr;

    r       = 5;
    piece   = 6;
    pointee = 7;

    return a + p.x;
}

// Binding an owning local is a borrow, not a transfer, so D31 demands no `move` for it.
i32 binds_an_owning_local()
{
    B a = B( 1 );

    ref B r = a;

    r.n = 5;

    return 0;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "12:5-12:15 error: a `ref` binding must be initialised = write `ref T r = x;`",
            "19:17-19:23 error: a `ref` binding needs a variable to bind to = drop `ref` to copy the value",
            "26:17-26:18 error: a `ref` binding needs a variable to bind to = drop `ref` to copy the value",
            "36:17-36:18 error: cannot borrow `u8` as `ref i32` = a borrow is the variable itself, so its type must match "
            "exactly",
            "45:5-45:9 error: `move` is not a binding mode = a local already owns what it holds; write the type on its own",
            "52:5-52:8 error: `out` is not a binding mode = `out` is how a callee assigns a caller's variable; write the type "
            "on its own",
            "74:15-74:16 error: `b` is borrowed, so it cannot be modified = take it as `ref B b` to modify it"
        }
    );
}

TEST_CASE( "errors_ref_return_element", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A `ref` result reached through `operator[]` is rooted in its object, so a local's element dies
// with the function and a parameter's lives on.
struct cell
{
    i32 x;

    const ref i32 operator[]( u64 index ) const
    {
        return x;
    }
};

const ref i32 from_local()
{
    cell c = cell { 1 };
    return c[0];
}

const ref i32 from_parameter( const ref cell c )
{
    return c[0];
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() == Lines { "16:12-16:16 error: a returned reference must borrow from a parameter = anything else here dies "
                             "when this function returns" }
    );
}

TEST_CASE( "errors_reference_return_writes", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN §6.7. A call returning a reference names a place - the referent was alive at the call site -
// so a projection off it is a place too, and reading one is exactly what the return is for. A
// `const ref` result is read-only, so writing through one has to be refused.
//
// It is accepted today, and it is not merely useless: `by_ref( a ).t = 2;` emits a store and `a`
// changes. The same write through a named `const ref` binding is already refused, so one fact has
// two spellings and two answers. place_root resolved only a `Name_expr`, so a call root became an
// invalid id and check_writable read that as permission.

struct P
{
    i32 t;
};

struct Holder
{
    P inner;

    const ref P get() const
    {
        return inner;
    }
};

const ref P by_ref( const ref P p )
{
    return p;
}

P* by_pointer( P* p )
{
    return p;
}

i32 main()
{
    P      a = P { 1 };
    Holder h = Holder { P { 2 } };

    // Each of these writes through a read-only reference.
    by_ref( a ).t = 2;
    by_ref( a ).t++;

    // Through a method rather than a free function: a different call site of the same rule, since
    // a receiver reaches check_writable from the method call rather than from the assignment.
    h.get().t = 5;

    // A mutable borrow of a read-only place is the same refusal, asked before anything is written.
    ref i32 r = by_ref( a ).t;

    // Legal: reading through the reference is the whole point of returning one.
    i32 v = by_ref( a ).t;

    // Legal: a pointer's referent outlives the call, so this is an ordinary write. The bare `p.t`
    // spelling reaches through the pointer, which is the form the rule actually has to recognise.
    by_pointer( &a ).t = 3;

    return v;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "41:5-41:18 error: `by_ref` returns a const ref, so it cannot be modified = a `const ref` result is read-only",
            "42:5-42:18 error: `by_ref` returns a const ref, so it cannot be modified = a `const ref` result is read-only",
            "46:5-46:14 error: `get` returns a const ref, so it cannot be modified = a `const ref` result is read-only",
            "49:17-49:30 error: `by_ref` returns a const ref, so it cannot be modified = a `const ref` result is read-only"
        }
    );
}

TEST_CASE( "errors_refinement", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D40: an interface requires another by naming it after `implements`. Conforming to it implies the
// other, checked against the same members; a cycle is refused at the name that closes it; one requiring a bound-only interface is bound-only too; and a `ref` to the
// requiring interface does not become one of the required.
interface Named
{
    i32 id() const;
};

interface Shape implements Named
{
    f64 area() const;
};

struct Square implements Shape
{
    f64 side;

    i32 id() const { return 1; }
    f64 area() const { return side * side; }
};

enum Tile
{
    Floor,
    Wall,
};

implements Shape for Tile
{
    f64 area() const { return 1.0; }
};

interface Loop implements Again
{
    void spin() const;
};

interface Again implements Loop
{
    void turn() const;
};

interface Itself implements Itself
{
    void be() const;
};

interface Sized implements i32
{
    u64 size() const;
};

// What `Keyed` promises includes `same`, which two values behind a `ref Keyed` could not honour.
interface Same
{
    bool same( const ref self other ) const;
};

interface Keyed implements Same
{
    u64 key() const;
};

void hold( const ref Keyed k )
{
}

i32 named( const ref Named n )
{
    return n.id();
}

i32 shaped( const ref Shape s )
{
    return named( s );
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "28:12-28:17 error: `Tile` implements `Named` but has no method `id` = add `i32 id() const` to the block",
            "38:28-38:32 error: `Again` cannot require `Loop`, which already requires `Again`",
            "43:29-43:35 error: `Itself` cannot require itself",
            "48:28-48:31 error: `i32` is not an interface = only an interface can be implemented",
            "64:22-64:27 error: `Keyed` can only bound a type parameter, because it requires `Same`, whose `same` mentions "
            "`self` = take a `T` with `where T : Keyed` instead",
            "75:19-75:20 error: expected `Named`, but got `Shape`"
        }
    );
}

TEST_CASE( "errors_refs", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN D31/D32. The marker appears in the signature and at the call site and the two must agree.
// Unlike `move`, `ref` is not exempt on a type that owns nothing: `move` is the caller's own
// assertion that the source is dead afterwards, which the callee never sees, while `ref` changes
// what the callee is holding whatever the type is.
struct Point
{
    i32 x;
};

void bump( ref i32 n )
{
    n = n + 1;
}

void plain( i32 n )
{
}

i32 missing_marker()
{
    i32 x = 1;

    bump( x );

    return x;
}

i32 marker_not_asked_for()
{
    i32 x = 1;

    plain( ref x );

    return x;
}

i32 not_a_place()
{
    bump( ref 1 );

    return 0;
}

// A binding is the caller's object itself, so there is no conversion step for a widened copy to
// live in: `ref u8` and `ref i32` are different bindings, not convertible ones. Without this the
// callee would write into a temporary nobody reads.
i32 not_convertible()
{
    u8 x = 1;

    bump( ref x );

    return 0;
}

// `out` needs definite assignment in the callee, which is §8's lattice read from the Uninitialised
// end. Refused rather than half-supported until that exists.
void init( out i32 n )
{
    n = 1;
}

// Everything below is accepted, and the fixture would be worth much less without it: it is what
// shows the rule is not simply refusing every marker it meets.

i32 lends_a_field()
{
    Point p = Point { 1 };

    bump( ref p.x );

    return p.x;
}

i32 lends_a_pointee()
{
    i32 x = 1;
    i32* q = &x;

    bump( ref *q );

    return x;
}

i32 still_exempt()
{
    i32 x = 1;

    plain( move x );

    return 0;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "23:11-23:12 error: `bump` may modify this argument = write `ref`",
            "32:12-32:17 error: `plain` does not modify this argument = remove `ref`",
            "39:15-39:16 error: `ref` needs a variable to borrow",
            "51:11-51:16 error: cannot borrow `u8` as `ref i32` = a borrow is the variable itself, so its type must match "
            "exactly"
        }
    );
}

TEST_CASE( "errors_refused_block_body", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A block for this package's own struct or class is refused once. Its bodies are not checked: the
// bare field, the private one, and the mistyped return would all read differently inside the type.
interface Named
{
    u64 id() const;
};

class Y
{
    u64 d;
};

struct Z
{
    u64 e;
};

implements Named for Y
{
    u64 id() const { return d + this.d; }
};

implements Named for Z
{
    u64 id() const { return e + "x"; }
};

i32 main() { return 0; }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "18:22-18:23 error: `Y` is declared in this package, so a block cannot be written for it = write `implements "
            "Named` where `Y` is declared",
            "23:22-23:23 error: `Z` is declared in this package, so a block cannot be written for it = write `implements "
            "Named` where `Z` is declared"
        }
    );
}

TEST_CASE( "errors_repeated_bounds", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// One mistake, one error: a bound named twice, an instance's unmet bound, a name a parameter hides.
interface Shape { i32 area() const; };
struct Plain { i32 n; };

class Sorted<T> where T : comparable & copyable
{
    T item;
    public Sorted( T x ) { item = x; }
};

void twice<T>( T _ ) where T : copyable & copyable { }
void twice_interface<T>( const ref T _ ) where T : Shape & Shape { }

// The annotation and the constructor name one instance.
void instance( Plain x )
{
    Sorted<Plain> s = Sorted( x );
}

void hides_bound<copyable>( copyable _ ) { }
void hides_interface<Shape>( Shape _ ) { }
void hides_struct<Plain>( Plain _ ) { }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "11:43-11:51 error: `copyable` is named twice for `T` = remove one",
            "12:60-12:65 error: `Shape` is named twice for `T` = remove one",
            "17:12-17:17 error: `Sorted` needs a `T` that implements `comparable`, but `Plain` does not = declare `Plain` with "
            "`implements equatable, comparable`",
            "20:18-20:26 error: `copyable` already names a bound = a type parameter would hide it; give it a name of its own, "
            "such as `T`",
            "21:22-21:27 error: `Shape` already names an interface = a type parameter would hide it; give it a name of its "
            "own, such as `T`",
            "22:19-22:24 error: `Plain` already names a type = a type parameter would hide it; give it a name of its own, such "
            "as `T`"
        }
    );
}

TEST_CASE( "errors_requirement_conflicts", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Two requirements of one name and parameters are one method to an implementer, so a set of them
// that disagrees on the return, a `ref` return or `static` can never be met: refused once, at the
// interface or the `where` clause that first brings them together. One interface naming a
// requirement twice is refused too. `const` may differ, since a `const` method answers both.
interface Base
{
    i32 f() const;
};

interface Clash implements Base
{
    i64 f() const;
};

// Inherits the conflict without adding one.
interface Inherits implements Clash
{
    void k();
};

interface Dup
{
    i32 f() const;
    i32 f() const;
};

interface Shape
{
    f64 area() const;
};

interface Other
{
    i32 area() const;
};

interface Both implements Shape, Other
{
    void h();
};

interface Stat
{
    static i32 make();
};

interface Inst
{
    i32 make();
};

interface Held
{
    ref i32 at( u64 i );
};

interface Copied
{
    i32 at( u64 i );
};

// Accepted: overloads by parameter, and `const` beside not.
interface Over
{
    i32 g( i32 x ) const;
    i32 g( i64 x ) const;
};

interface Lean
{
    i32 c() const;
};

interface Loose
{
    i32 c();
};

i32 one<T>( const ref T t ) where T : Shape & Other
{
    return 0;
}

i32 two<T>( const ref T t ) where T : Stat & Inst
{
    return 0;
}

i32 three<T>( ref T t ) where T : Held & Copied
{
    return 0;
}

i32 lean<T>( const ref T t ) where T : Lean & Loose
{
    return 0;
}

// The conflict is `Clash`'s, so its calls add nothing.
i64 via_clash<T>( const ref T t ) where T : Clash
{
    return t.f();
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "12:5-12:19 error: `Clash` requires `f` to return `i64`, and `Base` requires it to return `i32` = no type could "
            "implement both; rename one, or make the two agree",
            "24:5-24:19 error: `Dup` requires `f` twice = remove one",
            "37:34-37:39 error: `Shape` requires `area` to return `f64`, and `Other` requires it to return `i32` = no type "
            "could implement both; rename one, or make the two agree",
            "79:29-79:52 error: `Shape` requires `area` to return `f64`, and `Other` requires it to return `i32` = no type "
            "could implement both; rename one, or make the two agree",
            "84:29-84:50 error: `Stat` requires `make` to be `static`, and `Inst` requires it to take an object = no type "
            "could implement both; rename one, or make the two agree",
            "89:25-89:48 error: `Held` and `Copied` both require `at`, but only one returns it by `ref` = no type could "
            "implement both; rename one, or make the two agree"
        }
    );
}

TEST_CASE( "errors_resolve_hints", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Small resolve diagnostics: each says what the name is, or where it was first declared.
enum E { a, b };
enum F { a };
enum D { x, y, x };

i32 one() { return 1; }

i32 main() {
    E e = b;
    E f = a;
    i32 one = 2;
    i32 n = one();
    E v = E::a;
    E w = v::a;
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "4:16-4:17 error: `D` already has a variant `x` = previous declaration is at: t.kl:4:10",
            "9:11-9:12 error: `b` is not declared = write `E::b`",
            "10:11-10:12 error: `a` is not declared = write `E::a` or `F::a`",
            "12:13-12:16 error: `one` is not callable = this `one` is the local, which hides the function of that name",
            "14:11-14:12 error: `v` is a variable, not a type = `::` reaches a variant of an `enum` or a static method of a "
            "`struct` or `class`"
        }
    );
}

TEST_CASE( "errors_returns", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D9's other half: a non-void function that can reach its end without producing a value. The
// lowerer has to terminate every block, so the fall-off-the-end path returns the slot regardless -
// which made this a wrong answer delivered silently rather than a diagnostic.
i32 nothing_at_all()
{
}

i32 only_when_positive( i32 x )
{
    if( x > 0 )
    {
        return 1;
    }
}

i32 only_one_arm( i32 x )
{
    switch( x )
    {
        case 1: return 1;
        default: i32 y = 2; _ = y;
    }
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "4:1-6:2 error: this function never returns a value = it returns `i32`, so every path out of it must produce one",
            "8:1-14:2 error: this function does not return a value on every path = it returns `i32`, so every path out of it "
            "must produce one",
            "16:1-23:2 error: this function does not return a value on every path = it returns `i32`, so every path out of it "
            "must produce one"
        }
    );
}

TEST_CASE( "errors_run_escape", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// §8 for runs. An element or a sub-run of a run parameter borrows what the parameter borrows, so it
// may be returned. What a `T[*]` points at outlives the call, as the `T*` half of §8 already says, so
// an element or a sub-run reached through one may be returned too. The accepted cases come first.
struct Bytes
{
    u8[*] data;
    u64   size;

    const ref u8[] as_text() const
    {
        unsafe
        {
            return data[ 0..size ];
        }
    }
};

const ref u8[] middle( const ref u8[] text )
{
    return text[ 1..3 ];
}

const ref u8 first( const ref u8[] text )
{
    return text[ 0 ];
}

const ref u8[] tail( ref u8[] text )
{
    return text[ 1..text.size ];
}

const ref u8[] passes( const ref u8[] text )
{
    return text;
}

const ref u8[] through_a_call( const ref u8[] text )
{
    return passes( text )[ 0..1 ];
}

const ref u8[] from_a_many_pointer( u8[*] data, u64 size )
{
    unsafe
    {
        return data[ 0..size ];
    }
}

const ref u8 an_element_of_a_many_pointer( u8[*] data )
{
    unsafe
    {
        return data[ 0 ];
    }
}

const ref u8[] from_a_local_many_pointer( u64 size )
{
    unsafe
    {
        u8[*] data = alloc<u8>( size );

        return data[ 0..size ];
    }
}

// Accepted: a local binding lasts as long as the parameter it was bound to.
const ref u8[] slices_a_local_binding( const ref u8[] text )
{
    const ref u8[] local = text;

    return local[ 0..1 ];
}

const ref u8 indexes_a_local_binding( const ref u8[] text )
{
    const ref u8[] local = text;

    return local[ 0 ];
}

const ref u8[] passes_a_local_binding( const ref u8[] text )
{
    const ref u8[] local = text;

    return passes( local )[ 0..1 ];
}

// `size` and `data` are the callee's own copy of the run's description.
const ref u64 returns_the_size( const ref u8[] text )
{
    return text.size;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines { "94:12-94:21 error: `size` and `data` describe the run, so they cannot be returned by reference = return a copy"
        }
    );
}

TEST_CASE( "errors_run_positions", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Every place a run may not be written, one to a function so each is reported on its own. A run is
// written only straight after `ref` or `const ref`; the positions that accept one come first.
interface Shape
{
    f64 area() const;
};

struct Box<T> where T : copyable
{
    T value;
};

struct Holder
{
    u8[] bytes;
};

u64 accepted( const ref u8[] text, ref i32[] out_run, fn( const ref u8[] ) -> u64 measure )
{
    const ref u8[] again = text;

    return measure( again );
}

const ref u8[] returned( const ref u8[] text )
{
    return text;
}

void by_value( u8[] text )
{
}

u8[] returned_by_value( const ref u8[] text )
{
    return text;
}

void local()
{
    u8[] text;
}

void through_a_pointer( u8[]* text )
{
}

void through_a_many_pointer( u8[][*] texts )
{
}

void out_parameter( out u8[] text )
{
}

void move_parameter( move u8[] text )
{
}

void a_run_of_runs( const ref u8[][] texts )
{
}

void a_run_of_void( const ref void[] nothing )
{
}

void a_run_of_never( const ref never[] nothing )
{
}

void a_run_of_shapes( const ref Shape[] shapes )
{
}

void a_by_value_function_parameter( fn( u8[] ) -> u64 measure )
{
}

void a_generic_argument( const ref Box<u8[]> box )
{
}

void take<T>( const ref T value )
{
}

void an_explicit_type_argument( const ref u8[] text )
{
    take<u8[]>( text );
}

void a_deduced_type_argument( const ref u8[] text )
{
    take( text );
}

struct Reader
{
    i32 count;

    void read<T>( const ref T value ) const
    {
    }
};

void a_deduced_method_type_argument( const ref Reader reader, const ref u8[] text )
{
    reader.read( text );
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "15:5-15:9 error: `u8[]` is a run, so it is reached only through `ref` = borrow it as `const ref u8[]`, or point "
            "at its elements with `u8[*]`",
            "30:16-30:20 error: `u8[]` is a run, so it is reached only through `ref` = borrow it as `const ref u8[]`, or point "
            "at its elements with `u8[*]`",
            "34:1-34:5 error: `u8[]` is a run, so it is reached only through `ref` = borrow it as `const ref u8[]`, or point "
            "at its elements with `u8[*]`",
            "41:5-41:9 error: `u8[]` is a run, so it is reached only through `ref` = borrow it as `const ref u8[]`, or point "
            "at its elements with `u8[*]`",
            "44:25-44:29 error: `u8[]` is a run, so it is reached only through `ref` = borrow it as `const ref u8[]`, or point "
            "at its elements with `u8[*]`",
            "48:30-48:34 error: `u8[]` is a run, so it is reached only through `ref` = borrow it as `const ref u8[]`, or point "
            "at its elements with `u8[*]`",
            "52:25-52:29 error: `u8[]` is a run, so it is reached only through `ref` = borrow it as `const ref u8[]`, or point "
            "at its elements with `u8[*]`",
            "56:27-56:31 error: `u8[]` is a run, so it is reached only through `ref` = borrow it as `const ref u8[]`, or point "
            "at its elements with `u8[*]`",
            "60:31-60:35 error: `u8[]` is a run, so it is reached only through `ref` = borrow it as `const ref u8[]`, or point "
            "at its elements with `u8[*]`",
            "64:31-64:37 error: `void[]` has no element to hold = name the element's type, as in `u8[]`",
            "68:32-68:39 error: `never` is only a return type",
            "72:33-72:38 error: `Shape` is an interface, so it cannot be held by value = borrow one as `const ref Shape` or "
            "`ref Shape`, or point at one with `Shape*`",
            "76:41-76:45 error: `u8[]` is a run, so it is reached only through `ref` = borrow it as `const ref u8[]`, or point "
            "at its elements with `u8[*]`",
            "80:40-80:44 error: `u8[]` is a run, so it is reached only through `ref` = borrow it as `const ref u8[]`, or point "
            "at its elements with `u8[*]`",
            "90:10-90:14 error: `u8[]` is a run, so it is reached only through `ref` = borrow it as `const ref u8[]`, or point "
            "at its elements with `u8[*]`",
            "95:11-95:15 error: `T` cannot be `u8[]`: a run is never a type argument = take the run as `const ref u8[]`",
            "109:18-109:22 error: `T` cannot be `u8[]`: a run is never a type argument = take the run as `const ref u8[]`"
        }
    );
}

TEST_CASE( "errors_runs", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Using a run: indexing and slicing it, its `size` and `data`, and the ways it is never a value. A
// run is borrowed, so it is indexed and sliced without `unsafe`; a `T[*]` knows no length, so
// slicing one needs it. The accepted uses come first.
struct Grid
{
    i32 cell;

    const ref i32 operator[]( u64 at ) const
    {
        return cell;
    }
};

u64 accepted( const ref u8[] text, ref i32[] counts, u8[*] raw, u64 n )
{
    u8 first = text[ 0 ];
    u8 last  = text[ text.size - 1 ];

    const ref u8[] middle = text[ 1..3 ];
    const ref u8[] whole  = text[ 0..text.size ];
    const ref u8[] empty  = text[ 2..2 ];
    const ref u8[] inner  = middle[ 0..1 ];

    counts[ 0 ]         = 7;
    counts[ 1..3 ][ 0 ] = 8;

    const u8[*] bytes = text.data;
    i32[*]      ints  = counts.data;

    unsafe
    {
        const ref u8[] made = raw[ 0..n ];
        u8             byte = raw[ n..n + 1 ][ 0 ];

        counts.data[ 0 ] = 9;
    }

    return whole.size + empty.size + inner.size;
}

void index_must_be_an_integer( const ref u8[] text, bool flag )
{
    u8 a = text[ flag ];
    const ref u8[] b = text[ flag..2 ];
    const ref u8[] c = text[ 0..1.5 ];
}

void slices_run_upwards( const ref u8[] text, u64 n )
{
    const ref u8[] backwards = text[ 3..1 ];
    const ref u8[] unknown   = text[ n..1 ];
}

void slicing_a_many_pointer_needs_unsafe( u8[*] raw )
{
    const ref u8[] run = raw[ 0..4 ];
}

void a_run_needs_no_unsafe( const ref u8[] text )
{
    unsafe
    {
        const ref u8[] run = text[ 0..1 ];
    }
}

void others_cannot_be_sliced( u8* one, i32 n, Grid grid )
{
    u8  a = one[ 0..1 ][ 0 ];
    i32 b = n[ 0..1 ];
    i32 c = grid[ 0..1 ];
}

void fields( const ref u8[] text )
{
    u64 length = text.length;
    u8[*] bytes = text.data;
}

void writes( const ref u8[] text, ref u8[] other, u8[*] raw, const u8[*] fixed )
{
    text[ 0 ] = 1;
    text[ 0..2 ][ 0 ] = 1;

    unsafe
    {
        text.data[ 0 ]     = 1;
        fixed[ 0..2 ][ 0 ] = 1;
    }

    other.size = 3;
    other.data = raw;
}

void consume( const ref u8[] text )
{
}

void never_a_value( const ref u8[] text, ref u8[] other, bool flag )
{
    auto copy = text;
    consume( flag ? text : other );
    consume( move text );
    other = text;
    auto address = &text;
    bool same = text == other;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "43:18-43:22 error: an index must be an integer, but got `bool`",
            "44:30-44:34 error: an index must be an integer, but got `bool`",
            "45:33-45:36 error: an index must be an integer, but got `f64`",
            "50:38-50:42 error: `3..1` runs backwards = a range runs upwards; the lower bound comes first",
            "56:26-56:37 error: slicing needs an `unsafe` block = nothing checks that the run is inside the allocation",
            "61:5-64:6 error: an `unsafe` block that does nothing unsafe = remove `unsafe`",
            "69:13-69:24 error: `u8*` points at one `u8`, so it cannot be indexed = a many-item pointer, `u8[*]`, can be",
            "70:13-70:22 error: `i32` cannot be indexed",
            "71:13-71:25 error: `Grid` cannot be sliced",
            "76:18-76:29 error: a run has no field `length` = it has `size` and `data`",
            "77:19-77:28 error: expected `u8[*]`, but got `const u8[*]` = a pointer to `const` never converts back to one that "
            "writes",
            "82:5-82:14 error: `text` is `const` = remove `const` to modify it",
            "83:5-83:22 error: `text` is `const` = remove `const` to modify it",
            "87:9-87:23 error: this is reached through a `const u8[*]`, so it cannot be modified = a pointer to `const` only "
            "reads what it points at",
            "88:9-88:27 error: this is reached through a `const u8[*]`, so it cannot be modified = a pointer to `const` only "
            "reads what it points at",
            "91:5-91:15 error: `size` and `data` describe the run, so they cannot be modified",
            "92:5-92:15 error: `size` and `data` describe the run, so they cannot be modified",
            "101:17-101:21 error: a run cannot be held by value = borrow it with `const ref`",
            "102:14-102:33 error: a run cannot be held by value = borrow it with `const ref`",
            "103:19-103:23 error: a run cannot be held by value = borrow it with `const ref`",
            "104:5-104:10 error: a run cannot be assigned whole = assign its elements one at a time",
            "105:20-105:25 error: a run has no single address = `text.data` points at its first element",
            "106:17-106:30 error: no operator `==` for `u8[]` and `u8[]`"
        }
    );
}

TEST_CASE( "errors_sample_m7", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// The refusals M7's acceptance names, beside the program in codegen/sample_m7.kl that is accepted:
// a named constructor called through a value, and a hidden representation reached from outside.
class Window
{
    f64 lo;
    f64 hi;

    private Window( f64 a, f64 b )
    {
        lo = a;
        hi = b;
    }

    static Window around( f64 centre, f64 radius )
    {
        return Window( centre - radius, centre + radius );
    }
};

i32 main()
{
    Window w = Window::around( 3.0, 1.0 );

    Window v = w.around( 1.0, 1.0 );
    Window u = Window( 1.0, 2.0 );

    f64 low = w.lo;
    auto hi = &Window::hi;

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "24:16-24:24 error: `around` is a `static` method of `Window`, so it takes no object = call it as "
            "`Window::around(...)`",
            "25:16-25:22 error: `Window`'s constructor is private = build it through one of its own static methods instead",
            "27:15-27:19 error: `lo` is private to `Window` = only that type's own members may name it",
            "28:15-28:26 error: `hi` is private to `Window` = only that type's own members may name it"
        }
    );
}

TEST_CASE( "errors_sample_m8", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// The refusals M8's acceptance names, beside the program in codegen/sample_m8 that is accepted: a
// single-item pointer walked like a run, an address that outlives its local, a write through a
// literal's read-only bytes, and an element moved out through `[]`.
class Item
{
    public i32 n;

    Item()
    {
        n = 0;
    }

    ~Item()
    {
    }
};

class Bag
{
    Item[*] slots;

    Bag()
    {
        slots = nullptr;
    }

    public ref Item operator[]( u64 index ) const
    {
        unsafe
        {
            return slots[index];
        }
    }
};

void keep( move Item item )
{
}

i32* dangle()
{
    i32 x = 1;
    return &x;
}

i32 main()
{
    i32  x = 1;
    i32* p = &x;

    i32 second = p[1];
    i32* next  = p + 1;

    unsafe
    {
        i32[*] run = alloc<i32>( 2 );
        run[1]     = 2;
        free( run );
    }

    str hello = "hello";
    unsafe
    {
        hello.data[0] = 72;
    }

    Bag bag = Bag();
    keep( move bag[0] );

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "43:12-43:14 error: `x` dies when this function returns, so its address would dangle = return the value itself, or "
            "allocate it with `alloc` and return that",
            "51:18-51:22 error: `i32*` points at one `i32`, so it cannot be indexed = a many-item pointer, `i32[*]`, can be",
            "52:18-52:19 error: `i32*` points at one `i32`, so it has no arithmetic = a many-item pointer, `i32[*]`, can be "
            "offset",
            "64:9-64:22 error: this is reached through a `const u8[*]`, so it cannot be modified = a pointer to `const` only "
            "reads what it points at",
            "68:16-68:22 error: an element reached through `[]` cannot be moved out = it stays in its container; take it out "
            "through one of the container's methods"
        }
    );
}

TEST_CASE( "errors_self", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D55: `self` is written only in an interface, where it names the type that implements it. An
// interface whose methods mention it can only bound a type parameter. Each mistake is reported once,
// where it was made.
interface Same
{
    bool same( const ref self other ) const;
};

// A mismatch quotes the requirement with `self` read as the implementer.
struct P implements Same
{
    i32 v;

    bool same( const ref i32 other ) const { return true; }
};

// A caller reaches it only through a bound, so the help says so.
struct Q implements Same
{
    i32 v;

    bool same( const ref Q other ) { return true; }
};

// Held as anything but a bound.
void borrowed( ref Same s ) { }
void pointed( Same* s ) { }
void boxed( box<Same> s ) { }

// Inside a bounded generic, `self` is the `T`.
bool compare<T>( const ref T a ) where T : Same
{
    return a.same( 1 );
}

// An implementer writes its own name.
struct R implements Same
{
    i32 v;

    bool same( const ref self other ) const { return true; }
};

// Outside a type there is nothing to name, and inside one the help names it.
self make() { }

struct Holder
{
    self<i32>* p;
    i32        self;
};

// A type, not a value: neither the object nor something to build.
struct Counter
{
    i32 n;

    i32 get() const { return self.n; }
    static Counter make() { return self { 0 }; }
};

// Nor is it an enum's integer.
enum Small : self
{
    A,
};

// An implementer need not keep a promise its interface cannot make.
struct Cell<T> where T : copyable
{
    T v;
};

interface Wrapped
{
    void take( Cell<self>* h );
    void hold( self<i32>* h );
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "10:21-10:25 error: `P`'s `same` does not match the one `Same` declares = `Same` wants `bool same(const ref P "
            "other) const`",
            "18:21-18:25 error: `Q`'s `same` must be `const`, as `Same` declares it = a `const ref T` with `T : Same` may call "
            "it",
            "26:20-26:24 error: `Same` can only bound a type parameter, because its `same` mentions `self` = take a `T` with "
            "`where T : Same` instead",
            "27:15-27:19 error: `Same` can only bound a type parameter, because its `same` mentions `self` = take a `T` with "
            "`where T : Same` instead",
            "28:17-28:21 error: `Same` can only bound a type parameter, because its `same` mentions `self` = take a `T` with "
            "`where T : Same` instead",
            "33:20-33:21 error: a literal cannot be a `T` = `T` is a type that implements `Same`, so pass a `T`",
            "41:26-41:30 error: `self` is written only inside an `interface` = write `R`, the type's own name",
            "45:1-45:5 error: `self` is written only inside an `interface` = it names the type that implements the interface",
            "49:5-49:14 error: `self` is written only inside an `interface` = write `Holder`, the type's own name",
            "50:16-50:20 error: a declaration may not be named `self` = `self` names the type that implements an interface",
            "58:30-58:34 error: `self` names a type, not a value = the object a method is called on is `this`",
            "59:36-59:40 error: `self` names a type, not a value = build it by the type's own name",
            "63:14-63:18 error: `self` is written only inside an `interface` = it names the type that implements the interface",
            "76:21-76:25 error: `self` does not promise `copyable`, and `Cell` requires it of `T` = an interface cannot "
            "promise `copyable` of its implementers",
            "77:16-77:25 error: `self` takes no type arguments = it already names the type that implements the interface"
        }
    );
}

TEST_CASE( "errors_shadowing", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(i32 shadows_a_local()
{
    i32 x = 1;
    {
        i32 x = 2;
        return x;
    }
}

i32 shadows_a_parameter( i32 p )
{
    i32 p = 1;
    return p;
}

i32 shadows_from_three_levels_out()
{
    i32 deep = 1;
    {
        {
            i32 deep = 2;
            return deep;
        }
    }
}

// A local may not take a method's name either: a bare `size()` after it would call the local.
struct Sized
{
    i32 size() const
    {
        return 1;
    }

    i32 doubled() const
    {
        i32 size = 2;
        return size;
    }

    i32 n;
};
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "5:13-5:14 error: `x` shadows an outer declaration = previous declaration is at: t.kl:3:5",
            "12:9-12:10 error: `p` shadows an outer declaration = previous declaration is at: t.kl:10:26",
            "21:17-21:21 error: `deep` shadows an outer declaration = previous declaration is at: t.kl:18:5",
            "37:13-37:17 error: `size` shadows a method = a local may not take a method's name"
        }
    );
}

TEST_CASE( "errors_size_arguments", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D78: a size argument, `bitset<64>`, is given only where the generic declares a size. A type
// parameter given one is refused, wherever the argument list is written.
struct Box<T> where T : copyable
{
    T v;
};

i32 id<T>( i32 x )
{
    return x;
}

i32 main()
{
    Box<4> b = Box<4> { 1 };
    return id<4>( 1 );
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "15:9-15:10 error: `T` takes a type, but `4` is a size",
            "15:20-15:21 error: `T` takes a type, but `4` is a size",
            "16:15-16:16 error: `T` takes a type, but `4` is a size"
        }
    );
}

TEST_CASE( "errors_size_deduction", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D78: a size parameter is deduced only from an array or an array literal. A run's length and a
// fill's count are known only at run time or from the type, so both leave `N` open.
u64 sum<u64 N>( const ref i32[N] a )
{
    return N + cast<u64>( a[0] );
}

u64 from_run( const ref i32[] r )
{
    return sum( r ); // error
}

i32 main()
{
    u64 a = sum( [ 7, .. ] ); // error
    return cast<i32>( a );
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "10:12-10:15 error: nothing here says what `N` is in `sum` = write the type arguments, as in `sum<4>(...)`",
            "15:13-15:16 error: nothing here says what `N` is in `sum` = write the type arguments, as in `sum<4>(...)`"
        }
    );
}

TEST_CASE( "errors_size_equal_arguments", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Two written sizes with one value: the report points at the one the suffix reads, written here or
// carried through another generic. Each marked line is one error.
u64 second<u64 A, u64 B>() { u8[B - 2] a = [ 0, .. ]; return A; }
u64 split<u64 W, u64 H>() { u8[W / ( H - 2 )] a = [ 0, .. ]; return W; }
u64 third<u64 A, u64 B>() { u8[B - 4] a = [ 0, .. ]; return A; }
u64 carry<u64 X, u64 Y>() { return third<X, Y>(); }
u64 swapped<u64 X, u64 Y>() { return second<Y, X>(); }

struct Two<u64 A, u64 B> { u8[A] x; u8[B - 2] y; };

u64 both( const ref Two<2, 2> t ) { return 0; }    // error: at the second `2`

i32 main()
{
    second<2, 2>();                                 // error: at the second `2`
    split<2, 2>();                                  // error: at the divisor's `2`
    carry<4, 4>();                                  // error: at the second `4`
    swapped<2, 3>();                                // error: at the `2`, which becomes `B`
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "11:28-11:29 error: `Two<2, 2>` makes an array of 0 elements = `B - 2` at t.kl:9:40 is 0 when `B` is 2",
            "15:15-15:16 error: `second<2, 2>` makes an array of 0 elements = `B - 2` at t.kl:3:33 is 0 when `B` is 2",
            "16:14-16:15 error: `split<2, 2>` makes an array whose size divides by 0 = `W / (H - 2)` at t.kl:4:32 divides by 0 "
            "when `W` is 2 and `H` is 2",
            "17:14-17:15 error: `third<4, 4>` makes an array of 0 elements = `B - 4` at t.kl:5:32 is 0 when `B` is 4",
            "18:13-18:14 error: `second<3, 2>` makes an array of 0 elements = `B - 2` at t.kl:3:33 is 0 when `B` is 2"
        }
    );
}

TEST_CASE( "errors_size_param_uses", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// What a size argument, a suffix over `N`, and `N` in a body refuse. Each marked line is one error.
struct Box<T> where T : copyable { T x; };

u64 first<u64 N>( const ref i32[N] a )
{
    return cast<u64>( a[0] );
}

struct Shifted<u64 N> { i32[N << 1] x; };           // error: only + - * / %

void body<u64 N>()
{
    const u64 w = N * 2;                            // a read-only local, not a constant
    i32[N] filled = [ w > 0 ? 1 : 2, .. ];
    i32[N] listed = [ 1, 2 ];                       // error: listed against T[N]
    N = 3;                                          // error: not assignable

    switch( cast<u64>( 1 ) )
    {
        case N:                                     // error: known only per instance
            return;
        default:
            return;
    }
}

i32 main()
{
    i32[2] a = [ 1, 2 ];
    u64    n = 2;

    first<0>( a );                                  // error: at least 1
    first<i32>( a );                                // error: a type
    first<n>( a );                                  // error: a run-time value
    Box<4>* b = nullptr;                            // error: `T` takes a type
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "9:29-9:35 error: an array's size over `N` may use `+`, `-`, `*`, `/` and `%`",
            "15:21-15:29 error: `i32[N]`'s length is known only per instance = fill it with `[x, ..]`",
            "16:5-16:6 error: cannot assign to this expression = `N` is a value, not a place",
            "20:14-20:15 error: `N` is known only per instance = a `switch` decides which arm runs by comparing constants",
            "32:11-32:12 error: `N` is a size, which is at least 1",
            "33:11-33:14 error: `N` takes a size, but `i32` is a type",
            "34:11-34:12 error: `N` takes a size known at compile time = write a literal or a `const`",
            "35:9-35:10 error: `T` takes a type, but `4` is a size"
        }
    );
}

TEST_CASE( "errors_size_params", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A size parameter is a `u64` written before its name. Each declaration below is one error.
struct Point { i32 x; };

struct Signed<i32 N> { i32 x; };

struct Pointed<Point N> { i32 x; };

struct Bounded<T, u64 N> where T : copyable, where N : copyable { T x; };

struct Typed<u64 N> { N x; };

struct Fine<T, u64 N> where T : copyable { T x; };

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "4:15-4:18 error: a size parameter is a `u64` = other types are not supported yet",
            "6:16-6:21 error: a size parameter is a `u64` = write `u64 N`",
            "8:46-8:64 error: `N` is a size; a `where` clause constrains a type",
            "10:23-10:24 error: `N` is a size, not a type"
        }
    );
}

TEST_CASE( "errors_size_recursion", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A size computed from `N` and passed back round a cycle makes a new instance each time, bigger
// or smaller.
void grow<u64 N>( i32[N] a ) { i32[N + 1] b = [ 0, .. ]; grow( b ); }
void shrink<u64 N>( i32[N] a ) { i32[N - 1] b = [ 0, .. ]; shrink( b ); }

i32 main() { return 0; }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "3:58-3:67 error: `grow` is instantiated with a different size each time round = `grow` to `grow`, and each round "
            "computes a size from the last - so no set of instances ever finishes; forward the size parameter itself",
            "4:60-4:71 error: `shrink` is instantiated with a different size each time round = `shrink` to `shrink`, and each "
            "round computes a size from the last - so no set of instances ever finishes; forward the size parameter itself"
        }
    );
}

TEST_CASE( "errors_size_suffixes", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A suffix over a size parameter that is 0, or has no value, at one instance: reported at the size
// the caller wrote, however many generics carried it, with the suffix named in the help.
const u64 One = 1;

struct Ring<u64 N>
{
    u64[N - 1] slots;
};

struct Slab<u64 N>
{
    u8[N - 1] bytes;
};

struct Holder
{
    Ring<One> ring;
};

u64 inner<u64 N>() { Slab<N> s = Slab<N> { [ 0, .. ] }; return N; }
u64 outer<u64 N>() { return inner<N>(); }

u64 huge<u64 N>() { u8[N * 1000000000000] a = [ 0, .. ]; return N; }
u64 below<u64 N>() { u8[N - 3] a = [ 0, .. ]; return N; }
u64 split<u64 W, u64 H>() { u8[W / ( H - 2 )] a = [ 0, .. ]; return W; }
u64 shorter<u64 N>( const ref u64[N] a ) { u8[N - 1] b = [ 0, .. ]; return N; }

i32 main()
{
    outer<1>();
    huge<100000000>();
    below<2>();
    split<4, 2>();
    u64[1] one = [ 5 ];
    shorter( one );
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "17:10-17:13 error: `Ring<1>` makes an array of 0 elements = `N - 1` at t.kl:7:9 is 0 when `N` is 1",
            "30:11-30:12 error: `Slab<1>` makes an array of 0 elements = `N - 1` at t.kl:12:8 is 0 when `N` is 1",
            "31:10-31:19 error: `huge<100000000>` makes an array whose size overflows a `u64` = `N * 1000000000000` at "
            "t.kl:23:24 overflows a `u64` when `N` is 100000000",
            "32:11-32:12 error: `below<2>` makes an array whose size is below 0 = `N - 3` at t.kl:24:25 is below 0 when `N` is "
            "2",
            "33:14-33:15 error: `split<4, 2>` makes an array whose size divides by 0 = `W / (H - 2)` at t.kl:25:32 divides by "
            "0 when `W` is 4 and `H` is 2",
            "35:5-35:19 error: `shorter<1>` makes an array of 0 elements = `N - 1` at t.kl:26:47 is 0 when `N` is 1"
        }
    );
}

TEST_CASE( "errors_size_zero_in_type", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A size written in a type is checked as one written at a call. Each marked line is one error.
struct Bits<u64 N> { u64[N] words = [ 0, .. ]; };

u64 width( const ref Bits<0> b )                    // error: at least 1
{
    return b.words[0];
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE( p.shown() == Lines { "4:27-4:28 error: `N` is a size, which is at least 1" } );
}

TEST_CASE( "errors_statement_recovery", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A statement that reported is skipped to its `;`: what is left of it is not parsed again as a
// statement of its own, so nothing is reported about the fragment.
struct Point
{
    i32 x;
    i32 y;
};

i32 trailing_this()
{
    return 1 this;
}

i32 trailing_type( i32 total )
{
    total = total + 1 Point;
    return total;
}

i32 stray_parenthesis( i32 x )
{
    x = ( 1 2 );
    return x;
}

// *No effect* is decided once the statement has ended, so a missing operator is one error.
i32 missing_operator( i32 a, i32 b, i32 c )
{
    a + b c;
    return a;
}

// An argument list broken across lines is one error.
i32 negate( i32 n )
{
    return -n;
}

i32 broken_across_lines( i32 x )
{
    x = negate( 1
                2 );
    return x;
}

// A `;` missing at the end of a line is one error, and the next line is still a statement.
i32 missing_semicolon( i32 x )
{
    x = 1
    return x;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "11:13-11:13 error: expected `;`, found `this`",
            "16:22-16:22 error: expected `;`, found `Point`",
            "22:12-22:12 error: expected `)`, found `2`",
            "29:10-29:10 error: expected `;`, found `c`",
            "41:18-41:18 error: expected `,` or `)`, found `2`",
            "49:10-49:10 error: expected `;`, found `return`"
        }
    );
}

TEST_CASE( "errors_static_fields", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A static field is one variable for its whole type, so it is named through the type, takes a
// constant initialiser as a file-scope variable does, and is refused where it is not built yet.

i32 one()
{
    return 1;
}

struct Point
{
    i32 x;
};

class Counter
{
    static i32 made = 0;

    public static i32 limit = 10;

    public static const i32 cap = 3;

    // Runs, so it is not a constant.
    static i32 computed = one();

    // A struct at file scope needs its literal lowered somewhere, and there is nowhere yet.
    static Point origin;

    // Nor a class, for the same reason.
    static str label;

    // Shares the member namespace with the field below.
    static i32 id;

    i32 id;
};

// One variable per instantiation, which a type parameter would make a different type in each.
struct Box<T> where T : copyable
{
    T v;

    static i32 made = 0;

    static T last;
};

i32 through_an_object( const ref Counter c )
{
    return c.limit;
}

i32 main()
{
    i32 b = Counter::made;       // private
    i32 d = Counter::nope;       // nothing of that name
    Counter::cap = 4;            // const
    i32 e = Box::made;           // which instantiation
    i32 f = Counter<i32>::limit; // not generic
    i32 g = Box<i32>::made;      // accepted

    return Counter::limit; // accepted
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "26:12-26:25 error: a static field of `Point` needs an initialiser = its field `x` has no default",
            "29:12-29:22 error: a `str` static field needs a value = write `= \"\"` for an empty one",
            "34:9-34:11 error: field `id` is already declared = previous declaration is at: t.kl:32:12",
            "44:12-44:19 error: a static field's type cannot name a type parameter yet",
            "49:12-49:19 error: `limit` is static, so it belongs to `Counter` rather than to one object = write "
            "`Counter::limit`",
            "54:13-54:26 error: `made` is private to `Counter` = only that type's own members may name it",
            "55:13-55:26 error: `Counter` has no static field `nope`",
            "56:5-56:17 error: `cap` is `const` = remove `const` to modify it",
            "57:13-57:16 error: `Box` needs its type arguments here = write `Box< ... >::made`",
            "58:13-58:32 error: `Counter` is not a generic"
        }
    );
}

TEST_CASE( "errors_static_method_overloads", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A static method overloads as any method does, and `T::f( ... )` chooses among the set by its
// arguments, with the refusals a free call gets. Its candidates list only the written parameters,
// since a static method has no receiver to leave out. A set may hold static and instance methods
// at once; the one chosen decides which spelling may call it.

struct Number
{
    i32 x;

    static i32 from( i64 value )
    {
        return 1;
    }

    static i32 from( u64 value )
    {
        return 2;
    }

    static i32 mixed( i64 value )
    {
        return 3;
    }

    i32 mixed( u64 value ) const
    {
        return x;
    }

    // A bare call chooses among its siblings by the same rules.
    static i32 bare_ambiguous()
    {
        return from( 0.5 );
    }
};

// A float literal reaches neither integer.
i32 ambiguous()
{
    return Number::from( 0.5 );
}

i32 matches_none()
{
    return Number::from( true );
}

i32 wrong_count()
{
    return Number::from( 1, 2 );
}

// The instance overload is the match, and the type cannot supply its receiver.
i32 chooses_an_instance_method()
{
    u64 n = 4;
    return Number::mixed( n );
}

// Everything below is accepted.

struct Accepted
{
    i32 x;

    static i32 from( i64 value )
    {
        return 1;
    }

    static i32 from( u64 value )
    {
        return 2;
    }

    static i32 from_both()
    {
        i64 a = 1;
        u64 b = 2;
        return from( a ) + from( b );
    }

    i32 from_an_instance() const
    {
        u64 b = 2;
        return from( b ) + x;
    }
};

i32 accepted()
{
    i64 a = 1;
    u64 b = 2;
    i8  small = 3;
    return Accepted::from( a ) + Accepted::from( b ) + Accepted::from( small ) + Accepted::from_both();
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "33:16-33:27 error: no `from` matches these arguments = the ones declared take `(i64)` and `(u64)`",
            "40:12-40:31 error: no `from` matches these arguments = the ones declared take `(i64)` and `(u64)`",
            "45:12-45:32 error: no `from` matches these arguments = the ones declared take `(i64)` and `(u64)`",
            "50:12-50:32 error: no `from` takes 2 arguments = the ones declared take `(i64)` and `(u64)`",
            "57:12-57:25 error: `mixed` needs an object = call it as `value.mixed(...)`"
        }
    );
}

TEST_CASE( "errors_static_methods", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN §6.7. A static method belongs to a type and takes no receiver, so every route that
// reaches an object implicitly is closed to it - and the two call spellings stop being
// interchangeable in both directions. Each refusal below is a diagnostic rather than a miscompile:
// without them the lowerer adopts the first written parameter as `this` and projects a field off it.

struct P
{
    i32 x;

    static P make( i32 v )
    {
        return P { v };
    }

    i32 get() const
    {
        return x;
    }

    // A bare field name is `this.field` written implicitly, and there is no `this` to write it
    // through.
    static i32 reads_a_field()
    {
        return x;
    }

    // The same thing said out loud, which must not be the one spelling that survives.
    static i32 names_the_receiver()
    {
        return this.x;
    }

    // The member scope puts a sibling in reach by bare name. A static method joins it, but only the
    // half of it that needs no object.
    static i32 calls_an_instance_sibling()
    {
        return get();
    }
};

// A trailing `const` binds the receiver, so on a member with none it marks nothing. Refused rather
// than ignored: accepting it silently teaches the habit of writing it.
struct Marked
{
    i32 x;

    static i32 one() const
    {
        return 1;
    }
};

// A constructor and a destructor are defined by what they do to an object, so neither has a
// receiver to remove. A `class` rather than a `struct`, so this says one thing: D29 already refuses
// a constructor on a struct, and the two rules firing together would hide which one is under test.
class Ctor
{
    i32 x;

    static Ctor( i32 v )
    {
        x = v;
    }

    static ~Ctor()
    {
    }
};

// A static field is one variable for the whole type, accepted since M8. It once parsed away into an
// ordinary field, so every object carried its own copy of what was written to be shared - a
// miscompile with no diagnostic. `sema/errors_static_fields` holds its refusals.
class Shared
{
    static i32 count;

    i32 x;
};

// A static and an instance method of one name take the same written parameters and mangle to one
// symbol: the tag names the enclosing type and the receiver is among the parameters in neither. No
// call site is ambiguous, but `cc` refuses two definitions of one name - a failure in generated code
// rather than a diagnostic, which is the worst class this project has.
struct Clash
{
    i32 x;

    static i32 f( i32 a )
    {
        return a;
    }

    i32 f( i32 a ) const
    {
        return a + x;
    }
};

// The call spellings are not interchangeable in either direction.
i32 calls_a_static_through_an_object()
{
    P p = P::make( 1 );

    P q = p.make( 2 );

    return q.x;
}

i32 calls_an_instance_method_through_the_type()
{
    return P::get();
}

i32 reaches_a_field_through_the_type()
{
    return P::x;
}

i32 names_something_the_type_does_not_have()
{
    P p = P::nope();

    return p.x;
}

// The qualifier may now be a struct or a class, and that is the whole of what widened. A primitive
// has no members of any kind, and an expression is not a name at all.
i32 qualifies_a_primitive()
{
    return i32::make( 1 );
}

i32 free_function()
{
    return 1;
}

i32 qualifies_an_expression()
{
    return free_function()::x;
}

// `::` reaching a variant is what it meant before M7, and an enum still has no static methods for it
// to find instead.
enum Colour
{
    Red,
    Green
};

i32 qualifies_an_enum_with_a_name_it_lacks()
{
    return Colour::make( 1 );
}

// Everything below is accepted, and the fixture would be worth much less without it.

struct Good
{
    i32 x;

    static i32 one()
    {
        return 1;
    }

    // A static sibling needs no object, so a bare call to one is as legal here as between free
    // functions - and as legal from an instance method, which has a receiver it simply does not use.
    static i32 two()
    {
        return one() + one();
    }

    i32 plus_one() const
    {
        return one() + x;
    }
};

i32 accepted()
{
    Good g = Good { 1 };

    Colour c = Colour::Red;

    if( c == Colour::Green )
    {
        return 0;
    }

    return Good::two() + g.plus_one();
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "24:16-24:17 error: `x` is a field, and there is no object here to read it from = take one as a parameter, or make "
            "this a method",
            "30:16-30:20 error: `this` is not declared",
            "37:16-37:19 error: `get` is a method, and there is no object here to call it on = a method can only be called by "
            "bare name from inside another method of the same type",
            "47:22-47:27 error: a `static` method has no object, so it cannot be `const`",
            "60:5-60:11 error: a constructor cannot be `static`",
            "65:5-65:11 error: a destructor cannot be `static`",
            "93:9-93:10 error: `f` is already declared with these parameters = a `static` method and a method of one name must "
            "differ in their parameters",
            "104:11-104:17 error: `make` is a `static` method of `P`, so it takes no object = call it as `P::make(...)`",
            "111:12-111:18 error: `get` needs an object = call it as `value.get(...)`",
            "116:12-116:16 error: `P` has no static field `x` = take its offset as `&P::x`, or read it as `value.x`",
            "121:11-121:18 error: `P` has no static method `nope`",
            "130:12-130:21 error: `i32` has no member `make` = a number type has only `i32::max` and `i32::min`",
            "140:12-140:27 error: `::` needs the name of a type on its left",
            "153:12-153:24 error: `Colour` has no variant `make`"
        }
    );
}

TEST_CASE( "errors_static_requirements", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// M9.5: what a `static` requirement refuses. Each refusal is named in the comment above it.
interface counted
{
    static u64 count();
};

interface tallied implements counted
{
    u64 tally() const;
};

interface Shape
{
    f64 area() const;
};

// `count` must be `static`, as `counted` declares it.
struct instance_count implements counted
{
    i32 v;

    u64 count() { return 0; }
};

// `absent` has no `count`, which it must declare `static`.
struct absent implements counted
{
    i32 v;
};

// `count` is private, so it cannot answer `counted`.
class hidden implements counted
{
    i32 v;

    private static u64 count() { return 0; }
};

// `counted` is a bound only, because its `count` is `static`.
void by_ref( ref counted c ) { }

// The same, through a pointer.
void by_pointer( counted* c ) { }

// `tallied` is a bound only too, because it requires `counted`.
void refined( const ref tallied t ) { }

// A `static` requirement takes no object.
u64 on_object<T>( const ref T t ) where T : counted
{
    return t.count();
}

// `T` has no `where` clause to give it `count`.
u64 unbound<T>()
{
    return T::count();
}

// `counted` has no `size`.
u64 missing<T>() where T : counted
{
    return T::size();
}

// `area` needs an object, which `T::` does not supply.
f64 no_object<T>() where T : Shape
{
    return T::area();
}

implements counted for i32
{
    static u64 count() { return 0; }
};

// A block's `count` is reached only through a bound.
u64 direct()
{
    return i32::count();
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "18:34-18:41 error: `instance_count`'s `count` must be `static`, as `counted` declares it = it is called as "
            "`instance_count::count(...)`, with no object",
            "26:26-26:33 error: `absent` implements `counted` but has no method `count` = add `static u64 count()` to `absent`",
            "32:25-32:32 error: `hidden`'s `count` is private, but `counted` makes it callable by anyone = make `count` public",
            "40:18-40:25 error: `counted` can only bound a type parameter, because its `count` is `static` = take a `T` with "
            "`where T : counted` instead",
            "43:18-43:25 error: `counted` can only bound a type parameter, because its `count` is `static` = take a `T` with "
            "`where T : counted` instead",
            "46:25-46:32 error: `tallied` can only bound a type parameter, because it requires `counted`, whose `count` is "
            "`static` = take a `T` with `where T : tallied` instead",
            "51:12-51:19 error: `count` is a `static` method of `T`, so it takes no object = call it as `T::count(...)`",
            "57:12-57:20 error: `T` has no static method `count` = a `where` clause naming an interface gives `T` that "
            "interface's methods",
            "63:12-63:19 error: `T` has no static method `size`",
            "69:12-69:19 error: `area` needs an object = call it as `value.area(...)`",
            "80:12-80:22 error: `i32` has no member `count` = a number type has only `i32::max` and `i32::min`"
        }
    );
}

TEST_CASE( "errors_str", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A string literal is the prelude's `str`: read-only, made only by a literal, and compared with
// nothing yet. One refusal per function, so none of them stops another being checked.
str global = "file scope";

i32 not_a_number()
{
    i32 x = "abc";
    return x;
}

void size_is_read_only()
{
    str s  = "abc";
    s.size = 1;
}

void bytes_are_read_only()
{
    str s = "abc";

    unsafe
    {
        s.data[ 0 ] = 1;
    }
}

u64 only_a_literal_makes_one()
{
    str s = "abc";
    str t = str( s.data, s.size );
    return t.size;
}

u64 no_address()
{
    const str* p = &"abc";
    return p.size;
}

bool no_equality()
{
    return "a" == "b";
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "7:13-7:18 error: expected `i32`, but got `str`",
            "14:5-14:11 error: `size` is a `const` field, so it cannot be modified = only a constructor assigns it",
            "23:9-23:20 error: this is reached through a `const u8[*]`, so it cannot be modified = a pointer to `const` only "
            "reads what it points at",
            "30:13-30:16 error: `str`'s constructor is private = only a string literal makes one",
            "36:20-36:26 error: cannot take the address of this expression = it does not name a variable",
            "42:12-42:22 error: `str` has no `operator==`"
        }
    );
}

TEST_CASE( "errors_str_globals", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A `str` global takes a string literal or a `const str`'s name, and nothing else.
str       none;
str       number = 5;
const str SELF   = SELF;
str       joined = "a" + "b";

class Holder
{
    public static str kept;

    public i32 n;
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "2:1-2:16 error: a `str` file-scope variable needs a value = write `= \"\"` for an empty one",
            "3:20-3:21 error: expected `str`, but got an integer literal",
            "4:20-4:24 error: `SELF`'s initialiser depends on `SELF` itself = a `const` cannot be defined in terms of itself",
            "5:20-5:29 error: expected `str`, but got `joined_text`",
            "9:19-9:28 error: a `str` static field needs a value = write `= \"\"` for an empty one"
        }
    );
}

TEST_CASE( "errors_str_shadowed", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A program's own `str` is a different type from a literal's, though both read `str`.
struct str
{
    i32 n;
};

i32 main()
{
    str mine = "abc";
    return mine.n;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() == Lines { "9:16-9:21 error: expected `str`, but got `str` = two different types are both named `str`" }
    );
}

TEST_CASE( "errors_string_literal_adopts", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// M9.5: what a string literal does not adopt. Each refusal is named in the comment above it.
class label implements from<str>
{
    u64 length;

    public label( u64 n ) { length = n; }
    public static label from( move str text ) { return label( text.size ); }
};

struct width implements from<str>
{
    u64 n;

    static width from( move str text ) { return width { text.size }; }
};

struct plain
{
    u64 n;
};

struct number implements from<i64>
{
    i64 v;

    static number from( move i64 value ) { return number { value }; }
};

void both( label l ) { }
void both( width w ) { }

void edit( ref label l ) { }

u64 show<T>( T value ) where T : from<str>
{
    return 0;
}

void unbounded<T>()
{
    T x = "abc";
}

i32 main()
{
    // `plain` does not implement `from<str>`.
    plain p = "abc";

    // Only a literal adopts: a `str` value does not.
    str text = "abc";
    label l = text;

    // `from<i64>` lets no literal adopt `number`.
    number n = 5;

    // Two overloads each need adoption, and neither is closer.
    both( "x" );

    // A `ref` parameter cannot take the temporary a literal becomes.
    edit( "x" );

    // `T` is deduced from the literal as `str`, which does not implement `from<str>`.
    _ = show( "abc" );

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "41:11-41:16 error: expected `T`, but got `str` = `where T : from<str>` lets a string literal become one",
            "47:15-47:20 error: expected `plain`, but got `str` = a string literal becomes a `plain` only if `plain` "
            "implements the prelude's `from<str>`",
            "51:15-51:19 error: expected `label`, but got `str` = only a string literal becomes a `label`; write "
            "`label::from(...)`",
            "54:16-54:17 error: expected `number`, but got an integer literal",
            "57:5-57:16 error: this call to `both` is ambiguous = more than one matches: `(label)` and `(width)`; write a type "
            "the call can be told by",
            "60:11-60:14 error: `edit` may modify this argument = write `ref`",
            "63:15-63:20 error: `show` needs a `T` that implements `from<str>`, but `str` does not"
        }
    );
}

TEST_CASE( "errors_string_literal_own_from", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// M9.5: only the prelude's `from<str>` lets a string literal adopt a type. A program's own `from`
// shadows the prelude's name and changes nothing about `"abc"`.
interface from<T>
{
    static self from( T value );
};

struct mine implements from<str>
{
    u64 n;

    static mine from( str text ) { return mine { text.size }; }
};

i32 main()
{
    // `mine` implements this program's `from<str>`, not the prelude's.
    mine m = "abc";

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() == Lines { "18:14-18:19 error: expected `mine`, but got `str` = a string literal becomes a `mine` only if "
                             "`mine` implements the prelude's `from<str>`" }
    );
}

TEST_CASE( "errors_struct_private_member", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A struct's refused `private` is reported once; the member stays public after it.
struct Pt { i32 x; private i32 y; };

i32 main()
{
    Pt p = Pt { 1, 2 };
    return p.y;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() == Lines { "2:20-2:27 error: a `struct` has no private members = write `class` instead, which is the kind "
                             "that hides what it holds" }
    );
}

TEST_CASE( "errors_switch", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN D7/D30. Exhaustiveness is the whole reason `switch` is worth having over a chain of `if`s:
// adding a variant later breaks exactly the switches that should be revisited. Which is also why a
// `default` on a fully covered enum is refused - it would absorb the new variant silently and take
// the guarantee away.
enum Colour
{
    Red,
    Green,
    Blue
};

i32 leaves_a_gap( Colour c )
{
    switch( c )
    {
        case Colour::Red:
            return 1;
    }

    return 0;
}

i32 covers_twice( Colour c )
{
    switch( c )
    {
        case Colour::Red:
            return 1;
        case Colour::Red:
            return 2;
        default:
            return 0;
    }
}

i32 has_a_dead_default( Colour c )
{
    switch( c )
    {
        case Colour::Red:
            return 1;
        case Colour::Green:
            return 2;
        case Colour::Blue:
            return 3;
        default:
            return 0;
    }
}

// An enum or a number. A `bool` has two values and an `if`, so a switch over one buys nothing.
i32 switches_on_a_bool( bool b )
{
    switch( b )
    {
        default:
            return 1;
    }
}

// D34. Over a number, coverage is a set of half-open intervals: overlap is decidable and reported,
// completeness is not, so `default` is required rather than inferred.
i32 omits_the_default( i32 n )
{
    switch( n )
    {
        case 1:
            return 1;
    }

    return 0;
}

i32 runs_backwards( i32 n )
{
    switch( n )
    {
        case 5..1:
            return 1;
        default:
            return 0;
    }
}

// Half-open, so a range whose bounds are equal excludes everything.
i32 covers_nothing( i32 n )
{
    switch( n )
    {
        case 3..3:
            return 1;
        default:
            return 0;
    }
}

i32 overlaps_a_range( i32 n )
{
    switch( n )
    {
        case 1..10:
            return 1;
        case 5:
            return 2;
        default:
            return 0;
    }
}

i32 uses_a_variable( i32 n, i32 m )
{
    switch( n )
    {
        case m:
            return 1;
        default:
            return 0;
    }
}

// A range of variants would need declaration order to be an ordering, which enum comparison already
// refuses to treat it as.
i32 ranges_over_variants( Colour c )
{
    switch( c )
    {
        case Colour::Red..Colour::Green:
            return 1;
        default:
            return 0;
    }
}

// Exact equality against a float is the mistake every float guide opens with, and a `case` is the
// one place it would look deliberate.
i32 matches_a_float_exactly( f64 x )
{
    switch( x )
    {
        case 3:
            return 1;
        default:
            return 0;
    }
}

i32 labels_the_wrong_type( Colour c )
{
    switch( c )
    {
        case 1:
            return 1;
        default:
            return 0;
    }
}

// Everything below is accepted, and the fixture would be worth much less without it.

i32 covers_everything( Colour c )
{
    switch( c )
    {
        case Colour::Red:
            return 1;
        case Colour::Green:
            return 2;
        case Colour::Blue:
            return 3;
    }
}

// Stacked labels are one arm, and each still counts towards coverage.
i32 stacks( Colour c )
{
    switch( c )
    {
        case Colour::Red:
        case Colour::Green:
            return 1;
        case Colour::Blue:
            return 2;
    }
}

i32 fills_the_gap( Colour c )
{
    switch( c )
    {
        case Colour::Red:
            return 1;
        default:
            return 0;
    }
}

// A range over a float is fine, and is the reason bounds are integers rather than the scrutinee's
// type: `..` means one thing everywhere, and the meaning slicing will need is an integer one.
i32 buckets_a_float( f64 x )
{
    switch( x )
    {
        case 0..1:
            return 1;
        case 1..10:
            return 2;
        default:
            return 3;
    }
}

i32 buckets_an_integer( i32 n )
{
    switch( n )
    {
        case 1..5:
        case 20:
            return 1;
        default:
            return 0;
    }
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "14:5-18:6 error: `switch` does not cover every variant of `Colour` = missing `Green` and `Blue`",
            "29:14-29:25 error: `Red` is already covered = previous declaration is at: t.kl:27:14",
            "38:5-48:6 error: every variant of `Colour` is covered, so `default` can never run = remove it, so that adding a "
            "variant later is a compile error here",
            "54:13-54:14 error: `switch` needs an `enum` or a number, but this is a `bool`",
            "65:5-69:6 error: a `switch` over `i32` needs a `default` = its values cannot all be listed, so there is no other "
            "way to be exhaustive",
            "78:14-78:18 error: `5..1` matches nothing = a range runs upwards; the lower bound comes first",
            "90:14-90:18 error: `3..3` matches nothing = a range excludes its upper bound, so this one is empty",
            "103:14-103:15 error: this `case` overlaps an earlier one = previous declaration is at: t.kl:101:14",
            "114:14-114:15 error: a `case` label must be known at compile time = a `switch` decides which arm runs by "
            "comparing constants",
            "127:14-127:40 error: a range cannot name variants = the order variants were declared in is not an ordering; list "
            "them instead",
            "140:14-140:15 error: a single value cannot be matched against a `f64` = comparing floats for equality is rarely "
            "what is meant; write a range, as in `case 1..2:`",
            "151:14-151:15 error: a `case` label must be a variant of `Colour`"
        }
    );
}

TEST_CASE( "errors_switch_arm_breaks_out", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// An arm still runs on when its last `switch` or `while( true )` can be left by a `break`.
enum Shape
{
    Circle,
    Square,
};

i32 nested( Shape a, Shape b )
{
    switch( a )
    {
        case Shape::Circle:
            switch( b )
            {
                case Shape::Circle: return 1;
                case Shape::Square:
                    if( a == Shape::Circle )
                    {
                        break;
                    }
                    return 2;
            }
        case Shape::Square: return 3;
    }
    return 0;
}

i32 spins( i32 n )
{
    switch( n )
    {
        case 0:
            while( true )
            {
                if( n == 5 )
                {
                    break;
                }
                n = n + 1;
            }
        default: return -1;
    }
}

i32 inner_loop( i32 n )
{
    // Accepted: the `break` leaves only the inner loop, so the outer one never ends.
    switch( n )
    {
        case 0:
            while( true )
            {
                while( true )
                {
                    break;
                }
            }
        default: return -1;
    }
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "12:9-22:14 error: a `case` with a body must say how it ends = end it with `break;` or `return;`, or "
            "`fallthrough;` to run on into the next `case`",
            "32:9-40:14 error: a `case` with a body must say how it ends = end it with `break;` or `return;`, or "
            "`fallthrough;` to run on into the next `case`"
        }
    );
}

TEST_CASE( "errors_switch_arms", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D7's three rules about an arm's shape. Each of these compiled before, and two of them meant
// something other than they appear to.
enum Shape { Circle( i32 r ), Rect( i32 w, i32 h ), Dot };

enum E { A, B, C };

i32 runs_on( E e )
{
    i32 t = 0;

    switch( e )
    {
        case E::A:
            t = 1;
        case E::B:
        case E::C:
            t = 2;
    }

    return t;
}

i32 stacks_a_binding( Shape s )
{
    switch( s )
    {
        case Shape::Circle( r ):
        case Shape::Rect( w, h ):
            return 1;
        case Shape::Dot:
            return 0;
    }
}

i32 falls_into_a_binding( Shape s )
{
    switch( s )
    {
        case Shape::Circle:
            fallthrough;
        case Shape::Rect( w, h ):
            return w;
        case Shape::Dot:
            return 0;
    }
}

void falls_from_the_last( E e )
{
    switch( e )
    {
        case E::A:
            break;
        case E::B:
        case E::C:
            fallthrough;
    }
}

void falls_from_nowhere()
{
    fallthrough;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "13:9-14:19 error: a `case` with a body must say how it ends = end it with `break;` or `return;`, or "
            "`fallthrough;` to run on into the next `case`",
            "27:9-29:22 error: a `case` that destructures cannot share its body with another = give it an arm of its own, or "
            "drop the `(...)` to match the variant without its payload",
            "40:13-40:25 error: `fallthrough` into a `case` that destructures = the next arm binds names from its own variant, "
            "and this value is not one",
            "56:13-56:25 error: `fallthrough` in the last `case` = there is no arm after this one to fall into",
            "62:5-62:17 error: `fallthrough` must be the last statement of a `case` = it cannot be nested inside a block, a "
            "loop or an `if`"
        }
    );
}

TEST_CASE( "errors_switch_bare_variant", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A label naming a variant without its enum is refused once, with the spelling it meant, and then
// read as that variant: the switch is covered and the pattern's binding is typed, so nothing after
// it is reported. A name that is no variant of the scrutinee is refused without a hint.
enum Colour : u8
{
    Red,
    Green,
};

enum Shape
{
    Circle( f64 radius ),
    Dot,
};

i32 pick( Colour c )
{
    switch( c )
    {
        case Red:
            return 1;
        case Colour::Green:
            return 2;
    }
}

f64 area( Shape s )
{
    switch( s )
    {
        case Circle( r ):
            return r * r;
        case Dot:
            return 0.0;
    }
}

i32 other( Colour c )
{
    switch( c )
    {
        case Purple:
            return 1;
        default:
            return 0;
    }
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "20:14-20:17 error: `Red` is not declared = write `Colour::Red`",
            "31:14-31:20 error: `Circle` is not declared = write `Shape::Circle`",
            "33:14-33:17 error: `Dot` is not declared = write `Shape::Dot`",
            "42:14-42:20 error: `Purple` is not declared"
        }
    );
}

TEST_CASE( "errors_switch_float_bound", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A fractional `case` bound over a float is refused once per label, as a bound that is not an
// integer - a literal, a mix with an integer, and a float `const`. A bound that is not a constant
// is still "not known at compile time".

const f64 K = 0.5;

i32 f( f64 x, f64 y )
{
    switch( x )
    {
        case 0.5..1.5:
            return 1;
        case 2..2.5:
            return 2;
        case K..9:
            return 3;
        case 10..y:
            return 4;
        default:
            return 0;
    }
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "11:14-11:22 error: a `case` bound is an integer = `case 0..2:` over a `f64` compares against 0.0 and 2.0",
            "13:14-13:20 error: a `case` bound is an integer = `case 0..2:` over a `f64` compares against 0.0 and 2.0",
            "15:14-15:18 error: a `case` bound is an integer = `case 0..2:` over a `f64` compares against 0.0 and 2.0",
            "17:18-17:19 error: a `case` label must be known at compile time = a `switch` decides which arm runs by comparing "
            "constants"
        }
    );
}

TEST_CASE( "errors_switch_label_type", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A label of the wrong type is reported once, by its type check, not again as "not known at compile
// time" - a string, a variant, a fraction, and a range bound of the wrong type.

enum Colour : u8
{
    Red,
    Green,
};

i32 f( i32 n )
{
    switch( n )
    {
        case "one":
            return 1;
        case Colour::Red:
            return 2;
        case 1.5:
            return 3;
        case 1..Colour::Green:
            return 4;
        default:
            return 0;
    }
}

i32 main()
{
    return f( 1 );
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "14:14-14:19 error: expected `i32`, but got `str`",
            "16:14-16:25 error: expected `i32`, but got `Colour`",
            "18:14-18:17 error: expected `i32`, but got a floating-point literal = a fractional value cannot be an integer",
            "20:17-20:30 error: expected `i32`, but got `Colour`"
        }
    );
}

TEST_CASE( "errors_switch_nested_pattern", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A pattern does not nest. Each nested field is refused once, its names are declared so their uses
// stay quiet, and the outer variant is still covered once, so nothing after it is reported.
enum Inner
{
    Both( i32 a, i32 b ),
    Empty,
};

enum Outer
{
    Wrap( Inner inner ),
    Pair( Inner first, i32 count ),
    Nothing,
};

i32 sum( Outer o )
{
    switch( o )
    {
        case Outer::Wrap( Inner::Both( a, b ) ):
            return a + b;
        case Outer::Wrap( Inner::Empty ):
            return 1;
        case Outer::Pair( Inner::Both( x, _ ), n ):
            return x + n;
        case Outer::Nothing:
            return 0;
    }
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "20:27-20:46 error: a pattern does not nest = bind it, and `switch` on the binding inside this arm",
            "22:27-22:39 error: a pattern does not nest = bind it, and `switch` on the binding inside this arm",
            "24:27-24:46 error: a pattern does not nest = bind it, and `switch` on the binding inside this arm"
        }
    );
}

TEST_CASE( "errors_switch_range_past_top", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A range may end one past its type's top, but no further, and that end still counts for overlap.
i32 too_far( u8 n )
{
    switch( n )
    {
        case 200..257: return 1;
        default: return 0;
    }
}

i32 overlap( u8 n )
{
    switch( n )
    {
        case 255: return 2;
        case 200..256: return 1;
        default: return 0;
    }
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "6:19-6:22 error: `257` does not fit in `u8`",
            "16:14-16:22 error: this `case` overlaps an earlier one = previous declaration is at: t.kl:15:14"
        }
    );
}

TEST_CASE( "errors_switch_unset_scrutinee", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// An unset scrutinee is reported under the scrutinee, not under the first label that tests it.
enum Colour : u8
{
    Red,
    Green,
};

i32 of_enum()
{
    Colour c;
    switch( c )
    {
        case Colour::Red: return 1;
        default: return 0;
    }
}

i32 of_number()
{
    i32 n;
    switch( n )
    {
        case 1: return 1;
        default: return 0;
    }
}

i32 main()
{
    return of_enum() + of_number();
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "11:13-11:14 error: `c` is used before it is initialised = give it a value at its declaration",
            "21:13-21:14 error: `n` is used before it is initialised = give it a value at its declaration"
        }
    );
}

TEST_CASE( "errors_temporary_writes", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// PLAN §6.7. A projection off a value with no storage of its own is a place that can be read and
// not written: the write lands in a temporary that is discarded on the next line, so it provably
// does nothing. Three things produce one - a call returning by value, a conditional, and a struct
// literal - and the conditional inherited it when `?:` shipped, since a conditional's result gets a
// temporary's place even when both arms are places.
//
// The struct literal is not a silent acceptance but an abort: it passed sema and then asserted in
// the lowerer, which had no place to project from. Refusing it here is what removes the crash.

struct P
{
    i32 t;

    void set( i32 v )
    {
        t = v;
    }

    i32 bump()
    {
        t += 1;
        return t;
    }
};

P by_value()
{
    return P { 1 };
}

i32 main()
{
    bool c = true;
    P    a = P { 1 };
    P    b = P { 2 };

    // The call's result has no scope of its own, so each of these writes something nothing can read.
    by_value().t = 1;
    by_value().t++;

    // A method that writes its receiver is the same write, asked at the receiver rather than at an
    // assignment - so it reaches the rule from the method call instead.
    by_value().set( 3 );

    // Its answer dropped, a method returning a value is the same. Used, it is not refused.
    by_value().bump();
    i32 n = by_value().bump();

    // A mutable borrow of a temporary is refused for the stronger reason: it would outlive it.
    ref i32 r = by_value().t;

    // Both arms are places and the result still is not - the conditional materialises a value.
    ( c ? a : b ).t = 4;

    // Reached the lowerer and aborted before this rule existed.
    P { 5 }.t = 6;

    // Legal: reading through a temporary is an ordinary field read, and refusing it would take the
    // conditional's own tests with it.
    i32 v = by_value().t;

    if( ( c ? a : b ).t == 1 )
    {
        return v + n;
    }

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "38:5-38:15 error: this value is a temporary, so writing to it has no effect = assign it to a variable and modify "
            "that",
            "39:5-39:15 error: this value is a temporary, so writing to it has no effect = assign it to a variable and modify "
            "that",
            "43:5-43:15 error: this value is a temporary, so writing to it has no effect = assign it to a variable and modify "
            "that",
            "46:5-46:15 error: this value is a temporary, so writing to it has no effect = assign it to a variable and modify "
            "that",
            "50:17-50:27 error: a `ref` binding cannot borrow from a temporary = it ends with this statement; give it a "
            "variable of its own first",
            "53:5-53:18 error: this value is a temporary, so writing to it has no effect = assign it to a variable and modify "
            "that",
            "56:5-56:12 error: this value is a temporary, so writing to it has no effect = assign it to a variable and modify "
            "that"
        }
    );
}

TEST_CASE( "errors_this_loans", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D54: `this` is a place like any other, so a loan from a field is kept, and broken by a non-`const`
// method of `this`, whether `this` is written or implied.
class buffer
{
    buffer()
    {
        unsafe { data = alloc<i32>( 4 ); data[0] = 1; }
    }

    ~buffer()
    {
        unsafe { free( data ); }
    }

    public ref i32 operator[]( u64 index ) const
    {
        unsafe { return data[index]; }
    }

    public void grow()
    {
        unsafe { free( data ); data = alloc<i32>( 4 ); data[0] = 2; }
    }

    private i32[*] data;
};

class M
{
    public buffer inner;

    M()
    {
        inner = buffer();
    }

    void reset()
    {
        inner.grow();
    }

    i32 implicit_this()
    {
        ref i32 x = inner[0];
        reset();
        return x;
    }

    i32 explicit_this()
    {
        ref i32 x = this.inner[0];
        this.reset();
        return x;
    }

    // Accepted: nothing changes `this` while `r` is live.
    i32 field_of_this()
    {
        const ref buffer r = this.inner;
        return r[0];
    }
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "45:9-45:16 error: this could change `this` while `x` still borrows from it = `x` is used after this; it was bound "
            "at 44:9",
            "52:9-52:13 error: this could change `this` while `x` still borrows from it = `x` is used after this; it was bound "
            "at 51:9"
        }
    );
}

TEST_CASE( "errors_tilde_literal", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A `~` over a literal adopts only an integer context, and its operand is range-checked there.
i32 main()
{
    f32 f = ~0;
    u8  b = ~256;
    f64 g = ~1.5;
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "4:13-4:15 error: no operator `~` for `f32` = the operand must be an integer",
            "5:14-5:17 error: `256` does not fit in `u8`",
            "6:13-6:17 error: no operator `~` for `f64` = the operand must be an integer"
        }
    );
}

TEST_CASE( "errors_trig", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Each `@trig_` function below is not `f64( f64 )`, one error apiece at its directive.

@trig_sin
f32 narrow( f32 x ) { return x; }

@trig_cos
f64 two( f64 x, f64 y ) { return x + y; }

@trig_tan
f64 by_reference( const ref f64 x ) { return x; }

@trig_sin
f64 none() { return 0.0; }

struct Angle
{
    f64 radians;

    @trig_cos
    f64 cosine() const { return radians; }
};

@real_arithmetic
@trig_sin
f64 accepted( f64 x ) { return x; }

i32 main() { return 0; }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "3:1-3:10 error: `narrow` is `f32( f32 )`, but a `@trig_` function is `f64( f64 )` = it takes one `f64` by value "
            "and returns an `f64`",
            "6:1-6:10 error: `two` is `f64( f64, f64 )`, but a `@trig_` function is `f64( f64 )` = it takes one `f64` by value "
            "and returns an `f64`",
            "9:1-9:10 error: `by_reference` is `f64( const ref f64 )`, but a `@trig_` function is `f64( f64 )` = it takes one "
            "`f64` by value and returns an `f64`",
            "12:1-12:10 error: `none` is `f64()`, but a `@trig_` function is `f64( f64 )` = it takes one `f64` by value and "
            "returns an `f64`",
            "19:5-19:14 error: `cosine` is `f64( const ref Angle )`, but a `@trig_` function is `f64( f64 )` = it takes one "
            "`f64` by value and returns an `f64`"
        }
    );
}

TEST_CASE( "errors_try", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D6. `try` takes the payload out of an `ok` and returns an `err` to the caller, so its operand must
// be a `result`, and so must what the function returns, with the same error type.
class Buf
{
    public u64 n;

    Buf( u64 m )
    {
        n = m;
    }

    ~Buf()
    {
    }
};

enum Bad
{
    nope
};

enum Other
{
    worse
};

result<i32, Bad> parse()
{
    return result::ok( 1 );
}

result<Buf, Bad> fill()
{
    return result::ok( Buf( 4 ) );
}

i32 plain()
{
    return 1;
}

result<i32, Bad> not_a_result()
{
    i32 n = try plain();
    return result::ok( n );
}

i32 returns_plain()
{
    i32 n = try parse();
    return n + 1;
}

void returns_nothing()
{
    try parse();
}

result<i32, Other> wrong_error()
{
    i32 n = try parse();
    return result::ok( n );
}

result<u64, Bad> copied()
{
    result<Buf, Bad> r = fill();
    Buf b = try r;
    return result::ok( b.n );
}

i32 main()
{
    return try parse();
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "44:17-44:24 error: `try` needs a `result`, and this is `i32`",
            "50:13-50:24 error: `try` passes an error on to the caller, so `returns_plain` must return a `result` = it returns "
            "`i32`",
            "56:5-56:16 error: `try` passes an error on to the caller, so `returns_nothing` must return a `result` = it "
            "returns `void`",
            "61:13-61:24 error: `try` passes on a `Bad` error, but `wrong_error` returns `result<i32, Other>`",
            "68:17-68:18 error: an owning value is transferred, not copied = write `move r`",
            "74:12-74:23 error: `try` passes an error on to the caller, so `main` must return a `result` = it returns `i32`"
        }
    );
}

TEST_CASE( "errors_type_as_value", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A type's name is not a value: each use is one error, and nothing after it cascades.
struct Point
{
    i32 x;
};

enum Colour
{
    Red,
    Green,
};

i32 measure<T>( T value )
{
    T copy = T;
    return 0;
}

i32 main()
{
    Point  p = Point;
    Point* q = &Point;
    Colour = Colour::Red;
    i32    n = Point + 1;
    i32    m = measure( n );
    return Colour;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "15:14-15:15 error: `T` is a type, not a value",
            "21:16-21:21 error: `Point` is a type, not a value",
            "22:17-22:22 error: `Point` is a type, not a value",
            "23:5-23:11 error: `Colour` is a type, not a value",
            "24:16-24:21 error: `Point` is a type, not a value",
            "26:12-26:18 error: `Colour` is a type, not a value"
        }
    );
}

TEST_CASE( "errors_unbalanced_braces", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A body that lost its `}` leaves every brace after it one level off. What breaks first reports,
// the end of the file reports the brace, and nothing else in the file does.
i32 first( i32 x )
{
    if( x > 0 )
    {
        return 1;
    }
    return 0;

i32 second( i32 y )
{
    return y + missing;
}

i32 main()
{
    return first( 1 ) + second( 2 ) + also_missing;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "9:14-9:14 error: expected `}`, found `i32`",
            "19:2-19:2 error: expected `}`, found end of file = a `{` above was never closed"
        }
    );
}

TEST_CASE( "errors_uninitialised", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D9: reading a variable before anything put a value in it. The analysis is the one D31's `out`
// parameters already needed - a *must* analysis over the CFG - asked at every read rather than only
// at the ways out.
struct Point { i32 x; i32 y; };

i32 never_written()
{
    i32 x;

    return x;
}

i32 written_on_one_branch( bool b )
{
    i32 x;

    if( b )
    {
        x = 1;
    }

    return x;
}

i32 read_in_a_condition()
{
    i32 x;

    if( x > 0 )
    {
        return 1;
    }

    return 0;
}

i32 a_whole_struct()
{
    Point p;

    return p.x;
}

// An `out` parameter holds storage, not a value: the caller supplies somewhere to write, and until
// the body writes it there is nothing to read.
void reads_its_own_out( out i32 a )
{
    i32 copied = a;

    a = copied;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "10:12-10:13 error: `x` is used before it is initialised = give it a value at its declaration",
            "22:12-22:13 error: `x` may be used before it is initialised = it is assigned on some paths to here, but not all",
            "29:9-29:10 error: `x` is used before it is initialised = give it a value at its declaration",
            "41:12-41:15 error: `p` is used before it is initialised = give it a value at its declaration",
            "48:18-48:19 error: `a` is read before this function assigns it = an `out` parameter holds no value on entry - the "
            "caller supplies the storage, not the value"
        }
    );
}

TEST_CASE( "errors_unknown_name", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(i32 main()
{
    i32 known = 1;
    return known + missing;
}

i32 caller()
{
    return no_such_function( 1 );
}

// Every use of a missing name is reported: each one is undeclared.
i32 repeated()
{
    i32 a = missing + 1;
    i32 b = missing * a;
    return missing + b;
}

i32 types()
{
    Widgt first  = 1;
    Widgt second = 2;
    return 0;
}

i32 value_then_type()
{
    i32 a = shape;
    shape b = 2;
    return a;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "4:20-4:27 error: `missing` is not declared",
            "9:12-9:28 error: `no_such_function` is not declared",
            "15:13-15:20 error: `missing` is not declared",
            "16:13-16:20 error: `missing` is not declared",
            "17:12-17:19 error: `missing` is not declared",
            "22:5-22:10 error: unknown type `Widgt`",
            "23:5-23:10 error: unknown type `Widgt`",
            "29:13-29:18 error: `shape` is not declared",
            "30:5-30:10 error: unknown type `shape`"
        }
    );
}

TEST_CASE( "errors_unsafe", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// The gate and the two rules around it: an operation that needs permission and has none, a block
// that asks for permission it does not use, and a block inside one that already grants it.
i32 main()
{
    i32  x = 1;
    i32* address = &x;

    u8* bare = cast<u8*>( address );

    unsafe
    {
        i32 nothing = 2;
    }

    unsafe
    {
        u8* allowed = cast<u8*>( address );

        unsafe
        {
            u8* again = cast<u8*>( address );
        }
    }

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "8:16-8:36 error: converting between `i32*` and `u8*` needs an `unsafe` block = the compiler cannot check that the "
            "target type describes what is there",
            "10:5-13:6 error: an `unsafe` block that does nothing unsafe = remove `unsafe`",
            "19:9-22:10 error: an `unsafe` block inside another one = the enclosing block already permits it"
        }
    );
}

TEST_CASE( "errors_use_before_declaration", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(i32 main()
{
    i32 later = used_first;
    i32 used_first = 1;
    return later;
}
)kl",
        Stage::Reported
    );

    REQUIRE( p.shown() == Lines { "3:17-3:27 error: `used_first` is not declared" } );
}

TEST_CASE( "errors_value_parameters", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A type before a type parameter's name makes a value parameter, which Keel does not have yet.
// Each is one error; the parameter's uses in the body stay quiet.
class Value<i32 N> // error
{
    i32 v;
    public Value() { v = N; }
};

struct Point { i32 x; };
void at<Point p>() { } // error

u64 width<u64 W>() { return W; } // error

// A builtin's name as a type parameter is refused once, though each method declares it again.
class Holder<i32> // error
{
    i32 v;
    public Holder() { v = 0; }
    public i32 get() const { return v; }
};

// A type parameter has no default.
class Default<T = i32> { T v; public Default( T x ) { v = x; } }; // error

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "3:13-3:16 error: a size parameter is a `u64` = other types are not supported yet",
            "10:9-10:14 error: a size parameter is a `u64` = write `u64 N`",
            "15:14-15:17 error: `i32` is the name of a builtin type = a declaration may not take one",
            "23:16-23:16 error: expected `>`, found `=` = a type parameter has no default: write the type argument at each use"
        }
    );
}

TEST_CASE( "errors_variant_default", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A payload's field takes no default, and is still declared.
enum Shape
{
    Circle( f64 radius = 1.0 ),
    Square( f64 side )
};

f64 size( Shape s )
{
    switch( s )
    {
    case Shape::Circle( r ):
        return r;
    case Shape::Square( side ):
        return side;
    }
}

i32 main()
{
    return size( Shape::Circle( 2.0 ) ) > 1.0 ? 0 : 1;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() == Lines { "4:24-4:25 error: a variant's field takes no default = give its value where the variant is built, "
                             "as in `Circle(...)`" }
    );
}

TEST_CASE( "errors_void", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D51. `void` holds nothing: written as a parameter, field, payload or global it is refused, and as
// a local it is warned about. None of it is reported when `void` arrives as a type argument.
void takes( void v )
{
}

void calls( fn( void ) -> i32 p )
{
}

struct Holder
{
    i32 n;
    void v;

    void take( ref void r )
    {
    }
};

enum Shape
{
    dot( void nothing ),
    circle( f64 radius )
};

void global;

void effect()
{
}

T same<T>( T a ) where T : copyable
{
    T held = a;
    return held;
}

enum Bad
{
    nope
};

result<void, Bad> fine()
{
    return result::ok();
}

result<void, Bad> extra()
{
    return result::ok( effect() );
}

i32 main()
{
    void x = effect();
    void y;
    same( effect() );
    switch( fine() )
    {
        case result::ok( v ):
            break;
        case result::err:
            break;
    }
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "3:13-3:17 error: a parameter cannot be `void` = it passes nothing; remove it",
            "7:17-7:21 error: a parameter cannot be `void` = it passes nothing; remove it",
            "14:5-14:9 error: a field cannot be `void` = it holds nothing; remove it",
            "16:20-16:24 error: a parameter cannot be `void` = it passes nothing; remove it",
            "23:10-23:14 error: a field cannot be `void` = it holds nothing; remove it",
            "27:1-27:5 error: a global cannot be `void` = it holds nothing; remove it",
            "51:22-51:34 error: `ok` carries 0 values, but 1 was given",
            "56:5-56:9 warning: `x` is `void`, so it holds nothing [-Wvoid-variable] = write `effect();` on its own",
            "57:5-57:9 warning: `y` is `void`, so it holds nothing [-Wvoid-variable] = remove it",
            "61:14-61:29 error: `ok` carries 0 values, but 1 was bound = write it without a pattern"
        }
    );
}

TEST_CASE( "errors_void_pointer", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// `void*` is opaque: it is passed, returned and cast, but there is nothing at the other end to read,
// write or step over. Each misuse is one error.
extern void* malloc( u64 size );
extern void* memset( void* s, i32 c, u64 n );

i32 main()
{
    i32  x = 5;
    i32* px = &x;

    unsafe
    {
        void* raw = malloc( 8 );
        auto  read = *raw;
        *raw = 1;
        void* next = raw + 1;
        memset( px, 0, 4 );
    }

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "14:22-14:26 error: `void*` is opaque, so there is nothing to dereference = cast it to the pointer it really is, "
            "as in `cast<u8*>(p)`",
            "15:9-15:13 error: `void*` is opaque, so there is nothing to dereference = cast it to the pointer it really is, as "
            "in `cast<u8*>(p)`",
            "16:22-16:25 error: `void*` points at one `void`, so it has no arithmetic = cast it to a `u8[*]` to offset it by "
            "bytes",
            "17:17-17:19 error: expected `void*`, but got `i32*`"
        }
    );
}

TEST_CASE( "errors_walk", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D74: a loop over a walk, and `yield`. One error per marked line.
class Bag
{
    i32[*] data_;
    u64    count_;

    public Bag()
    {
        data_  = nullptr;
        count_ = 0;
    }

    void operator:( const ref i32 item ) const
    {
        for( u64 i : 0..count_ )
        {
            unsafe
            {
                yield( data_[i], 1 ); // error: `yield` takes 1 argument
            }
        }
    }

    void operator:( ref i32 item )
    {
        for( u64 i : 0..count_ )
        {
            unsafe
            {
                yield( data_[i] ); // error: needs `ref`
            }
        }
    }
};

class Reading
{
    i32 x_;

    public Reading()
    {
        x_ = 0;
    }

    void operator:( const ref i32 item ) const
    {
        yield( x_ );
        i32 y = item; // error: `item` is not declared
    }
};

class Loops
{
    i32 x_;

    public Loops()
    {
        x_ = 0;
    }

    void operator:( const ref i32 item ) const
    {
        for( const ref i32 x : this ) // error: a walk cannot loop over itself
        {
            yield( x );
        }
    }
};

void outside()
{
    yield( 1 ); // error: only allowed in a walk
}

i32 main()
{
    i32[*] raw = nullptr;
    for( const ref i32 x : raw ) // error: has no walk
    {
    }

    const Reading r = Reading();
    for( ref i32 x : r ) // error: has no writing walk
    {
    }

    Bag b = Bag();
    for( const ref u8 x : b ) // error: the walk yields `i32`
    {
    }

    for( const ref i32 x : b )
    {
        x = 1; // error: `x` is `const`
    }

    for( i32 x : b )
    {
        x = 1; // error: lent by the walk
    }

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "19:22-19:37 error: `yield` takes 1 argument, but 2 were given",
            "30:24-30:32 error: `yield` may modify this argument = write `ref`",
            "48:17-48:21 error: `item` is not declared",
            "63:32-63:36 error: a walk cannot loop over itself = it is inlined, so it would never end",
            "72:5-72:16 error: `yield` is only allowed in a walk = declare `void operator:(...)`",
            "78:28-78:31 error: `i32[*]` has no walk = count over it: `for(u64 i : 0..raw.size())`",
            "83:22-83:23 error: `Reading` has no writing walk = bind it `const ref`",
            "88:10-88:22 error: the walk yields `i32`",
            "94:9-94:10 error: `x` is `const` = remove `const` to modify it",
            "99:9-99:10 error: `x` is lent by the walk, so it cannot be modified = bind it `ref`"
        }
    );
}

TEST_CASE( "errors_walk_bindings", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D74: bindings by name over a walk that yields two values. One error per marked line.
class Pairs
{
    i32[*] keys_;
    i32[*] values_;
    u64    count_;

    public Pairs()
    {
        keys_   = nullptr;
        values_ = nullptr;
        count_  = 0;
    }

    void operator:( const ref i32 key, const ref i32 value ) const
    {
        for( u64 i : 0..count_ )
        {
            unsafe
            {
                yield( keys_[i], values_[i] );
            }
        }
    }

    void operator:( const ref i32 key, ref i32 value )
    {
        for( u64 i : 0..count_ )
        {
            unsafe
            {
                yield( keys_[i], ref values_[i] );
            }
        }
    }
};

i32 main()
{
    Pairs p = Pairs();
    i32   sum = 0;

    for( [ const ref key, const ref size ] : p ) // error: no `size`
    {
        sum += key;
    }

    for( [ key, key ] : p ) // error: bound twice
    {
        sum += key;
    }

    for( [ ref key, ref value ] : p ) // error: `key` is read-only
    {
        value = key;
    }

    for( const ref i32 key : p ) // error: two values
    {
        sum += key;
    }

    for( [ const ref key ] : p )
    {
        for( [ key, value ] : p ) // error: `key` shadows
        {
            sum += value;
        }
    }

    for( [ value, const ref key ] : p )
    {
        sum += key + value;
    }

    for( [ ref value ] : p )
    {
        value += 1;
    }

    for( [ value ] : p )
    {
        value = 0; // error: bare is read-only
    }

    return sum;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "43:37-43:41 error: this walk yields no `size` = it yields `key`, `value`",
            "48:17-48:20 error: `key` is already declared in this scope = previous declaration is at: t.kl:48:12",
            "53:12-53:15 error: the walk lends `key` read-only = bind it `const ref`",
            "58:10-58:31 error: this walk yields 2 values = bind them by name: `for([const ref key, ref value] : m)`",
            "65:16-65:19 error: `key` shadows an outer declaration = previous declaration is at: t.kl:63:12",
            "83:9-83:14 error: `value` is lent by the walk, so it cannot be modified = bind it `ref`"
        }
    );
}

TEST_CASE( "errors_walk_bound", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D74: a walk is reached through a bound only, and a bound without a walk gives none. One error
// per marked line.
interface iterable<T>
{
    void operator:( const ref T item ) const;
};

i32 held( const ref iterable<i32> c ) // error: a bound only
{
    return 0;
}

i32 plain<C>( const ref C c ) where C : copyable
{
    i32 total = 0;

    for( i32 v : c ) // error: no walk
    {
        total += v;
    }

    return total;
}

void zero<C>( ref C c ) where C : iterable<i32>
{
    for( ref i32 v : c ) // error: no writing walk
    {
        v = 0;
    }
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "8:21-8:34 error: `iterable<i32>` can only bound a type parameter, because its `operator:` is a walk = take a `T` "
            "with `where T : iterable<i32>` instead",
            "17:18-17:19 error: `C` has no walk = bound it by an interface that requires `operator:`",
            "27:22-27:23 error: `C` has no writing walk = bind it `const ref`"
        }
    );
}

TEST_CASE( "errors_walk_bound_ambiguity", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// Two bounds that each require a walk do not say which a loop through them means, whichever is
// written first, even when both yield the same type; nor does one interface at two instances.

interface ints { void operator:( const ref i32 item ) const; };
interface more_ints { void operator:( const ref i32 item ) const; };
interface bools { void operator:( const ref bool item ) const; };
interface each<T> { void operator:( const ref T item ) const; };
interface sized { u64 size() const; };

i32 ints_then_bools<C>( const ref C c ) where C : ints & bools { i32 s = 0; for( const ref i32 x : c ) { s += x; } return s; }
i32 bools_then_ints<C>( const ref C c ) where C : bools & ints { i32 s = 0; for( const ref i32 x : c ) { s += x; } return s; }
i32 same_yield<C>( const ref C c ) where C : ints & more_ints { i32 s = 0; for( i32 x : c ) { s += x; } return s; }
i32 two_instances<C>( const ref C c ) where C : each<i32> & each<bool> { i32 s = 0; for( i32 x : c ) { s += x; } return s; }
i32 two_instances_swapped<C>( const ref C c ) where C : each<bool> & each<i32> { i32 s = 0; for( i32 x : c ) { s += x; } return s; }

// Still accepted: only one bound walks.
i32 one_walks<C>( const ref C c ) where C : sized & ints { i32 s = 0; for( i32 x : c ) { s += x; } return s; }

i32 main() { return 0; }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "10:100-10:101 error: both `ints` and `bools` walk `C`, so the loop does not say which = loop in a function bound "
            "by only one of them",
            "11:100-11:101 error: both `bools` and `ints` walk `C`, so the loop does not say which = loop in a function bound "
            "by only one of them",
            "12:89-12:90 error: both `ints` and `more_ints` walk `C`, so the loop does not say which = loop in a function "
            "bound by only one of them",
            "13:98-13:99 error: both `each<i32>` and `each<bool>` walk `C`, so the loop does not say which = loop in a "
            "function bound by only one of them",
            "14:106-14:107 error: both `each<bool>` and `each<i32>` walk `C`, so the loop does not say which = loop in a "
            "function bound by only one of them"
        }
    );
}

TEST_CASE( "errors_walk_builtin", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D74: the compiler's walk of a run or an array yields each element, unnamed, and writes only what
// may be written. One error per marked line.
void zero( const ref i32[] values )
{
    for( ref i32 v : values ) // error: `values` is read-only
    {
        v = 0;
    }
}

i32 main()
{
    i32[2] a = [ 1, 2 ];
    i32    sum = 0;

    for( [ value ] : a ) // error: no names
    {
        sum += value;
    }

    for( const ref i64 v : a ) // error: yields `i32`
    {
        sum += 1;
    }

    const i32[2] b = [ 3, 4 ];
    for( ref i32 v : b ) // error: `b` is const
    {
        v = 0;
    }

    for( i32 v : a )
    {
        v = 0; // error: bare is read-only
    }

    return sum;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "5:22-5:28 error: `values` is `const` = remove `const` to modify it",
            "16:10-16:19 error: `i32[2]` yields its elements without names = bind each one alone: `for(const ref i32 x : a)`",
            "21:10-21:23 error: the walk yields `i32`",
            "27:22-27:23 error: `b` is `const` = remove `const` to modify it",
            "34:9-34:10 error: `v` is lent by the walk, so it cannot be modified = bind it `ref`"
        }
    );
}

TEST_CASE( "errors_walk_builtin_loans", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D74: a loop over an array freezes it as a walk's receiver does: the body may not replace what
// owns something. An array of plain values owns nothing, so it may change. One error per
// marked line.
class Counted
{
    public i32 value;

    public Counted( i32 v )
    {
        value = v;
    }

    ~Counted()
    {
    }
};

i32 main()
{
    Counted[2] c = [ Counted( 1 ), Counted( 2 ) ];

    for( const ref Counted item : c )
    {
        c[0] = Counted( item.value ); // error: replaced while walked
    }

    i32[2] a = [ 1, 2 ];
    for( i32 v : a )
    {
        a[1] = v;
    }

    return a[1];
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "24:9-24:38 error: this could change `c` while a loop walks it = the walk goes on reading it; the loop is at 22:5"
        }
    );
}

TEST_CASE( "errors_walk_declaration", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D74: what a walk may hand its loop. One error per marked line.
class Wrong
{
    i32 x_;

    i32 operator:( const ref i32 item ) const // error: a walk returns nothing
    {
        return 0;
    }
};

class Empty
{
    i32 x_;

    void operator:() const {} // error: a walk hands the loop at least one value
};

class Taken
{
    i32 x_;

    void operator:( move i32 item ) const {} // error: a walk lends what it yields
};

class Writes
{
    i32 x_;

    void operator:( ref i32 item ) const {} // error: a `const` walk cannot hand out `ref`
};

class Reads
{
    i32 x_;

    void operator:( const ref i32 item ) {} // error: a walk that writes nothing is `const`
};

class Unnamed
{
    i32 x_;

    void operator:( const ref i32, const ref i32 b ) const {} // error: name each value a walk yields
};

class Twice
{
    i32 x_;

    void operator:( const ref i32 a ) const {}
    void operator:( const ref i32 b ) const {} // error: already has a reading walk
};

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "6:5-6:8 error: a walk returns nothing = write `void operator:(...)`",
            "16:19-16:21 error: a walk hands the loop at least one value",
            "23:21-23:34 error: a walk lends what it yields = take it `const ref` or `ref`",
            "30:21-30:33 error: a `const` walk cannot hand out `ref` = take it `const ref`, or drop the `const`",
            "37:10-37:19 error: a walk that writes nothing is `const` = write `const` after its parameter list",
            "44:21-44:34 error: name each value a walk yields = a loop binds them by name",
            "52:10-52:19 error: `Twice` already has a reading walk = previous declaration is at: t.kl:51:5"
        }
    );
}

TEST_CASE( "errors_walk_loans", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// D74: a walk's receiver freezes its container while the walk can still reach it, so the body may
// neither change nor move it. After the last time the walk reads it, as on a `break`, it may. One
// error per marked line.
class Bag
{
    i32[*] data_;
    u64    count_;

    public Bag( u64 n )
    {
        unsafe
        {
            data_ = alloc<i32>( n );
        }
        count_ = n;
    }

    ~Bag()
    {
        unsafe
        {
            free( data_ );
        }
    }

    public void grow()
    {
        count_ = count_;
    }

    public u64 size() const
    {
        return count_;
    }

    void operator:( const ref i32 item ) const
    {
        for( u64 i : 0..count_ )
        {
            unsafe
            {
                yield( data_[i] );
            }
        }
    }

    void operator:( ref i32 item )
    {
        for( u64 i : 0..count_ )
        {
            unsafe
            {
                yield( ref data_[i] );
            }
        }
    }
};

void take( move Bag _ ) {}

i32 main()
{
    Bag c = Bag( 2 );

    for( const ref i32 x : c )
    {
        c.grow(); // error: this could change `c` while a loop walks it
    }

    for( const ref i32 x : c )
    {
        c = Bag( 1 ); // error: this could change `c` while a loop walks it
    }

    // Reading it is fine, and so is writing an element through a `ref` binding.
    u64 n = 0;
    for( ref i32 x : c )
    {
        n += c.size();
        x = 1;
    }

    // Nothing reads `c` after a `break`.
    for( const ref i32 x : c )
    {
        take( move c );
        break;
    }

    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "65:24-65:25 warning: unused variable `x` [-Wunused-variable] = write `_` in its place: `for(_ : c)`",
            "67:9-67:10 error: this could change `c` while a loop walks it = the walk goes on reading it; the loop is at 65:5",
            "70:24-70:25 warning: unused variable `x` [-Wunused-variable] = write `_` in its place: `for(_ : c)`",
            "72:9-72:22 error: this could change `c` while a loop walks it = the walk goes on reading it; the loop is at 70:5",
            "84:24-84:25 warning: unused variable `x` [-Wunused-variable] = write `_` in its place: `for(_ : c)`"
        }
    );
}

TEST_CASE( "errors_write_after_move", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A write into part of a moved value does not make it whole again: the field is filled, the rest
// is gone, and the scope would destroy it a second time.
class Buffer
{
    public u8[*] bytes;
    public u64   size;

    Buffer( u64 n )
    {
        unsafe
        {
            bytes = alloc<u8>( n );
        }
        size = n;
    }

    ~Buffer()
    {
        unsafe
        {
            free( bytes );
        }
    }
};

void take( move Buffer _ )
{
}

void fill( out u64 n )
{
    n = 1;
}

void assigned()
{
    Buffer b = Buffer( 4 );
    take( move b );
    b.size = 8;
}

void through_out()
{
    Buffer b = Buffer( 4 );
    take( move b );
    fill( out b.size );
}

void on_one_path( bool give )
{
    Buffer b = Buffer( 4 );
    if( give )
    {
        take( move b );
    }
    b.size = 8;
}

// Writing the whole value is the refill, and is fine.
void refilled()
{
    Buffer b = Buffer( 4 );
    take( move b );
    b = Buffer( 8 );
    b.size = 2;
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "39:5-39:16 error: `b` is used after it was moved = moved at 38:11",
            "46:11-46:21 error: `b` is used after it was moved = moved at 45:11",
            "56:5-56:16 error: `b` may already have been moved = moved at 54:15 on some path to here"
        }
    );
}

TEST_CASE( "needless_deref", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// `( *p ).x` where `.` already reaches through `p`. A line ending `// warning` warns exactly once,
// and every other line warns not at all.
struct Point
{
    i64 x;

    i64 get() const
    {
        return x;
    }
};

i32 main()
{
    Point  q = Point { 3 };
    Point* p = &q;
    ( *p ).x = 4;     // warning
    _ = ( *p ).get(); // warning
    p.x = 5;
    _ = p.get();
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "17:5-17:13 warning: `*` is not needed: `.` reaches through `p` [-Wneedless-deref] = write `p.x`",
            "18:9-18:19 warning: `*` is not needed: `.` reaches through `p` [-Wneedless-deref] = write `p.get`"
        }
    );
}

TEST_CASE( "unconditional_recursion", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A function every path of which calls itself warns, once however many instances it has. A line
// ending `// warning` warns exactly once; every other line warns not at all.
i32 forever( i32 n ) // warning
{
    return forever( n + 1 );
}

i32 either( i32 n ) // warning
{
    if( n > 0 )
    {
        return either( n - 1 );
    }
    return either( n + 1 );
}

T forwards<T>( T x ) // warning
{
    return forwards( x );
}

class Counter
{
    i32 k;
    public Counter() { k = 0; }
    public i32 up( i32 n ) // warning
    {
        return this.up( n + k );
    }
};

// A base case, a `panic` the author wrote, or a loop that never calls it.
i32 down( i32 n )
{
    if( n == 0 )
    {
        return 0;
    }
    return down( n - 1 );
}

i32 dies( i32 n )
{
    if( n == 0 )
    {
        panic( "reached zero" );
    }
    return dies( n - 1 );
}

void spin()
{
    while( true )
    {
    }
}

i32 main()
{
    Counter c = Counter();
    if( c.up( 0 ) == 0 )
    {
        spin();
    }
    return forever( 1 ) + either( 2 ) + forwards<i32>( 3 ) + cast<i32>( forwards<i64>( 4 ) ) + down( 5 ) + dies( 6 );
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "3:5-3:12 warning: `forever` calls itself on every path, so it never returns [-Wunconditional-recursion] = every "
            "path reaches the call at 5:12; return without it on some path",
            "8:5-8:11 warning: `either` calls itself on every path, so it never returns [-Wunconditional-recursion] = every "
            "path reaches the call at 12:16; return without it on some path",
            "17:3-17:11 warning: `forwards` calls itself on every path, so it never returns [-Wunconditional-recursion] = "
            "every path reaches the call at 19:12; return without it on some path",
            "26:16-26:18 warning: `up` calls itself on every path, so it never returns [-Wunconditional-recursion] = every "
            "path reaches the call at 28:16; return without it on some path"
        }
    );
}

TEST_CASE( "unreachable_after_endless_loop", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A loop that never finishes leaves its block: what follows warns. One with a `break` may finish,
// so what follows it is reached. A line ending `// warning` warns exactly once.
i32 endless()
{
    while( true )
    {
    }
    return 0; // warning
}

i32 left_by_break( i32 n )
{
    while( true )
    {
        if( n > 0 )
        {
            break;
        }
    }
    return n;
}

i32 main()
{
    return left_by_break( 1 );
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "8:5-8:14 warning: this statement is never reached [-Wunreachable-code] = the one before it always leaves the block"
        }
    );
}

TEST_CASE( "unreachable_code", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A statement after one that always leaves its block warns, once a block. A line ending
// `// warning` warns exactly once; every other line warns not at all.
never stop()
{
    panic( "stopped" );
}

i32 after_return()
{
    return 1;
    i32 x = 2; // warning
    return x;
}

void after_loop_exits( i32 n )
{
    i32 x = 0;
    while( n > 0 )
    {
        break;
        x = 1; // warning
    }
    for( i32 i = 0; i < n; i++ )
    {
        continue;
        x = 2; // warning
    }
    _ = x;
}

i32 after_both_arms( bool b )
{
    if( b )
    {
        return 1;
    }
    else
    {
        return 2;
    }
    return 3; // warning
}

i32 after_never()
{
    stop();
    return 1; // warning
}

i32 after_panic()
{
    panic( "no" );
    return 1; // warning
}

// Only one arm leaves, a loop may finish, and a nested block's exit leaves the outer block too.
i32 reached( bool b, i32 n )
{
    if( b )
    {
        return 1;
    }
    while( n > 0 )
    {
        return 2;
    }
    {
        return 3;
    }
    return 4; // warning
}

i32 main()
{
    return 0;
}
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() ==
        Lines {
            "11:5-11:15 warning: this statement is never reached [-Wunreachable-code] = the one before it always leaves the "
            "block",
            "21:9-21:15 warning: this statement is never reached [-Wunreachable-code] = the one before it always leaves the "
            "block",
            "26:9-26:15 warning: this statement is never reached [-Wunreachable-code] = the one before it always leaves the "
            "block",
            "41:5-41:14 warning: this statement is never reached [-Wunreachable-code] = the one before it always leaves the "
            "block",
            "47:5-47:14 warning: this statement is never reached [-Wunreachable-code] = the one before it always leaves the "
            "block",
            "53:5-53:14 warning: this statement is never reached [-Wunreachable-code] = the one before it always leaves the "
            "block",
            "70:5-70:14 warning: this statement is never reached [-Wunreachable-code] = the one before it always leaves the "
            "block"
        }
    );
}

TEST_CASE( "unused_private_without_main", "[sema][diagnostics]" )
{
    const Compiled p(
        R"kl(// A file with no `main` is a module of a program it cannot see, whose `@functions_of` may list its
// private functions, so a test-shaped one stays quiet; a generic one no list can hold still warns.
private void listed( i32 _ ) {}
private T kept<T>( move T t ) { return t; }
)kl",
        Stage::Reported
    );

    REQUIRE(
        p.shown() == Lines { "4:11-4:15 warning: unused private function `kept` [-Wunused-private] = nothing public in this "
                             "module reaches it; remove it" }
    );
}

} // namespace keel
#endif
