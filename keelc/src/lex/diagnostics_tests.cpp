// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Sources the lexer diagnoses, each with every diagnostic it shows.

#ifdef ENABLE_UNIT_TESTS
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

#include "check/pipeline_test_support.h"
#include "lex/lexer.h"

namespace keel
{
namespace
{

using Lines = std::vector<std::string>;

// What lexing the source diagnoses.
Lines lexed( std::string_view source )
{
    Source_manager sm;
    Interner       interner;
    Literal_pool   literals;
    Diagnostics    diags;
    lex( sm.add_file( "t.kl", std::string( source ) ), sm, interner, literals, diags );
    return Compiled::shown( diags, sm );
}
} // namespace

TEST_CASE( "errors_misc", "[lex][diagnostics]" )
{
    REQUIRE(
        lexed( R"kl(#include <stdio.h>
$
a b
)kl" ) ==
        Lines {
            "1:1-1:2 error: Keel has no preprocessor = use `import` instead of `#include`",
            "2:1-2:2 error: unexpected character `$`",
            "3:2-3:4 error: unexpected non-breaking space (U+00A0) = replace it with an ordinary space"
        }
    );
}

TEST_CASE( "errors_numbers", "[lex][diagnostics]" )
{
    REQUIRE(
        lexed( R"kl(007
1_
1__0
0x
0b
42u
1e
1else
)kl" ) ==
        Lines {
            "1:1-1:4 error: leading zeros are not allowed in numeric literals = Keel has no octal literals; remove the leading "
            "zero",
            "2:2-2:3 error: digit separator must be between digits",
            "3:2-3:3 error: digit separator must be between digits",
            "4:1-4:3 error: hex literal has no digits = write at least one digit after `0x`",
            "5:1-5:3 error: binary literal has no digits = write at least one digit after `0b`",
            "6:3-6:4 error: unexpected `u` after numeric literal = literal suffixes are not supported",
            "7:2-7:3 error: exponent has no digits = write at least one digit after `e`"
        }
    );
}

TEST_CASE( "errors_strings", "[lex][diagnostics]" )
{
    REQUIRE(
        lexed( R"kl("\q"
"\x4"
"unterminated
''
'ab'
)kl" ) ==
        Lines {
            "1:2-1:4 error: unknown escape sequence `\\q` = valid escapes are \\n \\t \\r \\0 \\\\ \\\" \\' and \\xNN",
            "2:2-2:4 error: `\\x` needs exactly two hex digits = for example `\\x41`",
            "3:1-3:2 error: unterminated string literal = add a closing `\"`",
            "4:1-4:3 error: empty character literal = write one character between the quotes",
            "5:1-5:5 error: character literal must contain exactly one character = use a string literal for more than one"
        }
    );
}

} // namespace keel
#endif
