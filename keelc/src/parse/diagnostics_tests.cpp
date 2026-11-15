// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Programs loading diagnoses, as `--dump-ast` loads them, each with every diagnostic it shows.

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

#include "check/pipeline_test_support.h"
#include "parse/loader.h"

namespace keel
{
namespace
{

using Lines = std::vector<std::string>;

// What loading the source diagnoses.
Lines loaded( std::string_view source )
{
    Source_manager sm;
    Interner       interner;
    Literal_pool   literals;
    Diagnostics    diags;
    load_program( sm.add_file( "t.kl", std::string( source ) ), sm, interner, literals, diags );
    return Compiled::shown( diags, sm );
}
} // namespace

TEST_CASE( "errors_ambiguous_operators", "[parse][diagnostics]" )
{
    REQUIRE(
        loaded( R"kl(i32 bitwise_vs_comparison()  { return a & b == c; }
i32 shift_vs_additive()      { return a << b + c; }
i32 logical()                { return a && b || c; }
)kl" ) ==
        Lines {
            "1:43-1:47 error: `&` and `==` cannot be mixed without parentheses = add parentheses to say which grouping you "
            "mean",
            "2:44-2:47 error: `<<` and `+` cannot be mixed without parentheses = add parentheses to say which grouping you "
            "mean",
            "3:39-3:48 error: `&&` and `||` cannot be mixed without parentheses = add parentheses to say which grouping you "
            "mean"
        }
    );
}

TEST_CASE( "errors_assign_in_condition", "[parse][diagnostics]" )
{
    REQUIRE(
        loaded( R"kl(// `=` in a head is an assignment where a comparison was meant.
i32 main()
{
    i32 x = 0;

    if( x = 1 ) { } // error

    return x;
}
)kl" ) ==
        Lines { "6:10-6:10 error: expected `)`, found `=` = `=` assigns, and an assignment is not a value: compare with `==`" }
    );
}

TEST_CASE( "errors_bad_statement", "[parse][diagnostics]" )
{
    REQUIRE( loaded( R"kl(i32 main()
{
    $
    return 0;
}
)kl" ) == Lines { "3:5-3:6 error: unexpected character `$`" } );
}

TEST_CASE( "errors_cast_no_type", "[parse][diagnostics]" )
{
    REQUIRE(
        loaded( R"kl(// A `cast` or `wrap` with no target type is one error, and the operand after it is read.
i32 main()
{
    i32 a = cast( 1 ); // error
    i32 b = wrap( 2 ); // error

    return a + b;
}
)kl" ) ==
        Lines {
            "4:17-4:18 error: `cast` needs a target type = write it in angle brackets, as in `cast<T>(x)`",
            "5:17-5:18 error: `wrap` needs a target type = write it in angle brackets, as in `wrap<T>(x)`"
        }
    );
}

TEST_CASE( "errors_destructor_call", "[parse][diagnostics]" )
{
    REQUIRE(
        loaded( R"kl(// A destructor cannot be called through a value or a pointer, as `x.~R()` or `p->~R()` would.
class R
{
    i32 n;

    R( i32 v )
    {
        n = v;
    }

    ~R()
    {
    }
};

i32 main()
{
    R x = R( 1 );
    x.~R();
    R* p = &x;
    p.~R();
    return 0;
}
)kl" ) ==
        Lines {
            "19:7-19:9 error: a destructor cannot be named = it runs by itself when the value's lifetime ends, and `_ = move "
            "x;` ends a local `x` now",
            "21:7-21:9 error: a destructor cannot be named = it runs by itself when the value's lifetime ends, and `_ = move "
            "x;` ends a local `x` now"
        }
    );
}

TEST_CASE( "errors_destructor_name", "[parse][diagnostics]" )
{
    REQUIRE(
        loaded( R"kl(// A destructor cannot be named after `::`, whether for its address or to call it.
class C
{
    i32 n;

    C( i32 v )
    {
        n = v;
    }

    ~C()
    {
    }
};

i32 main()
{
    C c = C( 1 );
    auto p = &C::~C;
    C::~C();
    return 0;
}
)kl" ) ==
        Lines {
            "19:18-19:20 error: a destructor cannot be named = it runs by itself when the value's lifetime ends, and `_ = move "
            "x;` ends a local `x` now",
            "20:8-20:10 error: a destructor cannot be named = it runs by itself when the value's lifetime ends, and `_ = move "
            "x;` ends a local `x` now"
        }
    );
}

TEST_CASE( "errors_directives", "[parse][diagnostics]" )
{
    REQUIRE(
        loaded( R"kl(// Each directive below is misplaced or unknown; the functions around them still parse.
@located
struct Point { i32 x; };

@located
extern void abs();

@located @located
void twice() { }

@inline
void unknown() { }

@caller
void expression_only() { }

i32 main()
{
    i32 a = @located;
    i32 b = @inline();
    return 0;
}
)kl" ) ==
        Lines {
            "2:1-2:9 error: `@located` marks a function or a method",
            "5:1-5:9 error: an `extern` function cannot be `@located` = it is defined in C, which takes no location",
            "8:10-8:18 error: `@located` is written twice",
            "11:1-11:8 error: there is no directive `@inline` = the directives a declaration takes are `@located`, "
            "`@real_arithmetic`, `@trig_sin`, `@trig_cos`, `@trig_tan` and `@dft`",
            "14:1-14:8 error: `@caller()` is an expression, not a declaration's directive = write `@located` to receive the "
            "caller",
            "19:13-19:21 error: `@located` marks a declaration = write it before the function's signature",
            "20:13-20:20 error: there is no directive `@inline` = the directives in an expression are `@caller()` and "
            "`@functions_of(T)`"
        }
    );
}

TEST_CASE( "errors_do_while", "[parse][diagnostics]" )
{
    REQUIRE(
        loaded( R"kl(// A `do` body with no `while` after it is one error at the body's `}`, and a missing `;` after the
// condition is one error too. The statements after each still parse.
i32 main()
{
    i32 n = 0;
    do
    {
        n++;
    }

    do
    {
        n++;
    } while( n < 3 )

    return n;
}
)kl" ) ==
        Lines {
            "9:5-9:6 error: expected `while` after a `do` body = write `do { ... } while(condition);`",
            "14:21-14:21 error: expected `;`, found `return`"
        }
    );
}

TEST_CASE( "errors_interfaces", "[parse][diagnostics]" )
{
    REQUIRE(
        loaded( R"kl(interface Shape
{
    f64 area() const;
    i32 sides;
    void grow( f64 by );
};

class Circle implements Shape
{
    f64 r;

    f64 area() const;
    void grow( f64 by ) { }
};
)kl" ) ==
        Lines {
            "4:5-4:15 error: an `interface` holds only method signatures",
            "12:21-12:22 error: expected `{`, found `;` = Keel has no forward declarations: write the definition here"
        }
    );
}

TEST_CASE( "errors_lambdas", "[parse][diagnostics]" )
{
    REQUIRE(
        loaded( R"kl(// A capture is a copy or a `ref`; a lambda's parameter takes no default.
void f()
{
    i32 a = 1;
    run( fn[ move a ]() { } );
    run( fn[ out a ]() { } );
    run( fn[]( i32 x = 1 ) { } );
}
)kl" ) ==
        Lines {
            "5:14-5:18 error: a lambda cannot capture by `move` = a capture is a copy, written bare, or a borrow, written "
            "`ref`",
            "6:14-6:17 error: a lambda cannot capture by `out` = a capture is a copy, written bare, or a borrow, written `ref`",
            "7:24-7:25 error: a lambda's parameter takes no default"
        }
    );
}

TEST_CASE( "errors_local_static", "[parse][diagnostics]" )
{
    REQUIRE(
        loaded( R"kl(// One error each, and no cascade.
enum level
{
    low = 1,
    high
};

i32 bump()
{
    static i32 count = 0;
    count = count + 1;
    return count;
}

i32 main()
{
    u8 accent = 'é';
    u8 euro = '€';
    u8 two = 'ab';
    return bump();
}
)kl" ) ==
        Lines {
            "4:9-4:10 error: a variant cannot be given a value = a variant's value is its position; Keel has no explicit "
            "discriminants",
            "10:5-10:11 error: a function cannot have a `static` = declare it at file scope (a `private` one keeps it to this "
            "module)",
            "17:17-17:21 error: a character literal is one byte; `é` is 2 bytes in UTF-8 = write it in a string literal",
            "18:15-18:20 error: a character literal is one byte; `€` is 3 bytes in UTF-8 = write it in a string literal",
            "19:14-19:18 error: character literal must contain exactly one character = use a string literal for more than one"
        }
    );
}

TEST_CASE( "errors_map_directives", "[parse][diagnostics]" )
{
    REQUIRE(
        loaded( R"kl(// Each `@real_arithmetic` and `@trig_` directive below is misplaced or repeated, one error apiece.
@real_arithmetic
struct Pair { f64 a; f64 b; };

@real_arithmetic @real_arithmetic
void twice( out f64 y, f64 x ) { y = x; }

@trig_sin
f64 angle = 0.5;

@trig_cos @trig_cos
f64 cosine_twice( f64 x ) { return x; }

@trig_sin @trig_tan
f64 sine_or_tangent( f64 x ) { return x; }

i32 main() { return 0; }
)kl" ) ==
        Lines {
            "2:1-2:17 error: `@real_arithmetic` marks a function or a method",
            "5:18-5:34 error: `@real_arithmetic` is written twice",
            "8:1-8:10 error: a `@trig_` directive marks a function or a method",
            "11:11-11:20 error: `@trig_cos` is written twice",
            "14:11-14:20 error: `@trig_tan` after another `@trig_` directive = a function is one of sin, cos and tan"
        }
    );
}

TEST_CASE( "errors_missing_semicolon", "[parse][diagnostics]" )
{
    REQUIRE( loaded( R"kl(i32 main()
{
    return 0
}
)kl" ) == Lines { "3:13-3:13 error: expected `;`, found `}`" } );
}

TEST_CASE( "errors_octal_prefix", "[parse][diagnostics]" )
{
    REQUIRE(
        loaded( R"kl(// `0o` is the same missing octal as a leading zero.
i32 main()
{
    i32 a = 0o755; // error
    i32 b = 0O17; // error

    return a + b;
}
)kl" ) ==
        Lines {
            "4:13-4:18 error: octal literals are not supported = Keel has no octal literals; write the value in hex or decimal",
            "5:13-5:17 error: octal literals are not supported = Keel has no octal literals; write the value in hex or decimal"
        }
    );
}

TEST_CASE( "errors_operator_names", "[parse][diagnostics]" )
{
    REQUIRE(
        loaded( R"kl(// The operator names a type may not declare: `!=` comes from `==` and `<` from `<=>`, `&&` and
// `||` never can be, `!` not yet, shifts, `++` and `--` are kept for numbers, `[` needs its `]`, and
// an operator outside a type has no left operand.
struct a
{
    i32 x;

    bool operator!=( const ref a other ) const
    {
        return false;
    }

    bool operator&&( const ref a other ) const
    {
        return false;
    }

    bool operator<( const ref a other ) const
    {
        return false;
    }

    bool operator!() const
    {
        return false;
    }

    a operator<<( u64 by ) const
    {
        return a { x };
    }

    void operator>>=( u64 by )
    {
    }

    void operator++()
    {
    }

    void operator--()
    {
    }

    bool operator( const ref a other ) const
    {
        return false;
    }

    i32* operator[( u64 index ) const
    {
        return nullptr;
    }
};

bool operator==( const ref a l, const ref a r )
{
    return true;
}

i32* operator[]( u64 index )
{
    return nullptr;
}
)kl" ) ==
        Lines {
            "8:10-8:20 error: `operator!=` cannot be declared = declare `operator==`, and `!=` is its negation",
            "13:10-13:20 error: `&&` cannot be overloaded = it evaluates its right operand only when needed, and a call would "
            "evaluate both",
            "18:10-18:19 error: `operator<` cannot be declared = ordering is declared once, as `operator<=>`, and `<`, `<=`, "
            "`>` and `>=` come from it",
            "23:10-23:19 error: `operator!` cannot be declared yet",
            "28:7-28:17 error: `operator<<` cannot be declared = a shift is kept for the built-in integers",
            "33:10-33:21 error: `operator>>=` cannot be declared = a shift is kept for the built-in integers",
            "37:10-37:20 error: `operator++` cannot be declared = `++` and `--` are kept for the built-in numbers; declare "
            "`operator+=`",
            "41:10-41:20 error: `operator--` cannot be declared = `++` and `--` are kept for the built-in numbers; declare "
            "`operator+=`",
            "45:10-45:18 error: expected an operator after `operator`",
            "50:10-50:19 error: `operator[` cannot be declared = write `operator[]`",
            "56:6-56:16 error: an operator is declared inside the type of its left operand = move it into that type as a "
            "`const` method",
            "61:6-61:16 error: an operator is declared inside the type of its left operand = move it into that type as a "
            "`const` method"
        }
    );
}

TEST_CASE( "errors_promise_directives", "[parse][diagnostics]" )
{
    REQUIRE(
        loaded( R"kl(// Each `@dft` below is misplaced, repeated or a second promise, one error apiece.
@dft
f64 window = 0.5;

@dft @dft
void twice( const ref f64[2] re, const ref f64[2] im, ref f64[2] yr, ref f64[2] yi ) {}

@trig_sin @dft
f64 sine( f64 x ) { return x; }

@dft @trig_cos
f64 cosine( f64 x ) { return x; }

struct Plan
{
    f64 scale;

    @dft
    void run( const ref f64[2] re, const ref f64[2] im, ref f64[2] yr, ref f64[2] yi ) {}
};

i32 main() { return 0; }
)kl" ) ==
        Lines {
            "2:1-2:5 error: `@dft` marks a function",
            "5:6-5:10 error: `@dft` is written twice",
            "8:11-8:15 error: `@dft` after `@trig_sin` = a function makes one promise",
            "11:6-11:15 error: `@trig_cos` after `@dft` = a function makes one promise",
            "18:5-18:9 error: `@dft` marks a function"
        }
    );
}

TEST_CASE( "errors_recovery", "[parse][diagnostics]" )
{
    REQUIRE( loaded( R"kl(i32 broken()
{
    return 0
}

i32 fine()
{
    return 1;
}
)kl" ) == Lines { "3:13-3:13 error: expected `;`, found `}`" } );
}

TEST_CASE( "errors_statements", "[parse][diagnostics]" )
{
    REQUIRE(
        loaded( R"kl(i32 no_effect()     { a + b; }
i32 spaced_pointer() { u32 *p; }
i32 missing_name()   { auto = 5; }
i32 not_a_statement() { struct x; }
)kl" ) ==
        Lines {
            "1:23-1:28 error: this expression has no effect = assign the result, or remove it",
            "2:24-2:30 error: `*` must touch the type it modifies = write `u32* p` to declare a pointer",
            "3:28-3:28 error: expected an identifier, found `=`",
            "4:25-4:31 error: a struct or a class cannot be declared inside a function = declare it at file scope, where it is "
            "visible throughout the file",
            "4:33-4:33 error: expected `{`, found `;`"
        }
    );
}

TEST_CASE( "errors_switch_pattern_discard", "[parse][diagnostics]" )
{
    REQUIRE(
        loaded( R"kl(// `_` stands for a field only where the call is the whole label, so each one below is refused once,
// with no help about discarding a call.
enum Shape
{
    Rect( f64 w, f64 h ),
};

i32 width( Shape s )
{
    switch( s )
    {
        case Shape::Rect( _, _ ).w: return 1;
        case Shape::Rect( ( _ ), h ): return 2;
        case _: return 3;
        default: return 0;
    }
}

i32 main()
{
    return 0;
}
)kl" ) ==
        Lines {
            "12:27-12:28 error: `_` stands only for a field of a pattern",
            "12:30-12:31 error: `_` stands only for a field of a pattern",
            "13:29-13:30 error: `_` stands only for a field of a pattern",
            "14:14-14:15 error: `_` stands only for a field of a pattern"
        }
    );
}

TEST_CASE( "errors_unary_plus", "[parse][diagnostics]" )
{
    REQUIRE(
        loaded( R"kl(// There is no unary `+`.
i32 main()
{
    i32 x = 1;
    i32 y = +x; // error

    return y;
}
)kl" ) == Lines { "5:13-5:14 error: expected an expression, found `+` = there is no unary `+`: write the operand on its own" }
    );
}

TEST_CASE( "errors_walk_discard", "[parse][diagnostics]" )
{
    REQUIRE( loaded( R"kl(// A loop's binding needs a name: `_` is not one. One error, and none after it.
class Bag
{
    i32 value_;

    public Bag()
    {
        value_ = 1;
    }

    void operator:( const ref i32 item ) const
    {
        yield( value_ );
    }
};

i32 main()
{
    Bag b = Bag();
    i32 n = 0;

    for( const ref i32 _ : b ) // error: `_` is not a name
    {
        n += 1;
    }

    return n;
}
)kl" ) == Lines { "22:24-22:25 error: expected an identifier, found `_` = `_` is a keyword, so it cannot be used as a name" } );
}

TEST_CASE( "errors_yield", "[parse][diagnostics]" )
{
    REQUIRE( loaded( R"kl(// D74: `yield` takes its arguments as a call does, so without parentheses it is one error.
i32 main()
{
    yield 1;
    return 0;
}
)kl" ) == Lines { "4:10-4:10 error: expected `(`, found `1`" } );
}

} // namespace keel
#endif
