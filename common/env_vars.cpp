/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * Copyright The Trace Developers, see TRACE_AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <build_version.h>
#include <env_vars.h>
#include <settings/environment.h>
#include <core/kicad_algo.h>

#include <map>

#include <wx/regex.h>
#include <wx/translation.h>
#include <wx/utils.h>

/**
 * List of pre-defined environment variables.
 *
 * @todo Instead of defining these values here, extract them from elsewhere in the program
 * (where they are originally defined).
 */
static const std::vector<wxString> predefinedEnvVars = {
    wxS( "KIPRJMOD" ),
    // Trace versioned variables (shown to users)
    ENV_VAR::GetTraceVersionedEnvVarName( wxS( "SYMBOL_DIR" ) ),
    ENV_VAR::GetTraceVersionedEnvVarName( wxS( "3DMODEL_DIR" ) ),
    ENV_VAR::GetTraceVersionedEnvVarName( wxS( "FOOTPRINT_DIR" ) ),
    ENV_VAR::GetTraceVersionedEnvVarName( wxS( "TEMPLATE_DIR" ) ),
    ENV_VAR::GetTraceVersionedEnvVarName( wxS( "3RD_PARTY" ) ),
    ENV_VAR::GetTraceVersionedEnvVarName( wxS( "DESIGN_BLOCK_DIR" ) ),
    // KiCad versioned variables (for backwards compatibility)
    ENV_VAR::GetKicadVersionedEnvVarName( wxS( "SYMBOL_DIR" ) ),
    ENV_VAR::GetKicadVersionedEnvVarName( wxS( "3DMODEL_DIR" ) ),
    ENV_VAR::GetKicadVersionedEnvVarName( wxS( "FOOTPRINT_DIR" ) ),
    ENV_VAR::GetKicadVersionedEnvVarName( wxS( "TEMPLATE_DIR" ) ),
    ENV_VAR::GetKicadVersionedEnvVarName( wxS( "3RD_PARTY" ) ),
    ENV_VAR::GetKicadVersionedEnvVarName( wxS( "DESIGN_BLOCK_DIR" ) ),
    // User template dir (both TRACE and KICAD versions for compatibility)
    wxS( "TRACE_USER_TEMPLATE_DIR" ),
    wxS( "KICAD_USER_TEMPLATE_DIR" ),
    wxS( "KICAD_PTEMPLATES" ),
};


bool ENV_VAR::IsEnvVarImmutable( const wxString& aEnvVar )
{
    for( const wxString& s : predefinedEnvVars )
    {
        if( s == aEnvVar )
            return true;
    }

    return false;
}


const std::vector<wxString>& ENV_VAR::GetPredefinedEnvVars()
{
    return predefinedEnvVars;
}


void ENV_VAR::GetEnvVarAutocompleteTokens( wxArrayString* aVars )
{
    for( const wxString& var : ENV_VAR::GetPredefinedEnvVars() )
    {
        if( !alg::contains( *aVars, var ) )
            aVars->push_back( var );
    }
}


wxString ENV_VAR::GetVersionedEnvVarName( const wxString& aBaseName )
{
    // Return the Trace versioned name by default
    return GetTraceVersionedEnvVarName( aBaseName );
}


wxString ENV_VAR::GetTraceVersionedEnvVarName( const wxString& aBaseName )
{
    // Get the Trace major version from the major.minor string
    wxString traceMajorMinor = GetTraceMajorMinorVersion();
    long major = 0;
    traceMajorMinor.BeforeFirst( '.' ).ToLong( &major );

    return wxString::Format( "TRACE%ld_%s", major, aBaseName );
}


wxString ENV_VAR::GetKicadVersionedEnvVarName( const wxString& aBaseName )
{
    int version = 0;
    std::tie(version, std::ignore, std::ignore) = GetMajorMinorPatchTuple();

    return wxString::Format( "KICAD%d_%s", version, aBaseName );
}


std::optional<wxString> ENV_VAR::GetVersionedEnvVarValue( const ENV_VAR_MAP& aMap,
                                                          const wxString& aBaseName )
{
    // First try exact match with TRACE version
    wxString traceMatch = ENV_VAR::GetTraceVersionedEnvVarName( aBaseName );

    if( aMap.count( traceMatch ) )
        return aMap.at( traceMatch ).GetValue();

    // Then try exact match with KICAD version
    wxString kicadMatch = ENV_VAR::GetKicadVersionedEnvVarName( aBaseName );

    if( aMap.count( kicadMatch ) )
        return aMap.at( kicadMatch ).GetValue();

    // Try partial match with TRACE*_
    wxString tracePartialMatch = wxString::Format( "TRACE*_%s", aBaseName );

    for( const auto& [k, v] : aMap )
    {
        if( k.Matches( tracePartialMatch ) )
            return v.GetValue();
    }

    // Try partial match with KICAD*_
    wxString kicadPartialMatch = wxString::Format( "KICAD*_%s", aBaseName );

    for( const auto& [k, v] : aMap )
    {
        if( k.Matches( kicadPartialMatch ) )
            return v.GetValue();
    }

    return std::nullopt;
}


