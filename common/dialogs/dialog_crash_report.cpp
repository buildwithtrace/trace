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

#include <dialogs/dialog_crash_report.h>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/textctrl.h>
#include <wx/artprov.h>
#include <wx/statbmp.h>


DIALOG_CRASH_REPORT::DIALOG_CRASH_REPORT( wxWindow* aParent,
                                           const std::vector<wxString>& aCrashLogs,
                                           const wxString& aSystemInfo )
    : wxDialog( aParent, wxID_ANY, wxT( "Trace Crash Report" ),
                wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER ),
      m_crashLogs( aCrashLogs ),
      m_systemInfo( aSystemInfo ),
      m_detailsCtrl( nullptr ),
      m_detailsBtn( nullptr ),
      m_detailsVisible( false )
{
    wxBoxSizer* mainSizer = new wxBoxSizer( wxVERTICAL );

    // Icon + message row
    wxBoxSizer* topSizer = new wxBoxSizer( wxHORIZONTAL );

    wxStaticBitmap* icon = new wxStaticBitmap( this, wxID_ANY,
        wxArtProvider::GetBitmap( wxART_WARNING, wxART_MESSAGE_BOX ) );
    topSizer->Add( icon, 0, wxALL | wxALIGN_TOP, 10 );

    wxStaticText* message = new wxStaticText( this, wxID_ANY,
        wxT( "We noticed Trace didn't shut down properly last time.\n\n"
             "Would you like to send the session log to help us\n"
             "fix the issue? No personal project data is included." ) );
    topSizer->Add( message, 1, wxALL | wxALIGN_CENTER_VERTICAL, 10 );

    mainSizer->Add( topSizer, 0, wxEXPAND );

    // Details toggle button
    m_detailsBtn = new wxButton( this, wxID_ANY, wxT( "Show Details..." ) );
    m_detailsBtn->Bind( wxEVT_BUTTON, &DIALOG_CRASH_REPORT::onToggleDetails, this );
    mainSizer->Add( m_detailsBtn, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10 );

    // Details text (initially hidden)
    wxString detailsText;
    detailsText += wxT( "--- Log files ---\n" );

    for( const wxString& path : m_crashLogs )
    {
        detailsText += path + wxT( "\n" );
    }

    detailsText += wxT( "\n--- System information ---\n" );
    detailsText += m_systemInfo + wxT( "\n" );

    m_detailsCtrl = new wxTextCtrl( this, wxID_ANY, detailsText,
                                     wxDefaultPosition, wxSize( 500, 200 ),
                                     wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP );
    m_detailsCtrl->SetFont( wxFont( wxNORMAL_FONT->GetPointSize() - 1,
                                     wxFONTFAMILY_TELETYPE,
                                     wxFONTSTYLE_NORMAL,
                                     wxFONTWEIGHT_NORMAL ) );
    m_detailsCtrl->Hide();
    mainSizer->Add( m_detailsCtrl, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10 );

    // Action buttons
    wxBoxSizer* buttonSizer = new wxBoxSizer( wxHORIZONTAL );

    wxButton* noThanksBtn = new wxButton( this, wxID_CANCEL, wxT( "No Thanks" ) );
    noThanksBtn->Bind( wxEVT_BUTTON, &DIALOG_CRASH_REPORT::onNoThanks, this );
    buttonSizer->Add( noThanksBtn, 0, wxALL, 5 );

    buttonSizer->AddStretchSpacer();

    wxButton* sendBtn = new wxButton( this, wxID_OK, wxT( "Send Report" ) );
    sendBtn->SetDefault();
    sendBtn->Bind( wxEVT_BUTTON, &DIALOG_CRASH_REPORT::onSendReport, this );
    buttonSizer->Add( sendBtn, 0, wxALL, 5 );

    mainSizer->Add( buttonSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10 );

    SetSizerAndFit( mainSizer );
    CentreOnScreen();
}


void DIALOG_CRASH_REPORT::onSendReport( wxCommandEvent& aEvent )
{
    EndModal( wxID_OK );
}


void DIALOG_CRASH_REPORT::onNoThanks( wxCommandEvent& aEvent )
{
    EndModal( wxID_CANCEL );
}


void DIALOG_CRASH_REPORT::onToggleDetails( wxCommandEvent& aEvent )
{
    m_detailsVisible = !m_detailsVisible;

    m_detailsCtrl->Show( m_detailsVisible );
    m_detailsBtn->SetLabel( m_detailsVisible ? wxT( "Hide Details..." )
                                              : wxT( "Show Details..." ) );

    GetSizer()->Layout();
    Fit();
}
