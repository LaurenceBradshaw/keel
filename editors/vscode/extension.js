// Copyright 2026 Laurence Bradshaw
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Runs `keelc --check --diagnostics=json` on each save and underlines what it reports.

'use strict';

const vscode = require( 'vscode' );
const child_process = require( 'child_process' );
const fs = require( 'fs' );
const path = require( 'path' );

const severities = {
    error: vscode.DiagnosticSeverity.Error,
    warning: vscode.DiagnosticSeverity.Warning,
    note: vscode.DiagnosticSeverity.Information,
};

// keelc's lines split on '\n' alone and its columns count bytes, so the conversion works on bytes.
function lines_of( file )
{
    let bytes;
    try
    {
        bytes = fs.readFileSync( file );
    }
    catch
    {
        return [];
    }

    const lines = [];
    let start = 0;
    for( let i = 0; i <= bytes.length; ++i )
    {
        if( i === bytes.length || bytes[i] === 0x0a )
        {
            let end = i;
            if( end > start && bytes[end - 1] === 0x0d )
            {
                --end;
            }
            lines.push( bytes.subarray( start, end ) );
            start = i + 1;
        }
    }

    return lines;
}

// A 1-based byte column on a 1-based line, as a 0-based UTF-16 one.
function utf16_col( lines, line, col )
{
    const bytes = lines[line - 1];
    if( bytes === undefined )
    {
        return 0;
    }

    return bytes.subarray( 0, Math.min( col - 1, bytes.length ) ).toString( 'utf8' ).length;
}

// The span's first line only, as the terminal renderer does: underlining a whole body helps nobody.
// A zero-length span is widened to one character so there is something to see.
function range_of( lines, record )
{
    const line = record.line - 1;
    const start = utf16_col( lines, record.line, record.col );
    const line_end = utf16_col( lines, record.line, Infinity );

    let end = record.end_line === record.line ? utf16_col( lines, record.line, record.end_col ) : line_end;
    if( end <= start )
    {
        end = start + 1;
    }

    return new vscode.Range( line, start, line, end );
}

// keelc's output as diagnostics per absolute path. Every loaded file has an entry, empty or not,
// so that fixing a file's last error clears it.
function parse( stdout, cwd, saved )
{
    const by_file = new Map();
    const lines_cache = new Map();

    for( const text of stdout.split( '\n' ) )
    {
        if( text.trim() === '' )
        {
            continue;
        }

        let record;
        try
        {
            record = JSON.parse( text );
        }
        catch
        {
            continue;
        }

        if( record.kind === 'file' )
        {
            const file = path.resolve( cwd, record.path );
            if( !by_file.has( file ) )
            {
                by_file.set( file, [] );
            }
            continue;
        }

        if( record.kind !== 'diagnostic' )
        {
            continue;
        }

        // No span: shown at the top of the file that was saved.
        let file = saved;
        let range = new vscode.Range( 0, 0, 0, 0 );

        if( record.file !== undefined )
        {
            file = path.resolve( cwd, record.file );
            if( !lines_cache.has( file ) )
            {
                lines_cache.set( file, lines_of( file ) );
            }
            range = range_of( lines_cache.get( file ), record );
        }

        const message = record.help ? `${record.message}\n${record.help}` : record.message;
        const diagnostic = new vscode.Diagnostic( range, message, severities[record.severity] ?? vscode.DiagnosticSeverity.Error );
        diagnostic.source = 'keelc';

        if( !by_file.has( file ) )
        {
            by_file.set( file, [] );
        }
        by_file.get( file ).push( diagnostic );
    }

    return by_file;
}

function activate( context )
{
    const collection = vscode.languages.createDiagnosticCollection( 'keel' );
    const output = vscode.window.createOutputChannel( 'Keel' );
    context.subscriptions.push( collection, output );

    // One run per saved file at a time: a newer save kills the older run, and its output is dropped.
    const running = new Map();
    let warned_missing = false;

    function check( document )
    {
        if( document.languageId !== 'keel' || document.uri.scheme !== 'file' )
        {
            return;
        }

        const saved = document.uri.fsPath;
        const folder = vscode.workspace.getWorkspaceFolder( document.uri );
        const cwd = folder ? folder.uri.fsPath : path.dirname( saved );

        const config = vscode.workspace.getConfiguration( 'keel', document.uri );
        const compiler = path.resolve( cwd, config.get( 'compilerPath' ) );

        const args = [ '--check', '--diagnostics=json' ];
        for( const spec of config.get( 'packages' ) )
        {
            args.push( '--package', spec );
        }
        args.push( path.relative( cwd, saved ) );

        const previous = running.get( saved );
        if( previous )
        {
            previous.kill();
        }

        const child = child_process.execFile( compiler, args, { cwd }, ( error, stdout, stderr ) =>
        {
            if( running.get( saved ) !== child )
            {
                return;
            }
            running.delete( saved );

            if( error && error.code === 'ENOENT' )
            {
                if( !warned_missing )
                {
                    warned_missing = true;
                    vscode.window.showWarningMessage( `Keel: no compiler at ${compiler}; set keel.compilerPath.` );
                }
                return;
            }

            // 0 is clean and 1 is errors; anything else means keelc could not check the file at all.
            if( error && error.code !== 1 )
            {
                output.appendLine( `keelc ${args.join( ' ' )} (in ${cwd}) failed:` );
                output.appendLine( stderr.trim() || String( error ) );
                return;
            }

            for( const [ file, diagnostics ] of parse( stdout, cwd, saved ) )
            {
                collection.set( vscode.Uri.file( file ), diagnostics );
            }
        } );

        running.set( saved, child );
    }

    context.subscriptions.push( vscode.workspace.onDidSaveTextDocument( check ) );
    context.subscriptions.push( vscode.workspace.onDidOpenTextDocument( check ) );
    vscode.workspace.textDocuments.forEach( check );
}

function deactivate()
{
}

module.exports = { activate, deactivate };
