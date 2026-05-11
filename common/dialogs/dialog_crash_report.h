/*
 * This program source code file is part of Trace, a fork of KiCad.
 *
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

#ifndef DIALOG_CRASH_REPORT_H
#define DIALOG_CRASH_REPORT_H

#include <kicommon.h>
#include <wx/dialog.h>
#include <wx/string.h>
#include <vector>

class wxTextCtrl;
class wxButton;

/**
 * Consent dialog shown on startup when a previous session crash is detected.
 *
 * Shows the user a message explaining that a crash was detected and asks
 * whether they want to send the session log to Trace for debugging.
 * An expandable "Details" section shows the log file paths and system info.
 *
 * Returns wxID_OK if the user consents to sending, wxID_CANCEL otherwise.
 */
class KICOMMON_API DIALOG_CRASH_REPORT : public wxDialog
{
public:
    /**
     * @param aParent     Parent window (may be nullptr).
     * @param aCrashLogs  Paths to the crash log files found.
     * @param aSystemInfo JSON string of collected system information.
     */
    DIALOG_CRASH_REPORT( wxWindow* aParent,
                         const std::vector<wxString>& aCrashLogs,
                         const wxString& aSystemInfo );

    const std::vector<wxString>& GetCrashLogPaths() const { return m_crashLogs; }

private:
    void onSendReport( wxCommandEvent& aEvent );
    void onNoThanks( wxCommandEvent& aEvent );
    void onToggleDetails( wxCommandEvent& aEvent );

    std::vector<wxString> m_crashLogs;
    wxString              m_systemInfo;
    wxTextCtrl*           m_detailsCtrl;
    wxButton*             m_detailsBtn;
    bool                  m_detailsVisible;
};

#endif // DIALOG_CRASH_REPORT_H
