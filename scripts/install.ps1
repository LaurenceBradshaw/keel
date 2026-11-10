# Installs keelc and the kl package on Windows, replacing any earlier install there:
#
#     <prefix>\bin\keelc.exe
#     <prefix>\lib\keel\<version>\kl\
#
# and adds <prefix>\bin to the user's PATH. Run from anywhere, e.g.
#
#     powershell -ExecutionPolicy Bypass -File scripts\install.ps1 [-Build debug] [-Prefix <dir>]
#
# -Build picks build\windows-release (the default) or build\windows-debug. With no keelc built
# there, kl is still installed and keelc left as it was.

param(
    [ValidateSet( 'release', 'debug' )]
    [string] $Build = 'release',
    [string] $Prefix = ( Join-Path $env:LOCALAPPDATA 'Programs\keel' )
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot

# keelc looks for kl under its version, which comes from the top-level project().
$project = Select-String -Path ( Join-Path $root 'CMakeLists.txt' ) -Pattern 'project\(\s*keel\s+VERSION\s+([0-9.]+)'
if( -not $project ) { throw 'could not read the version from CMakeLists.txt' }
$version = $project.Matches[ 0 ].Groups[ 1 ].Value

$Prefix = [IO.Path]::GetFullPath( $Prefix )
$bin    = Join-Path $Prefix 'bin'
$kl     = Join-Path $Prefix "lib\keel\$version\kl"

New-Item -ItemType Directory -Force -Path $bin | Out-Null

# keelc, and its .pdb when there is one.
$built = Join-Path $root "build\windows-$Build\bin"
$keelc = Join-Path $built 'keelc.exe'
if( Test-Path $keelc )
{
    Copy-Item -Force $keelc $bin
    $pdb = Join-Path $built 'keelc.pdb'
    $old = Join-Path $bin 'keelc.pdb'
    if( Test-Path $pdb ) { Copy-Item -Force $pdb $bin }
    elseif( Test-Path $old ) { Remove-Item -Force $old }
    Write-Host "installed $keelc"
}
else
{
    Write-Warning "no keelc at $keelc; build the windows-$Build preset first. Installing kl only."
}

# kl, replaced whole so no file removed from the source lingers.
if( Test-Path $kl ) { Remove-Item -Recurse -Force $kl }
New-Item -ItemType Directory -Force -Path $kl | Out-Null
Copy-Item -Recurse -Force ( Join-Path $root 'keel_stl\src\*' ) $kl
Write-Host "installed kl $version to $kl"

# PATH: the user's, for new shells, and this one's. Read and written raw, so %VAR% entries survive.
$key      = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey( 'Environment', $true )
$userPath = $key.GetValue( 'Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames )
$entries  = @( $userPath -split ';' | Where-Object { $_ } )
if( $entries | Where-Object { [Environment]::ExpandEnvironmentVariables( $_ ).TrimEnd( '\' ) -ieq $bin } )
{
    Write-Host "$bin is already on PATH"
}
else
{
    $key.SetValue( 'Path', ( ( $entries + $bin ) -join ';' ), [Microsoft.Win32.RegistryValueKind]::ExpandString )
    # Tell running programs (Explorer, and so new shells) that the environment changed.
    [Environment]::SetEnvironmentVariable( 'KEEL_INSTALL_TMP', '1', 'User' )
    [Environment]::SetEnvironmentVariable( 'KEEL_INSTALL_TMP', $null, 'User' )
    Write-Host "added $bin to the user PATH; open a new shell to pick it up"
}
$key.Close()
if( -not ( $env:Path -split ';' | Where-Object { $_.TrimEnd( '\' ) -ieq $bin } ) ) { $env:Path += ";$bin" }
