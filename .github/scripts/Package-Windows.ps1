[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
            "${ProjectRoot}/release/${ProductName}-*-windows-*.exe"
        )
    }

    Remove-Item @RemoveArgs

    Log-Group "Archiving ${ProductName}..."
    $CompressArgs = @{
        Path = (Get-ChildItem -Path "${ProjectRoot}/release/${Configuration}" -Exclude "${OutputName}*.*")
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Log-Group

    $IsccFile = "${ProjectRoot}/build_${Target}/installer.iss"
    if ( ! ( Test-Path -Path $IsccFile ) ) {
        throw "Inno Setup script not found at ${IsccFile}. Configure the project before packaging."
    }

    $PluginFiles = "${ProjectRoot}/release/${Configuration}/${ProductName}"
    if ( ! ( Test-Path -Path "${PluginFiles}/bin/64bit/${ProductName}.dll" ) ) {
        throw "Built plugin not found at ${PluginFiles}."
    }

    $Iscc = $null
    $IsccCommand = Get-Command iscc -ErrorAction SilentlyContinue
    if ( $IsccCommand ) {
        $Iscc = $IsccCommand.Source
    }

    $IsccCandidates = @(
        ${Iscc}
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    ) | Where-Object { $_ -and ( Test-Path -Path $_ ) } | Select-Object -First 1

    if ( ! $IsccCandidates ) {
        throw "Inno Setup compiler (ISCC.exe) was not found. Install Inno Setup 6 and re-run packaging."
    }

    Log-Group "Creating ${OutputName} installer..."
    Invoke-External $IsccCandidates $IsccFile "/O${ProjectRoot}/release" "/F${OutputName}-Installer" "/DPluginFiles=${PluginFiles}"
    Log-Group
}

Package
