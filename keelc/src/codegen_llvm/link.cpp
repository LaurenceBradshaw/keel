// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "codegen_llvm/link.h"
#include <lld/Common/Driver.h>
#include <llvm/Support/raw_ostream.h>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdlib>
#endif

#ifdef _WIN32
LLD_HAS_DRIVER( coff )
#else
LLD_HAS_DRIVER( elf )
#endif

namespace keel
{

namespace
{

std::string run_lld( const std::vector<std::string>& words )
{
    std::vector<const char*> arguments;
    for( const std::string& word : words )
    {
        arguments.push_back( word.c_str() );
    }

    std::string              output;
    std::string              errors;
    llvm::raw_string_ostream out( output );
    llvm::raw_string_ostream err( errors );
#ifdef _WIN32
    const lld::Result result = lld::lldMain( arguments, out, err, { { lld::WinLink, &lld::coff::link } } );
#else
    const lld::Result result = lld::lldMain( arguments, out, err, { { lld::Gnu, &lld::elf::link } } );
#endif
    if( result.retCode != 0 )
    {
        return errors.empty() ? "lld failed" : errors;
    }
    return {};
}

#ifdef _WIN32

// The numbers of an SDK folder's name, `10.0.26100.0`, for ordering; empty if it is not one.
std::vector<unsigned long> version_of( const std::string& name )
{
    std::vector<unsigned long> parts;
    const char*                at = name.c_str();
    while( *at != '\0' )
    {
        char* end = nullptr;
        parts.push_back( std::strtoul( at, &end, 10 ) );
        if( end == at || ( *end != '.' && *end != '\0' ) )
        {
            return {};
        }
        at = *end == '.' ? end + 1 : end;
    }
    return parts;
}

// The Windows 10+ SDK's root: the registry's, else where its installer puts it.
std::filesystem::path kits_root()
{
    wchar_t buffer[MAX_PATH];
    DWORD   size = sizeof( buffer );
    if( RegGetValueW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots",
            L"KitsRoot10",
            RRF_RT_REG_SZ,
            nullptr,
            buffer,
            &size
        ) == ERROR_SUCCESS )
    {
        return buffer;
    }
    const wchar_t* const programs = _wgetenv( L"ProgramFiles(x86)" );
    return std::filesystem::path( programs != nullptr ? programs : L"C:\\Program Files (x86)" ) / "Windows Kits" / "10";
}

// The newest SDK's `Lib/<version>` holding both ucrt.lib and kernel32.lib, or empty.
std::filesystem::path find_sdk()
{
    std::filesystem::path      best;
    std::vector<unsigned long> best_version;
    std::error_code            ec;
    for( const auto& entry : std::filesystem::directory_iterator( kits_root() / "Lib", ec ) )
    {
        const std::vector<unsigned long> version = version_of( entry.path().filename().string() );
        if( version.empty() || version <= best_version ||
            !std::filesystem::exists( entry.path() / "ucrt" / "x64" / "ucrt.lib", ec ) ||
            !std::filesystem::exists( entry.path() / "um" / "x64" / "kernel32.lib", ec ) )
        {
            continue;
        }
        best         = entry.path();
        best_version = version;
    }
    return best;
}

#endif

} // namespace

#ifdef _WIN32

// x86-64 Windows: the SDK's UCRT and kernel32 alone, keel_rt giving the start-up (PLAN §7.1).
std::string link_executable(
    std::span<const std::filesystem::path> objects,
    const std::filesystem::path&           runtime,
    const std::filesystem::path&           executable,
    std::span<const std::string>           linked,
    bool                                   debug
)
{
    const std::filesystem::path sdk = find_sdk();
    if( sdk.empty() )
    {
        return "cannot find the Windows SDK's ucrt.lib and kernel32.lib - is the Windows 10 SDK installed?\n";
    }

    std::vector<std::string> words = {
        "lld-link",
        "/nologo",
        "/subsystem:console",
        "/entry:mainCRTStartup",
        "/nodefaultlib",
        "/include:_tls_used", // keel_rt's TLS directory, which nothing names
        "/out:" + executable.generic_string(),
        "/libpath:" + ( sdk / "ucrt" / "x64" ).generic_string(),
        "/libpath:" + ( sdk / "um" / "x64" ).generic_string(),
    };
    if( debug )
    {
        words.push_back( "/debug" );
    }
    for( const std::filesystem::path& object : objects )
    {
        words.push_back( object.generic_string() );
    }
    words.push_back( runtime.generic_string() );
    for( const std::string& word : linked )
    {
        // -L and -l, as link.exe spells them; C's maths is in the UCRT.
        if( word == "-lm" )
        {
            continue;
        }
        if( word.starts_with( "-L" ) )
        {
            words.push_back( "/libpath:" + word.substr( 2 ) );
        }
        else if( word.starts_with( "-l" ) )
        {
            words.push_back( word.substr( 2 ) + ".lib" );
        }
        else
        {
            words.push_back( word );
        }
    }
    words.push_back( "ucrt.lib" );
    words.push_back( "kernel32.lib" );
    return run_lld( words );
}

#else

// x86-64 Linux, glibc: its start files, the dynamic loader, and libc (PLAN §7.1's link line).
std::string link_executable(
    std::span<const std::filesystem::path> objects,
    const std::filesystem::path&           runtime,
    const std::filesystem::path&           executable,
    std::span<const std::string>           linked,
    bool
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
    return run_lld( words );
}

#endif

} // namespace keel