static void initialiseEnvVarHelp( std::map<wxString, wxString>& aMap )
{
    // Set up dynamically, as we want to be able to use _() translations,
    // which can't be done statically

    // Trace versioned variables (primary)
    aMap[ENV_VAR::GetTraceVersionedEnvVarName( wxS( "FOOTPRINT_DIR" ) )] =
        _( "The base path of locally installed system footprint libraries (.pretty folders)." );

    aMap[ENV_VAR::GetTraceVersionedEnvVarName( wxS( "3DMODEL_DIR" ) )] =
        _( "The base path of system footprint 3D shapes (.3Dshapes folders)." );

    aMap[ENV_VAR::GetTraceVersionedEnvVarName( wxS( "SYMBOL_DIR" ) )] =
        _( "The base path of the locally installed symbol libraries." );

    aMap[ENV_VAR::GetTraceVersionedEnvVarName( wxS( "TEMPLATE_DIR" ) )] =
        _( "A directory containing project templates installed with Trace." );

    aMap[ENV_VAR::GetTraceVersionedEnvVarName( wxS( "3RD_PARTY" ) )] =
        _( "A directory containing 3rd party plugins, libraries and other downloadable content." );

    aMap[ENV_VAR::GetTraceVersionedEnvVarName( wxS( "DESIGN_BLOCK_DIR" ) )] =
        _( "The base path of the locally installed design block libraries." );

    aMap[ENV_VAR::GetTraceVersionedEnvVarName( wxS( "SCRIPTING_DIR" ) )] =
        _( "A directory containing system-wide scripts installed with Trace." );

    aMap[ENV_VAR::GetTraceVersionedEnvVarName( wxS( "USER_SCRIPTING_DIR" ) )] =
        _( "A directory containing user-specific scripts." );

    // KiCad versioned variables (backwards compatibility aliases)
    aMap[ENV_VAR::GetKicadVersionedEnvVarName( wxS( "FOOTPRINT_DIR" ) )] =
        _( "The base path of locally installed system footprint libraries (.pretty folders). "
           "This is an alias for backwards compatibility." );

    aMap[ENV_VAR::GetKicadVersionedEnvVarName( wxS( "3DMODEL_DIR" ) )] =
        _( "The base path of system footprint 3D shapes (.3Dshapes folders). "
           "This is an alias for backwards compatibility." );

    aMap[ENV_VAR::GetKicadVersionedEnvVarName( wxS( "SYMBOL_DIR" ) )] =
        _( "The base path of the locally installed symbol libraries. "
           "This is an alias for backwards compatibility." );

    aMap[ENV_VAR::GetKicadVersionedEnvVarName( wxS( "TEMPLATE_DIR" ) )] =
        _( "A directory containing project templates. "
           "This is an alias for backwards compatibility." );

    aMap[ENV_VAR::GetKicadVersionedEnvVarName( wxS( "3RD_PARTY" ) )] =
        _( "A directory containing 3rd party plugins, libraries and other downloadable content. "
           "This is an alias for backwards compatibility." );

    aMap[ENV_VAR::GetKicadVersionedEnvVarName( wxS( "DESIGN_BLOCK_DIR" ) )] =
        _( "The base path of the locally installed design block libraries. "
           "This is an alias for backwards compatibility." );

    aMap[ENV_VAR::GetKicadVersionedEnvVarName( wxS( "SCRIPTING_DIR" ) )] =
        _( "A directory containing system-wide scripts. "
           "This is an alias for backwards compatibility." );

    aMap[ENV_VAR::GetKicadVersionedEnvVarName( wxS( "USER_SCRIPTING_DIR" ) )] =
        _( "A directory containing user-specific scripts. "
           "This is an alias for backwards compatibility." );

    aMap[wxS( "TRACE_USER_TEMPLATE_DIR" )] =
        _( "Optional. Can be defined if you want to create your own project templates folder." );

    aMap[wxS( "KICAD_USER_TEMPLATE_DIR" )] =
        _( "Optional. Can be defined if you want to create your own project templates folder. "
           "This is an alias for backwards compatibility." );

    aMap[wxS( "KIPRJMOD" )] =
        _( "Internally defined by Trace (cannot be edited) and is set to the absolute path of the currently "
           "loaded project file.  This environment variable can be used to define files and paths relative "
           "to the currently loaded project.  For instance, ${KIPRJMOD}/libs/footprints.pretty can be "
           "defined as a folder containing a project specific footprint library named footprints.pretty." );

    // Deprecated vars
#define DEP( var ) wxString::Format( _( "Deprecated version of %s." ), var )

    aMap[wxS( "KICAD_PTEMPLATES" )] = DEP( ENV_VAR::GetTraceVersionedEnvVarName( wxS( "TEMPLATE_DIR" ) ) );
    aMap[wxS( "KISYS3DMOD" )]       = DEP( ENV_VAR::GetTraceVersionedEnvVarName( wxS( "3DMODEL_DIR" ) ) );
    aMap[wxS( "KISYSMOD" )]         = DEP( ENV_VAR::GetTraceVersionedEnvVarName( wxS( "FOOTPRINT_DIR" ) ) );
    aMap[wxS( "KICAD_SYMBOL_DIR" )] = DEP( ENV_VAR::GetTraceVersionedEnvVarName( wxS( "SYMBOL_DIR" ) ) );

#undef DEP
}


wxString ENV_VAR::LookUpEnvVarHelp( const wxString& aEnvVar )
{
    static std::map<wxString, wxString> envVarHelpText;

    if( envVarHelpText.size() == 0 )
        initialiseEnvVarHelp( envVarHelpText );

    return envVarHelpText[ aEnvVar ];
}


template<>
std::optional<double> ENV_VAR::GetEnvVar( const wxString& aEnvVarName )
{
    wxString env;

    if( wxGetEnv( aEnvVarName, &env ) )
    {
        double value;

        if( env.ToDouble( &value ) )
            return value;
    }

    return std::nullopt;
}


template<>
std::optional<wxString> ENV_VAR::GetEnvVar( const wxString& aEnvVarName )
{
    std::optional<wxString> optValue;
    wxString                env;

    if( wxGetEnv( aEnvVarName, &env ) )
        optValue = env;

    return optValue;
}
