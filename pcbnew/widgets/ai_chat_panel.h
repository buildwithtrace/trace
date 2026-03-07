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

#include <widgets/ai_chat_panel_base.h>

class PCB_EDIT_FRAME;

/**
 * Pcbnew-specific AI chat panel implementation.
 * Extends AI_CHAT_PANEL_BASE with PCB_EDIT_FRAME-specific functionality.
 */
class AI_CHAT_PANEL : public AI_CHAT_PANEL_BASE
{
public:
    AI_CHAT_PANEL( wxWindow* aParent, PCB_EDIT_FRAME* aFrame );
    ~AI_CHAT_PANEL() override = default;

protected:
    bool ReloadFromFile( const wxString& aFileName ) override;
    bool CaptureStateForAIEdit( const wxString& aFilePath ) override;
    bool CompareAndCreateAIEditUndoEntries() override;
    bool GenerateSnapshot( const wxString& aOutputPath ) override;
    wxString GetCurrentFileName() const override;
    wxString GetAppType() const override;
    wxString ConvertToTraceFile( const wxString& aFilePath ) const override;

    /**
     * Handle file edit events with incremental diff support.
     */
    void HandleFileEditEvent( const AI_BACKEND_EVENT& aEvent, int aTabIndex ) override;

private:
    PCB_EDIT_FRAME* GetPcbFrame() const;
};

#endif // AI_CHAT_PANEL_H

