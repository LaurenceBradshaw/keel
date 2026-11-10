// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/link.h"
#include <lld/Common/Driver.h>
#include <llvm/Support/raw_ostream.h>
#include <vector>

LLD_HAS_DRIVER( elf )

namespace keel
{

// x86-64 Linux, glibc: its start files, the dynamic loader, and libc (PLAN §7.1's link line).
std::string link_executable(
    std::span<const std::filesystem::path> objects,
    const std::filesystem::path&           runtime,
    const std::filesystem::path&           executable,
    std::span<const std::string>           linked
)
{
    // Debian's multiarch folder, then Fedora's and Arch's.
    std::filesystem::path start;
    for( const char* folder : { "/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib" } )
    {
        if( std::filesystem::exists( std::filesystem::path( folder ) / "Scrt1.o" ) )
        {
            start = folder;
            break;
        }
    }
    if( start.empty() )
    {
        return "cannot find glibc's Scrt1.o - is its development package installed?";
    }

    std::vector<std::string> words = {
        "ld.lld",
        "--hash-style=gnu",
        "--eh-frame-hdr",
        "-pie",
        "-dynamic-linker",
        "/lib64/ld-linux-x86-64.so.2",
        "-o",
        executable.generic_string(),
        ( start / "Scrt1.o" ).generic_string(),
        ( start / "crti.o" ).generic_string(),
    };
    for( const char* folder :
         { "/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu", "/usr/lib64", "/lib64", "/usr/lib", "/lib", "/usr/local/lib"
         } )
    {
        if( std::filesystem::is_directory( folder ) )
        {
            words.push_back( std::string( "-L" ) + folder );
        }
    }
    for( const std::filesystem::path& object : objects )
    {
        words.push_back( object.generic_string() );
    }
    words.push_back( runtime.generic_string() );
    words.insert( words.end(), linked.begin(), linked.end() );
    words.push_back( "-lc" );
    words.push_back( ( start / "crtn.o" ).generic_string() );

    std::vector<const char*> arguments;
    for( const std::string& word : words )
    {
        arguments.push_back( word.c_str() );
    }

    std::string              output;
    std::string              errors;
    llvm::raw_string_ostream out( output );
    llvm::raw_string_ostream err( errors );
    const lld::Result        result = lld::lldMain( arguments, out, err, { { lld::Gnu, &lld::elf::link } } );
    if( result.retCode != 0 )
    {
        return errors.empty() ? "lld failed" : errors;
    }
    return {};
}

} // namespace keel
