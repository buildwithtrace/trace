/*
 * This program source code file is part of Trace, an AI-native PCB design application.
 *
 * Copyright The Trace Developers, see TRACE_AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef AI_CHAT_PANEL_H
#define AI_CHAT_PANEL_H

#include <wx/panel.h>
#include <wx/string.h>
#include <wx/process.h>
#include <atomic>
#include <memory>
#include <set>
#include <widgets/ai_chat_panel_base.h>
#include <wx/tglbtn.h>
#include <kiplatform/ui.h>

class SCH_EDIT_FRAME;

/**
 * Panel providing an AI agent chat interface for the schematic editor.
 * 
 * Communicates with a local Python subprocess via stdin/stdout JSON messages.
 * The subprocess handles file I/O and communication with the remote AI server.
 */
class AI_CHAT_PANEL : public AI_CHAT_PANEL_BASE
{
public:
    AI_CHAT_PANEL( wxWindow* aParent, SCH_EDIT_FRAME* aFrame );
    ~AI_CHAT_PANEL() override;

    /**
     * Get the schematic edit frame.
     * @return Pointer to the SCH_EDIT_FRAME, cast from base class m_frame.
     */
    SCH_EDIT_FRAME* GetSchFrame() const;

protected:
    bool     ReloadFromFile( const wxString& aFileName ) override;
    bool     CaptureStateForAIEdit( const wxString& aFilePath ) override;
    bool     CompareAndCreateAIEditUndoEntries() override;
    void     AutoplaceModifiedSymbols( const std::set<std::string>& aModifiedUUIDs ) override;
    void     AnnotateAllSymbols() override;
    bool     SaveDocument() override;
    void     MarkDocumentAsSaved() override;
    bool     GenerateSnapshot( const wxString& aOutputPath ) override;
    wxString GetCurrentFileName() const override;
    wxString GetAppType() const override;
    wxString ConvertToTraceFile( const wxString& aFilePath ) const override;
    
    /**
     * Ensure schematic is saved before AI can read it.
     * Auto-saves unsaved schematics to a temp location in the user's Documents folder.
     * @return Full path to the saved .kicad_sch file, or empty if save failed.
     */
    wxString EnsureFileSavedForAI() override;

    /**
    /**
     * Handle file edit events with incremental diff support.
     */
    void HandleFileEditEvent( const AI_BACKEND_EVENT& aEvent, int aTabIndex ) override;

    /**
     * Request the list of schematic versions from the backend.
     * Results will be emitted as a versions_list event.
     */
    void RequestVersionList() override;

    /**
     * Request to restore a specific schematic version.
     * @param versionId The version ID to restore.
     */
    void RestoreVersion( const wxString& versionId ) override;

    /**
     * Save the current schematic state as a version in the database.
     * Called automatically after AI edits are completed.
     * @param aDescription Description of the changes (e.g., from AI edit summary)
     */
    void saveVersionToDatabase( const wxString& aDescription ) override;

private:
    /**
     * Generate a PNG snapshot of the current schematic view.
     * @param aOutputPath Path where the PNG file should be saved.
     * @return True if successful, false otherwise.
     */
    bool GenerateSchematicSnapshot( const wxString& aOutputPath );
};

#endif // AI_CHAT_PANEL_H
