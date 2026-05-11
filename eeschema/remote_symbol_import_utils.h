/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
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

#ifndef REMOTE_SYMBOL_IMPORT_UTILS_H
#define REMOTE_SYMBOL_IMPORT_UTILS_H

#include <cstdint>
#include <vector>

#include <wx/filename.h>
#include <wx/string.h>

class SCH_EDIT_FRAME;
enum class LIBRARY_TABLE_TYPE;


/**
 * Replace non-alphanumeric characters (other than _ - .) with underscores.
 * Returns \a aDefault when \a aValue is empty after trimming.
 */
wxString SanitizeRemoteFileComponent( const wxString& aValue, const wxString& aDefault,
                                      bool aLower = false );

/**
 * Return the configured (or default) library prefix for remote downloads,
 * sanitized for use as a filename component.
 */
wxString RemoteLibraryPrefix();

/**
 * Write binary data to a file, creating parent directories as needed.
 */
bool WriteRemoteBinaryFile( const wxFileName& aOutput, const std::vector<uint8_t>& aPayload,
                            wxString& aError );

/**
 * Resolve and create the configured destination root directory for remote
 * symbol downloads.
 */
bool EnsureRemoteDestinationRoot( wxFileName& aOutDir, wxString& aError );

/**
 * Add or update a library table entry for a remote download library.
 * When \a aStrict is false, a missing library table is not treated as an error.
 */
bool EnsureRemoteLibraryEntry( LIBRARY_TABLE_TYPE aTableType, const wxFileName& aLibraryPath,
                                const wxString& aNickname, bool aGlobalTable, bool aStrict,
                                wxString& aError );

/**
 * Place a symbol from a remote download into the schematic editor.
 */
bool PlaceRemoteDownloadedSymbol( SCH_EDIT_FRAME* aFrame, const wxString& aNickname,
                                  const wxString& aLibItemName, wxString& aError );

#endif // REMOTE_SYMBOL_IMPORT_UTILS_H
