/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2004 Jean-Pierre Charras, jaen-pierre.charras@gipsa-lab.inpg.com
 * Copyright (C) 2008 Wayne Stambaugh <stambaughw@gmail.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * Copyright The Trace Developers, see TRACE_AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

/**
 * @file gestfich.cpp
 * @brief Functions for file management
 */

#include <wx/mimetype.h>
#include <wx/dir.h>

#include <pgm_base.h>
#include <confirm.h>
#include <core/arraydim.h>
#include <gestfich.h>
#include <string_utils.h>
#include <launch_ext.h>
#include "wx/tokenzr.h"
#include <wildcards_and_files_ext.h>
#include <python_manager.h>
#include <paths.h>
#include <config.h>
#include <wx/log.h>
#include <wx/arrstr.h>

#include <wx/wfstream.h>
#include <wx/fs_zip.h>
#include <wx/zipstrm.h>

#include <filesystem>

void QuoteString( wxString& string )
{
    if( !string.StartsWith( wxT( "\"" ) ) )
    {
        string.Prepend ( wxT( "\"" ) );
        string.Append ( wxT( "\"" ) );
    }
}


wxString FindKicadFile( const wxString& shortname )
{
    // Test the presence of the file in the directory shortname of
    // the KiCad binary path.
#ifndef __WXMAC__
    wxString fullFileName = Pgm().GetExecutablePath() + shortname;
#else
    wxString fullFileName = Pgm().GetExecutablePath() + wxT( "Contents/MacOS/" ) + shortname;
#endif
    if( wxFileExists( fullFileName ) )
        return fullFileName;

    if( wxGetEnv( wxT( "KICAD_RUN_FROM_BUILD_DIR" ), nullptr ) )
    {
        wxFileName buildDir( Pgm().GetExecutablePath(), shortname );
        buildDir.RemoveLastDir();
#ifndef __WXMSW__
        buildDir.AppendDir( shortname );
#else
        buildDir.AppendDir( shortname.BeforeLast( '.' ) );
#endif

        if( buildDir.GetDirs().Last() == "pl_editor" )
        {
            buildDir.RemoveLastDir();
            buildDir.AppendDir( "pagelayout_editor" );
        }

        if( wxFileExists( buildDir.GetFullPath() ) )
            return buildDir.GetFullPath();
    }

    // Test the presence of the file in the directory shortname
    // defined by the environment variable KiCad.
    if( Pgm().IsKicadEnvVariableDefined() )
    {
        fullFileName = Pgm().GetKicadEnvVariable() + shortname;

        if( wxFileExists( fullFileName ) )
            return fullFileName;
    }

#if defined( __WINDOWS__ )
    // KiCad can be installed highly portably on Windows, anywhere and concurrently
    // either the "kicad file" is immediately adjacent to the exe or it's not a valid install
    return shortname;
#else

    // Path list for KiCad binary files
    const static wxChar* possibilities[] = {
#if defined( __WXMAC__ )
        // all internal paths are relative to main bundle kicad.app
        wxT( "Contents/Applications/pcbnew.app/Contents/MacOS/" ),
        wxT( "Contents/Applications/eeschema.app/Contents/MacOS/" ),
        wxT( "Contents/Applications/gerbview.app/Contents/MacOS/" ),
        wxT( "Contents/Applications/bitmap2component.app/Contents/MacOS/" ),
        wxT( "Contents/Applications/pcb_calculator.app/Contents/MacOS/" ),
        wxT( "Contents/Applications/pl_editor.app/Contents/MacOS/" ),
#else
        wxT( "/usr/bin/" ),
        wxT( "/usr/local/bin/" ),
        wxT( "/usr/local/kicad/bin/" ),
#endif
    };

    // find binary file from possibilities list:
    for( unsigned i=0;  i<arrayDim(possibilities);  ++i )
    {
#ifndef __WXMAC__
        fullFileName = possibilities[i] + shortname;
#else
        // make relative paths absolute
        fullFileName = Pgm().GetExecutablePath() + possibilities[i] + shortname;
#endif

        if( wxFileExists( fullFileName ) )
            return fullFileName;
    }

    return shortname;

#endif
}


