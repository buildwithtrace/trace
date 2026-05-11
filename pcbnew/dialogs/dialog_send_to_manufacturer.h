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

#ifndef DIALOG_SEND_TO_MANUFACTURER_H
#define DIALOG_SEND_TO_MANUFACTURER_H

#include <wx/dialog.h>
#include <wx/choice.h>
#include <wx/textctrl.h>
#include <wx/stattext.h>
#include <wx/simplebook.h>
#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/panel.h>
#include <wx/checkbox.h>
#include <wx/scrolwin.h>
#include <wx/statbox.h>
#include <wx/spinctrl.h>
#include <wx/gauge.h>
#include <wx/filedlg.h>
#include <atomic>
#include <mutex>
#include <thread>

class PCB_EDIT_FRAME;

/**
 * Dialog for sending production files to a manufacturer.
 *
 * Allows the user to select a target manufacturer, configure order parameters,
 * generate Gerber/drill files, and submit them. Supports PCBWay (Partner API)
 * and Pikkolo Assembly (placeholder).
 */
class DIALOG_SEND_TO_MANUFACTURER : public wxDialog
{
public:
    DIALOG_SEND_TO_MANUFACTURER( PCB_EDIT_FRAME* aParent );
    ~DIALOG_SEND_TO_MANUFACTURER();

    wxString GetSelectedManufacturer() const;

private:
    void onManufacturerChanged( wxCommandEvent& aEvent );
    void onGenerateFiles( wxCommandEvent& aEvent );
    void onSend( wxCommandEvent& aEvent );
    void onAutoFill( wxCommandEvent& aEvent );

    void buildPcbwayPanel();
    void buildPikkoloPanel();
    void showPanelForManufacturer( const wxString& aMfrId );
    void applyBoardDefaults();
    void loadShippingSettings();
    void saveShippingSettings();

    PCB_EDIT_FRAME* m_frame;

    wxChoice*       m_mfrChoice;
    wxSimplebook*   m_notebook;

    wxStaticText*   m_statusLabel;

    // --- PCBWay panel controls ---
    wxScrolledWindow* m_pcbwayPanel;
    wxButton*       m_pcbwayAutoFillBtn;

    // PCB Specification
    wxChoice*       m_pcbwayBoardType;
    wxChoice*       m_pcbwayLayers;
    wxTextCtrl*     m_pcbwayLength;
    wxTextCtrl*     m_pcbwayWidth;
    wxTextCtrl*     m_pcbwayQty;
    wxChoice*       m_pcbwayMaterial;
    wxChoice*       m_pcbwayFR4Tg;
    wxChoice*       m_pcbwayThickness;
    wxChoice*       m_pcbwayMinTrackSpacing;
    wxChoice*       m_pcbwayMinHoleSize;
    wxChoice*       m_pcbwaySolderMask;
    wxChoice*       m_pcbwaySilkscreen;
    wxChoice*       m_pcbwayUVPrinting;
    wxButton*       m_pcbwayUVFileBtn;
    wxStaticText*   m_pcbwayUVFileLabel;
    wxBoxSizer*     m_pcbwayUVFileSizer;
    wxCheckBox*     m_pcbwayEdgeConnector;
    wxChoice*       m_pcbwaySurface;
    wxChoice*       m_pcbwayViaProcess;
    wxChoice*       m_pcbwayCopperWeight;
    wxChoice*       m_pcbwayRemoveProductNo;

    // SMD Stencil
    wxCheckBox*     m_pcbwayStencilEnabled;
    wxChoice*       m_pcbwayStencilType;
    wxCheckBox*     m_pcbwayStencilMultiLevel;
    wxChoice*       m_pcbwayStencilSide;
    wxSpinCtrl*     m_pcbwayStencilQty;
    wxChoice*       m_pcbwayStencilThickness;
    wxChoice*       m_pcbwayStencilFiducials;
    wxCheckBox*     m_pcbwayStencilElectropol;

    // Assembly
    wxCheckBox*     m_pcbwayAssemblyEnabled;
    wxChoice*       m_pcbwayAssemblyType;
    wxChoice*       m_pcbwayAssemblySide;
    wxSpinCtrl*     m_pcbwayAssemblyQty;
    wxCheckBox*     m_pcbwayContainsSensitive;
    wxCheckBox*     m_pcbwayAcceptAlternatives;
    wxTextCtrl*     m_pcbwayAssemblyNotes;

    // PCBWay Shipping Address
    wxTextCtrl*     m_pcbwayShipName;
    wxTextCtrl*     m_pcbwayShipAddress;
    wxTextCtrl*     m_pcbwayShipCity;
    wxTextCtrl*     m_pcbwayShipState;
    wxTextCtrl*     m_pcbwayShipZip;
    wxTextCtrl*     m_pcbwayShipCountry;

    // PCBWay Order Options (from quote response)
    wxChoice*       m_pcbwayBuildDays;
    wxChoice*       m_pcbwayShipType;
    wxTextCtrl*     m_pcbwayShipEmail;
    wxTextCtrl*     m_pcbwayShipPhone;

    // --- Pikkolo panel controls ---
    wxScrolledWindow* m_pikkoloPanel;
    wxButton*       m_pikkoloAutoFillBtn;
    wxChoice*       m_pikkoloPcbSource;
    wxTextCtrl*     m_pikkoloQty;
    wxStaticText*   m_pikkoloLayers;
    wxStaticText*   m_pikkoloBoardArea;
    wxCheckBox*     m_pikkoloIncludeFab;
    wxCheckBox*     m_pikkoloShipStencil;
    wxCheckBox*     m_pikkoloDblSidedAsm;
    wxTextCtrl*     m_pikkoloNotes;
    wxTextCtrl*     m_pikkoloEmail;
    wxTextCtrl*     m_pikkoloName;
    wxTextCtrl*     m_pikkoloCompany;
    wxTextCtrl*     m_pikkoloAddress;
    wxTextCtrl*     m_pikkoloCity;
    wxTextCtrl*     m_pikkoloState;
    wxTextCtrl*     m_pikkoloZip;
    wxChoice*       m_pikkoloShipping;

    // Additional files (both manufacturers)
    wxButton*       m_pcbwayAddFilesBtn;
    wxStaticText*   m_pcbwayAddFilesLabel;
    wxButton*       m_pikkoloAddFilesBtn;
    wxStaticText*   m_pikkoloAddFilesLabel;

    wxButton*       m_generateBtn;
    wxButton*       m_sendBtn;

    // AI autofill log output
    wxTextCtrl*     m_aiLogCtrl;
    wxStaticText*   m_aiLogLabel;

    // Progress indicator for long operations
    wxGauge*        m_progressGauge;

    // Thread safety
    std::atomic<bool> m_closing{false};
    std::mutex        m_threadMutex;
    std::thread       m_autofillThread;
    std::thread       m_sendThread;

    wxString        m_generatedZipPath;
    wxString        m_bomCsvPath;
    wxString        m_bomJsonPath;
    wxString        m_positionFilePath;
    wxString        m_uvPrintFilePath;
    std::vector<wxString> m_additionalFiles;

    void appendLog( const wxString& aMsg );
    void clearLog();
    void onUVPrintingChanged( wxCommandEvent& aEvent );
    void onPickUVFile( wxCommandEvent& aEvent );
    void onPickAdditionalFiles( wxCommandEvent& aEvent, wxStaticText* aLabel );
};

#endif // DIALOG_SEND_TO_MANUFACTURER_H