int ExecuteFile( const wxString& aEditorName, const wxString& aFileName, wxProcess* aCallback,
                 bool aFileForKicad )
{
    wxString              fullEditorName;
    std::vector<wxString> params;

#ifdef __UNIX__
    wxString param;
    bool     inSingleQuotes = false;
    bool     inDoubleQuotes = false;

    auto pushParam =
            [&]()
            {
                if( !param.IsEmpty() )
                {
                    params.push_back( param );
                    param.clear();
                }
            };

    for( wxUniChar ch : aEditorName )
    {
        if( inSingleQuotes )
        {
            if( ch == '\'' )
            {
                pushParam();
                inSingleQuotes = false;
                continue;
            }
            else
            {
                param += ch;
            }
        }
        else if( inDoubleQuotes )
        {
            if( ch == '"' )
            {
                pushParam();
                inDoubleQuotes = false;
            }
            else
            {
                param += ch;
            }
        }
        else if( ch == '\'' )
        {
            pushParam();
            inSingleQuotes = true;
        }
        else if( ch == '"' )
        {
            pushParam();
            inDoubleQuotes = true;
        }
        else if( ch == ' ' )
        {
            pushParam();
        }
        else
        {
            param += ch;
        }
    }

    pushParam();

    if( aFileForKicad )
        fullEditorName = FindKicadFile( params[0] );
    else
        fullEditorName = params[0];

    params.erase( params.begin() );
#else

    if( aFileForKicad )
        fullEditorName = FindKicadFile( aEditorName );
    else
        fullEditorName = aEditorName;
#endif

    if( wxFileExists( fullEditorName ) )
    {
        std::vector<const wchar_t*> args;

        args.emplace_back( fullEditorName.wc_str() );

        if( !params.empty() )
        {
            for( const wxString& p : params )
                args.emplace_back( p.wc_str() );
        }

        if( !aFileName.IsEmpty() )
            args.emplace_back( aFileName.wc_str() );

        args.emplace_back( nullptr );

        return wxExecute( const_cast<wchar_t**>( args.data() ), wxEXEC_ASYNC, aCallback );
    }

    wxString msg;
    msg.Printf( _( "Command '%s' could not be found." ), fullEditorName );
    DisplayErrorMessage( nullptr, msg );
    return -1;
}


bool OpenPDF( const wxString& file )
{
    wxString msg;
    wxString filename = file;

    Pgm().ReadPdfBrowserInfos();

    if( Pgm().UseSystemPdfBrowser() )
    {
        if( !LaunchExternal( filename ) )
        {
            msg.Printf( _( "Unable to find a PDF viewer for '%s'." ), filename );
            DisplayErrorMessage( nullptr, msg );
            return false;
        }
    }
    else
    {
        const wchar_t* args[3];

        args[0] = Pgm().GetPdfBrowserName().wc_str();
        args[1] = filename.wc_str();
        args[2] = nullptr;

        if( wxExecute( const_cast<wchar_t**>( args ) ) == -1 )
        {
            msg.Printf( _( "Problem while running the PDF viewer '%s'." ), args[0] );
            DisplayErrorMessage( nullptr, msg );
            return false;
        }
    }

    return true;
}


void KiCopyFile( const wxString& aSrcPath, const wxString& aDestPath, wxString& aErrors )
{
    if( !wxCopyFile( aSrcPath, aDestPath ) )
    {
        wxString msg;

        if( !aErrors.IsEmpty() )
            aErrors += "\n";

        msg.Printf( _( "Cannot copy file '%s'." ), aDestPath );
        aErrors += msg;
    }
}


wxString QuoteFullPath( wxFileName& fn, wxPathFormat format )
{
    return wxT( "\"" ) + fn.GetFullPath( format ) + wxT( "\"" );
}


bool RmDirRecursive( const wxString& aFileName, wxString* aErrors )
{
    namespace fs = std::filesystem;

    std::string rmDir = aFileName.ToStdString();

    if( rmDir.length() < 3 )
    {
        if( aErrors )
            *aErrors = _( "Invalid directory name, cannot remove root" );

        return false;
    }

    if( !fs::exists( rmDir ) )
    {
        if( aErrors )
            *aErrors = wxString::Format( _( "Directory '%s' does not exist" ), aFileName );

        return false;
    }

    fs::path path( rmDir );

    if( !fs::is_directory( path ) )
    {
        if( aErrors )
            *aErrors = wxString::Format( _( "'%s' is not a directory" ), aFileName );

        return false;
    }

    try
    {
        fs::remove_all( path );
    }
    catch( const fs::filesystem_error& e )
    {
        if( aErrors )
            *aErrors = wxString::Format( _( "Error removing directory '%s': %s" ),
                                         aFileName, e.what() );

        return false;
    }

    return true;
}


bool CopyDirectory( const wxString& aSourceDir, const wxString& aDestDir, wxString& aErrors )
{
    wxDir dir( aSourceDir );

    if( !dir.IsOpened() )
    {
        aErrors += wxString::Format( _( "Could not open source directory: %s" ), aSourceDir );
        aErrors += wxT( "\n" );
        return false;
    }

    if( !wxFileName::Mkdir( aDestDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
    {
        aErrors += wxString::Format( _( "Could not create destination directory: %s" ), aDestDir );
        aErrors += wxT( "\n" );
        return false;
    }

    wxString filename;
    bool     cont = dir.GetFirst( &filename );

    while( cont )
    {
        wxString sourcePath = aSourceDir + wxFileName::GetPathSeparator() + filename;
        wxString destPath = aDestDir + wxFileName::GetPathSeparator() + filename;

        if( wxFileName::DirExists( sourcePath ) )
        {
            // Recursively copy subdirectories
            if( !CopyDirectory( sourcePath, destPath, aErrors ) )
                return false;
        }
        else
        {
            // Copy files
            if( !wxCopyFile( sourcePath, destPath ) )
            {
                aErrors += wxString::Format( _( "Could not copy file: %s to %s" ),
                                             sourcePath,
                                             destPath );
                return false;
            }
        }

        cont = dir.GetNext( &filename );
    }

    return true;
}


bool CopyFilesOrDirectory( const wxString& aSourcePath, const wxString& aDestDir, wxString& aErrors,
                           int& aFileCopiedCount, const std::vector<wxString>& aExclusions )
{
    // Parse source path and determine if it's a directory
    wxFileName sourceFn( aSourcePath );
    wxString   sourcePath = sourceFn.GetFullPath();
    bool       isSourceDirectory = wxFileName::DirExists( sourcePath );
    wxString   baseDestDir = aDestDir;

    auto performCopy = [&]( const wxString& src, const wxString& dest ) -> bool
    {
        if( wxCopyFile( src, dest ) )
        {
            aFileCopiedCount++;
            return true;
        }

        aErrors += wxString::Format( _( "Could not copy file: %s to %s" ), src, dest );
        aErrors += wxT( "\n" );
        return false;
    };

    auto processEntries = [&]( const wxString& srcDir, const wxString& pattern,
                               const wxString& destDir ) -> bool
    {
        wxDir dir( srcDir );

        if( !dir.IsOpened() )
        {
            aErrors += wxString::Format( _( "Could not open source directory: %s" ), srcDir );
            aErrors += wxT( "\n" );
            return false;
        }

        wxString filename;
        bool     success = true;

        // Find all entries matching pattern (files + directories + hidden items)
        bool cont = dir.GetFirst( &filename, pattern, wxDIR_FILES | wxDIR_DIRS | wxDIR_HIDDEN );

        while( cont )
        {
            const wxString entrySrc = srcDir + wxFileName::GetPathSeparator() + filename;
            const wxString entryDest = destDir + wxFileName::GetPathSeparator() + filename;

            // Apply exclusion filters
            bool exclude =
                    filename.Matches( wxT( "~*.lck" ) ) || filename.Matches( wxT( "*.lck" ) );

            for( const auto& exclusion : aExclusions )
            {
                if( entrySrc.Matches( exclusion ) )
                {
                    exclude = true;
                    break;
                }
            }

            if( !exclude )
            {
                if( wxFileName::DirExists( entrySrc ) )
                {
                    // Recursively process subdirectories
                    if( !CopyFilesOrDirectory( entrySrc, destDir, aErrors, aFileCopiedCount,
                                               aExclusions ) )
                    {
                        aErrors += wxString::Format( _( "Could not copy directory: %s to %s" ),
                                                     entrySrc, entryDest );
                        aErrors += wxT( "\n" );

                        success = false;
                    }
                }
                else
                {
                    // Copy individual files
                    if( !performCopy( entrySrc, entryDest ) )
                    {
                        success = false;
                    }
                }
            }

            cont = dir.GetNext( &filename );
        }

        return success;
    };

    // If copying a directory, append its name to destination path
    if( isSourceDirectory )
    {
        wxString sourceDirName = sourceFn.GetFullName();
        baseDestDir = wxFileName( aDestDir, sourceDirName ).GetFullPath();
    }

    // Create destination directory hierarchy
    if( !wxFileName::Mkdir( baseDestDir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL ) )
    {
        aErrors +=
                wxString::Format( _( "Could not create destination directory: %s" ), baseDestDir );
        aErrors += wxT( "\n" );

        return false;
    }

    // Execute appropriate copy operation based on source type
    if( !isSourceDirectory )
    {
        const wxString fileName = sourceFn.GetFullName();

        // Handle wildcard patterns in filenames
        if( fileName.Contains( '*' ) || fileName.Contains( '?' ) )
        {
            const wxString dirPath = sourceFn.GetPath();

            if( !wxFileName::DirExists( dirPath ) )
            {
                aErrors += wxString::Format( _( "Source directory does not exist: %s" ), dirPath );
                aErrors += wxT( "\n" );

                return false;
            }
            // Process all matching files in source directory
            return processEntries( dirPath, fileName, baseDestDir );
        }

        // Single file copy operation
        return performCopy( sourcePath, wxFileName( baseDestDir, fileName ).GetFullPath() );
    }

    // Full directory copy operation
    return processEntries( sourcePath, wxEmptyString, baseDestDir );
}


bool AddDirectoryToZip( wxZipOutputStream& aZip, const wxString& aSourceDir, wxString& aErrors,
                        const wxString& aParentDir )
{
    wxDir dir( aSourceDir );

    if( !dir.IsOpened() )
    {
        aErrors += wxString::Format( _( "Could not open source directory: %s" ), aSourceDir );
        aErrors += "\n";
        return false;
    }

    wxString filename;
    bool     cont = dir.GetFirst( &filename );

    while( cont )
    {
        wxString sourcePath = aSourceDir + wxFileName::GetPathSeparator() + filename;
        wxString zipPath = aParentDir + filename;

        if( wxFileName::DirExists( sourcePath ) )
        {
            // Add directory entry to the ZIP file
            aZip.PutNextDirEntry( zipPath + "/" );

            // Recursively add subdirectories
            if( !AddDirectoryToZip( aZip, sourcePath, aErrors, zipPath + "/" ) )
                return false;
        }
        else
        {
            // Add file entry to the ZIP file
            aZip.PutNextEntry( zipPath );
            wxFFileInputStream fileStream( sourcePath );

            if( !fileStream.IsOk() )
            {
                aErrors += wxString::Format( _( "Could not read file: %s" ), sourcePath );
                return false;
            }

            aZip.Write( fileStream );
        }

        cont = dir.GetNext( &filename );
    }

    return true;
}


bool ConvertKicadSchToTraceSch( const wxString& aKicadSchPath )
{
    wxFileName kicadSchFile( aKicadSchPath );
    
    // Only process .kicad_sch files
    if( kicadSchFile.GetExt() != FILEEXT::KiCadSchematicFileExtension )
        return false;
    
    // Find Python interpreter
    wxString pythonPath = PYTHON_MANAGER::FindPythonInterpreter();
    
    if( pythonPath.IsEmpty() )
    {
        wxLogWarning( wxS( "Could not find Python interpreter to convert kicad_sch to trace_sch" ) );
        return false;
    }
    
    // Determine trace_sch output path
    wxFileName traceSchFile( kicadSchFile );
    traceSchFile.SetExt( FILEEXT::TraceSchematicFileExtension );
    wxString traceSchPath = traceSchFile.GetFullPath();
    
    // Find trace.py script using unified runtime path detection
    // PATHS::GetStockDataPath() automatically handles:
    // - Windows install: C:\Program Files\Trace\X.X\share\trace
    // - Windows dev: Build directory when KICAD_RUN_FROM_BUILD_DIR is set
    // - macOS bundle: Trace.app/Contents/SharedSupport
    // - Linux: /usr/share/trace
    wxString stockDataPath = PATHS::GetStockDataPath();
    wxString scriptPath = stockDataPath + wxS( "/scripting/trace/eeschema/trace.py" );
    
    wxFileName traceScript( scriptPath );
    
    if( !traceScript.FileExists() )
    {
        wxLogWarning( wxS( "Could not find trace.py script at %s" ), scriptPath );
        return false;
    }
    
    // Build command: python trace/trace.py -f kicad_sch -t trace_sch input.kicad_sch output.trace_sch
    wxString command = wxString::Format( wxT( "\"%s\" \"%s\" -f kicad_sch -t trace_sch \"%s\" \"%s\"" ),
                                        pythonPath,
                                        traceScript.GetFullPath(),
                                        aKicadSchPath,
                                        traceSchPath );
    
    // Log command for debugging
    wxLogTrace( wxS( "TraceConversion" ), wxString::Format( wxS( "Executing: %s" ), command ) );
    
    // Execute the conversion (synchronously)
    wxArrayString output;
    wxArrayString errors;
    long exitCode = wxExecute( command, output, errors, wxEXEC_SYNC );
    
    if( exitCode != 0 )
    {
        wxString errorMsg;
        
        // Collect all error lines (Python tracebacks are multi-line)
        if( !errors.IsEmpty() )
        {
            for( size_t i = 0; i < errors.GetCount(); ++i )
            {
                if( !errorMsg.IsEmpty() )
                    errorMsg += wxS( "\n" );
                errorMsg += errors[i];
            }
        }
        
        // Also check output, as Python errors sometimes go to stdout
        if( !output.IsEmpty() && errorMsg.IsEmpty() )
        {
            for( size_t i = 0; i < output.GetCount(); ++i )
            {
                if( !errorMsg.IsEmpty() )
                    errorMsg += wxS( "\n" );
                errorMsg += output[i];
            }
        }
        
        if( errorMsg.IsEmpty() )
            errorMsg = wxString::Format( wxS( "Exit code: %ld" ), exitCode );
        
        // Log the full error message
        wxLogWarning( wxString::Format( wxS( "Failed to convert kicad_sch to trace_sch:\n%s" ), errorMsg ) );
        
        // Also log the command that was executed for debugging
        wxLogTrace( wxS( "TraceConversion" ), wxString::Format( wxS( "Command: %s" ), command ) );
        wxLogTrace( wxS( "TraceConversion" ), wxString::Format( wxS( "Exit code: %ld" ), exitCode ) );
        
        return false;
    }
    
    // Verify output file was created
    if( !traceSchFile.FileExists() )
    {
        wxLogWarning( wxS( "trace_sch file was not created after conversion" ) );
        return false;
    }
    
    return true;
}


bool ConvertTraceSchToKicadSch( const wxString& aTraceSchPath )
{
    wxFileName traceSchFile( aTraceSchPath );
    
    // Only process .trace_sch files
    if( traceSchFile.GetExt() != FILEEXT::TraceSchematicFileExtension )
        return false;
    
    // Determine kicad_sch output path (same directory, different extension)
    wxFileName kicadSchFile( traceSchFile );
    kicadSchFile.SetExt( FILEEXT::KiCadSchematicFileExtension );
    wxString kicadSchPath = kicadSchFile.GetFullPath();
    
    // Find Python interpreter
    wxString pythonPath = PYTHON_MANAGER::FindPythonInterpreter();
    
    if( pythonPath.IsEmpty() )
    {
        wxLogWarning( wxS( "Could not find Python interpreter to convert trace_sch to kicad_sch" ) );
        return false;
    }
    
    // Find trace.py script using unified runtime path detection
    wxString stockDataPath = PATHS::GetStockDataPath();
    wxString scriptPath = stockDataPath + wxS( "/scripting/trace/eeschema/trace.py" );
    
    wxFileName traceScript( scriptPath );
    
    if( !traceScript.FileExists() )
    {
        wxLogWarning( wxS( "Could not find trace.py script at %s" ), scriptPath );
        return false;
    }
    
    // Check if output kicad_sch file exists - if so, pass it as existing_sch for merging
    wxString existingSchFlag;
    if( wxFileExists( kicadSchPath ) )
    {
        existingSchFlag = wxString::Format( wxT( " --existing-sch \"%s\"" ), kicadSchPath );
    }
    
    wxString command = wxString::Format( wxT( "\"%s\" \"%s\" -f trace_sch -t kicad_sch%s \"%s\" \"%s\"" ),
                                        pythonPath,
                                        traceScript.GetFullPath(),
                                        existingSchFlag,
                                        aTraceSchPath,
                                        kicadSchPath );
    
    // Log command for debugging
    wxLogTrace( wxS( "TraceConversion" ), wxString::Format( wxS( "Executing: %s" ), command ) );
    
    // Execute the conversion (synchronously)
    wxArrayString output;
    wxArrayString errors;
    long exitCode = wxExecute( command, output, errors, wxEXEC_SYNC );
    
    if( exitCode != 0 )
    {
        wxString errorMsg;
        
        // Collect all error lines (Python tracebacks are multi-line)
        if( !errors.IsEmpty() )
        {
            for( size_t i = 0; i < errors.GetCount(); ++i )
            {
                if( !errorMsg.IsEmpty() )
                    errorMsg += wxS( "\n" );
                errorMsg += errors[i];
            }
        }
        
        // Also check output, as Python errors sometimes go to stdout
        if( !output.IsEmpty() && errorMsg.IsEmpty() )
        {
            for( size_t i = 0; i < output.GetCount(); ++i )
            {
                if( !errorMsg.IsEmpty() )
                    errorMsg += wxS( "\n" );
                errorMsg += output[i];
            }
        }
        
        if( errorMsg.IsEmpty() )
            errorMsg = wxString::Format( wxS( "Exit code: %ld" ), exitCode );
        
        // Log the full error message
        wxLogWarning( wxString::Format( wxS( "Failed to convert trace_sch to kicad_sch:\n%s" ), errorMsg ) );
        
        // Also log the command that was executed for debugging
        wxLogTrace( wxS( "TraceConversion" ), wxString::Format( wxS( "Command: %s" ), command ) );
        wxLogTrace( wxS( "TraceConversion" ), wxString::Format( wxS( "Exit code: %ld" ), exitCode ) );
        
        return false;
    }
    
    // Verify output file was created
    if( !kicadSchFile.FileExists() )
    {
        wxLogWarning( wxS( "kicad_sch file was not created after conversion" ) );
        return false;
    }
    
    return true;
}


bool ConvertKicadPcbToTracePcb( const wxString& aKicadPcbPath )
{
    wxFileName kicadPcbFile( aKicadPcbPath );
    
    // Only process .kicad_pcb files
    if( kicadPcbFile.GetExt() != FILEEXT::KiCadPcbFileExtension )
        return false;
    
    // Find Python interpreter
    wxString pythonPath = PYTHON_MANAGER::FindPythonInterpreter();
    
    if( pythonPath.IsEmpty() )
    {
        wxLogWarning( wxS( "Could not find Python interpreter to convert kicad_pcb to trace_pcb" ) );
        return false;
    }
    
    // Determine trace_pcb output path
    wxFileName tracePcbFile( kicadPcbFile );
    tracePcbFile.SetExt( FILEEXT::TracePcbFileExtension );
    wxString tracePcbPath = tracePcbFile.GetFullPath();
    
    // Find trace.py script
    // Try multiple locations in order:
    // Find trace.py script using unified runtime path detection
    wxString stockDataPath = PATHS::GetStockDataPath();
    wxString scriptPath = stockDataPath + wxS( "/scripting/trace/pcbnew/trace.py" );
    
    wxFileName traceScript( scriptPath );
    
    if( !traceScript.FileExists() )
    {
        wxLogWarning( wxS( "Could not find trace.py script at %s" ), scriptPath );
        return false;
    }
    
    // Build command: python trace/pcbnew/trace.py -f kicad_pcb -t trace_pcb input.kicad_pcb output.trace_pcb
    wxString command = wxString::Format( wxT( "\"%s\" \"%s\" -f kicad_pcb -t trace_pcb \"%s\" \"%s\"" ),
                                        pythonPath,
                                        traceScript.GetFullPath(),
                                        aKicadPcbPath,
                                        tracePcbPath );
    
    // Log command for debugging
    wxLogTrace( wxS( "TraceConversion" ), wxString::Format( wxS( "Executing: %s" ), command ) );
    
    // Execute the conversion (synchronously)
    wxArrayString output;
    wxArrayString errors;
    long exitCode = wxExecute( command, output, errors, wxEXEC_SYNC );
    
    if( exitCode != 0 )
    {
        wxString errorMsg;
        
        // Collect all error lines (Python tracebacks are multi-line)
        if( !errors.IsEmpty() )
        {
            for( size_t i = 0; i < errors.GetCount(); ++i )
            {
                if( !errorMsg.IsEmpty() )
                    errorMsg += wxS( "\n" );
                errorMsg += errors[i];
            }
        }
        
        // Also check output, as Python errors sometimes go to stdout
        if( !output.IsEmpty() && errorMsg.IsEmpty() )
        {
            for( size_t i = 0; i < output.GetCount(); ++i )
            {
                if( !errorMsg.IsEmpty() )
                    errorMsg += wxS( "\n" );
                errorMsg += output[i];
            }
        }
        
        if( errorMsg.IsEmpty() )
            errorMsg = wxString::Format( wxS( "Exit code: %ld" ), exitCode );
        
        // Log the full error message
        wxLogWarning( wxString::Format( wxS( "Failed to convert kicad_pcb to trace_pcb:\n%s" ), errorMsg ) );
        
        // Also log the command that was executed for debugging
        wxLogTrace( wxS( "TraceConversion" ), wxString::Format( wxS( "Command: %s" ), command ) );
        wxLogTrace( wxS( "TraceConversion" ), wxString::Format( wxS( "Exit code: %ld" ), exitCode ) );
        
        return false;
    }
    
    // Verify output file was created
    if( !tracePcbFile.FileExists() )
    {
        wxLogWarning( wxS( "trace_pcb file was not created after conversion" ) );
        return false;
    }
    
    return true;
}


bool ConvertTracePcbToKicadPcb( const wxString& aTracePcbPath, 
                                const wxString& aKicadPcbPath,
                                const wxString& aKicadSchPath )
{
    wxFileName tracePcbFile( aTracePcbPath );
    
    // Only process .trace_pcb files
    if( tracePcbFile.GetExt() != FILEEXT::TracePcbFileExtension )
        return false;
    
    // Determine kicad_pcb output path (same directory, different extension, or use provided path)
    wxFileName kicadPcbFile;
    wxString kicadPcbPath;
    
    if( !aKicadPcbPath.IsEmpty() )
    {
        kicadPcbFile = wxFileName( aKicadPcbPath );
        kicadPcbPath = aKicadPcbPath;
    }
    else
    {
        kicadPcbFile = wxFileName( tracePcbFile );
        kicadPcbFile.SetExt( FILEEXT::KiCadPcbFileExtension );
        kicadPcbPath = kicadPcbFile.GetFullPath();
    }
    
    // Find Python interpreter
    wxString pythonPath = PYTHON_MANAGER::FindPythonInterpreter();
    
    if( pythonPath.IsEmpty() )
    {
        wxLogWarning( wxS( "Could not find Python interpreter to convert trace_pcb to kicad_pcb" ) );
        return false;
    }
    
    // Find trace.py script using unified runtime path detection
    wxString stockDataPath = PATHS::GetStockDataPath();
    wxString scriptPath = stockDataPath + wxS( "/scripting/trace/pcbnew/trace.py" );
    
    wxFileName traceScript( scriptPath );
    
    if( !traceScript.FileExists() )
    {
        wxLogWarning( wxS( "Could not find trace.py script at %s" ), scriptPath );
        return false;
    }
    
    // Build command: python trace/pcbnew/trace.py -f trace_pcb -t kicad_pcb input.trace_pcb output.kicad_pcb [--existing-pcb existing.kicad_pcb] [--kicad-sch file.kicad_sch]
    wxString command;
    
    if( !aKicadPcbPath.IsEmpty() && !aKicadSchPath.IsEmpty() )
    {
        // Both existing_pcb and kicad_sch provided
        command = wxString::Format( wxT( "\"%s\" \"%s\" -f trace_pcb -t kicad_pcb --existing-pcb \"%s\" --kicad-sch \"%s\" \"%s\" \"%s\"" ),
                                    pythonPath,
                                    traceScript.GetFullPath(),
                                    aKicadPcbPath,
                                    aKicadSchPath,
                                    aTracePcbPath,
                                    kicadPcbPath );
    }
    else if( !aKicadPcbPath.IsEmpty() )
    {
        // Only existing_pcb provided
        command = wxString::Format( wxT( "\"%s\" \"%s\" -f trace_pcb -t kicad_pcb --existing-pcb \"%s\" \"%s\" \"%s\"" ),
                                    pythonPath,
                                    traceScript.GetFullPath(),
                                    aKicadPcbPath,
                                    aTracePcbPath,
                                    kicadPcbPath );
    }
    else
    {
        // No existing files provided
        command = wxString::Format( wxT( "\"%s\" \"%s\" -f trace_pcb -t kicad_pcb \"%s\" \"%s\"" ),
                                    pythonPath,
                                    traceScript.GetFullPath(),
                                    aTracePcbPath,
                                    kicadPcbPath );
    }
    
    // Log command for debugging
    wxLogTrace( wxS( "TraceConversion" ), wxString::Format( wxS( "Executing: %s" ), command ) );
    
    // Execute the conversion (synchronously)
    wxArrayString output;
    wxArrayString errors;
    long exitCode = wxExecute( command, output, errors, wxEXEC_SYNC );
    
    if( exitCode != 0 )
    {
        wxString errorMsg;
        
        // Collect all error lines (Python tracebacks are multi-line)
        if( !errors.IsEmpty() )
        {
            for( size_t i = 0; i < errors.GetCount(); ++i )
            {
                if( !errorMsg.IsEmpty() )
                    errorMsg += wxS( "\n" );
                errorMsg += errors[i];
            }
        }
        
        // Also check output, as Python errors sometimes go to stdout
        if( !output.IsEmpty() && errorMsg.IsEmpty() )
        {
            for( size_t i = 0; i < output.GetCount(); ++i )
            {
                if( !errorMsg.IsEmpty() )
                    errorMsg += wxS( "\n" );
                errorMsg += output[i];
            }
        }
        
        if( errorMsg.IsEmpty() )
            errorMsg = wxString::Format( wxS( "Exit code: %ld" ), exitCode );
        
        // Log the full error message
        wxLogWarning( wxString::Format( wxS( "Failed to convert trace_pcb to kicad_pcb:\n%s" ), errorMsg ) );
        
        // Also log the command that was executed for debugging
        wxLogTrace( wxS( "TraceConversion" ), wxString::Format( wxS( "Command: %s" ), command ) );
        wxLogTrace( wxS( "TraceConversion" ), wxString::Format( wxS( "Exit code: %ld" ), exitCode ) );
        
        return false;
    }
    
    // Verify output file was created
    if( !kicadPcbFile.FileExists() )
    {
        wxLogWarning( wxS( "kicad_pcb file was not created after conversion" ) );
        return false;
    }
    
    return true;
}
