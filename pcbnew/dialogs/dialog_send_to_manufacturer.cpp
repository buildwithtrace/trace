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

#include "dialog_send_to_manufacturer.h"

#include <pcb_edit_frame.h>
#include <pcbnew_settings.h>
#include <project/project_file.h>
#include <board.h>
#include <board_design_settings.h>
#include <footprint.h>
#include <pad.h>
#include <pcb_track.h>
#include <pgm_base.h>
#include <auth/auth_manager.h>
#include <kicad_curl/kicad_curl_easy.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <map>
#include <string>
#include <wx/msgdlg.h>
#include <wx/statbox.h>
#include <wx/spinctrl.h>
#include <jobs/job_export_pcb_gerbers.h>
#include <jobs/job_export_pcb_drill.h>
#include <kiway.h>
#include <reporter.h>
#include <paths.h>
#include <wx/zipstrm.h>
#include <wx/wfstream.h>
#include <wx/dir.h>
#include <board_stackup_manager/board_stackup.h>
#include <project/net_settings.h>
#include <zone.h>
#include <pcb_field.h>
#include <string_utils.h>
#include <exporters/place_file_exporter.h>
#include <memory>
#include <thread>

static int findClosestChoice( wxChoice* aChoice, double aValue )
{
    int best = 0;
    double bestDiff = 1e18;

    for( unsigned i = 0; i < aChoice->GetCount(); ++i )
    {
        double val = 0;
        if( !aChoice->GetString( i ).ToDouble( &val ) )
            continue;  // skip non-numeric entries like "No Drill"

        double diff = std::fabs( val - aValue );

        if( diff < bestDiff )
        {
            bestDiff = diff;
            best = static_cast<int>( i );
        }
    }

    return best;
}


DIALOG_SEND_TO_MANUFACTURER::DIALOG_SEND_TO_MANUFACTURER( PCB_EDIT_FRAME* aParent ) :
        wxDialog( aParent, wxID_ANY, _( "Send to Manufacturer" ), wxDefaultPosition,
                  wxSize( 580, 780 ), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER ),
        m_frame( aParent ),
        m_mfrChoice( nullptr ),
        m_notebook( nullptr ),
        m_statusLabel( nullptr ),
        m_pcbwayPanel( nullptr ),
        m_pcbwayAutoFillBtn( nullptr ),
        m_pcbwayBoardType( nullptr ),
        m_pcbwayLayers( nullptr ),
        m_pcbwayLength( nullptr ),
        m_pcbwayWidth( nullptr ),
        m_pcbwayQty( nullptr ),
        m_pcbwayMaterial( nullptr ),
        m_pcbwayFR4Tg( nullptr ),
        m_pcbwayThickness( nullptr ),
        m_pcbwayMinTrackSpacing( nullptr ),
        m_pcbwayMinHoleSize( nullptr ),
        m_pcbwaySolderMask( nullptr ),
        m_pcbwaySilkscreen( nullptr ),
        m_pcbwayUVPrinting( nullptr ),
        m_pcbwayUVFileBtn( nullptr ),
        m_pcbwayUVFileLabel( nullptr ),
        m_pcbwayUVFileSizer( nullptr ),
        m_pcbwayEdgeConnector( nullptr ),
        m_pcbwaySurface( nullptr ),
        m_pcbwayViaProcess( nullptr ),
        m_pcbwayCopperWeight( nullptr ),
        m_pcbwayRemoveProductNo( nullptr ),
        m_pcbwayStencilEnabled( nullptr ),
        m_pcbwayStencilType( nullptr ),
        m_pcbwayStencilMultiLevel( nullptr ),
        m_pcbwayStencilSide( nullptr ),
        m_pcbwayStencilQty( nullptr ),
        m_pcbwayStencilThickness( nullptr ),
        m_pcbwayStencilFiducials( nullptr ),
        m_pcbwayStencilElectropol( nullptr ),
        m_pcbwayAssemblyEnabled( nullptr ),
        m_pcbwayAssemblyType( nullptr ),
        m_pcbwayAssemblySide( nullptr ),
        m_pcbwayAssemblyQty( nullptr ),
        m_pcbwayContainsSensitive( nullptr ),
        m_pcbwayAcceptAlternatives( nullptr ),
        m_pcbwayAssemblyNotes( nullptr ),
        m_pcbwayShipName( nullptr ),
        m_pcbwayShipAddress( nullptr ),
        m_pcbwayShipCity( nullptr ),
        m_pcbwayShipState( nullptr ),
        m_pcbwayShipZip( nullptr ),
        m_pcbwayShipCountry( nullptr ),
        m_pcbwayBuildDays( nullptr ),
        m_pcbwayShipType( nullptr ),
        m_pcbwayShipEmail( nullptr ),
        m_pcbwayShipPhone( nullptr ),
        m_pikkoloPanel( nullptr ),
        m_pikkoloAutoFillBtn( nullptr ),
        m_pikkoloPcbSource( nullptr ),
        m_pikkoloQty( nullptr ),
        m_pikkoloLayers( nullptr ),
        m_pikkoloBoardArea( nullptr ),
        m_pikkoloIncludeFab( nullptr ),
        m_pikkoloShipStencil( nullptr ),
        m_pikkoloDblSidedAsm( nullptr ),
        m_pikkoloNotes( nullptr ),
        m_pikkoloEmail( nullptr ),
        m_pikkoloName( nullptr ),
        m_pikkoloCompany( nullptr ),
        m_pikkoloAddress( nullptr ),
        m_pikkoloCity( nullptr ),
        m_pikkoloState( nullptr ),
        m_pikkoloZip( nullptr ),
        m_pikkoloShipping( nullptr ),
        m_pcbwayAddFilesBtn( nullptr ),
        m_pcbwayAddFilesLabel( nullptr ),
        m_pikkoloAddFilesBtn( nullptr ),
        m_pikkoloAddFilesLabel( nullptr ),
        m_generateBtn( nullptr ),
        m_sendBtn( nullptr ),
        m_aiLogCtrl( nullptr ),
        m_aiLogLabel( nullptr ),
        m_progressGauge( nullptr )
{
    wxBoxSizer* mainSizer = new wxBoxSizer( wxVERTICAL );

    // Manufacturer selection
    wxBoxSizer* mfrSizer = new wxBoxSizer( wxHORIZONTAL );
    mfrSizer->Add( new wxStaticText( this, wxID_ANY, _( "Manufacturer:" ) ),
                   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8 );

    wxArrayString mfrChoices;
    mfrChoices.Add( wxT( "PCBWay" ) );
    mfrChoices.Add( wxT( "Pikkolo Assembly" ) );

    m_mfrChoice = new wxChoice( this, wxID_ANY, wxDefaultPosition, wxDefaultSize, mfrChoices );
    mfrSizer->Add( m_mfrChoice, 1, wxEXPAND );
    mainSizer->Add( mfrSizer, 0, wxEXPAND | wxALL, 10 );

    wxString preferred = m_frame->Prj().GetProjectFile().m_PreferredManufacturer;
    if( preferred == wxT( "pikkolo" ) )
        m_mfrChoice->SetSelection( 1 );
    else
        m_mfrChoice->SetSelection( 0 );

    // Notebook for manufacturer-specific panels
    m_notebook = new wxSimplebook( this, wxID_ANY );
    buildPcbwayPanel();
    buildPikkoloPanel();
    mainSizer->Add( m_notebook, 1, wxEXPAND | wxLEFT | wxRIGHT, 10 );

    // Status
    m_statusLabel = new wxStaticText( this, wxID_ANY, wxEmptyString );
    mainSizer->Add( m_statusLabel, 0, wxEXPAND | wxALL, 10 );

    // Indeterminate progress bar for long operations
    m_progressGauge = new wxGauge( this, wxID_ANY, 100, wxDefaultPosition,
                                   wxSize( -1, 6 ), wxGA_HORIZONTAL | wxGA_SMOOTH );
    m_progressGauge->Hide();
    mainSizer->Add( m_progressGauge, 0, wxEXPAND | wxLEFT | wxRIGHT, 10 );

    // AI autofill log
    m_aiLogLabel = new wxStaticText( this, wxID_ANY, _( "AI Output:" ) );
    m_aiLogLabel->Hide();
    mainSizer->Add( m_aiLogLabel, 0, wxLEFT | wxRIGHT | wxTOP, 10 );

    m_aiLogCtrl = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                  wxSize( -1, 120 ),
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2 | wxHSCROLL );
    m_aiLogCtrl->SetBackgroundColour( wxColour( 30, 30, 30 ) );
    m_aiLogCtrl->SetForegroundColour( wxColour( 200, 200, 200 ) );
    wxFont monoFont( 9, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL );
    m_aiLogCtrl->SetFont( monoFont );
    m_aiLogCtrl->Hide();
    mainSizer->Add( m_aiLogCtrl, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10 );

    // Buttons
    wxBoxSizer* btnSizer = new wxBoxSizer( wxHORIZONTAL );

    m_generateBtn = new wxButton( this, wxID_ANY, _( "Generate Production Files" ) );
    btnSizer->Add( m_generateBtn, 0, wxRIGHT, 8 );

    m_sendBtn = new wxButton( this, wxID_ANY, _( "Send" ) );
    m_sendBtn->Enable( false );
    btnSizer->Add( m_sendBtn, 0, wxRIGHT, 8 );

    btnSizer->Add( new wxButton( this, wxID_CANCEL, _( "Close" ) ), 0 );

    mainSizer->Add( btnSizer, 0, wxALIGN_RIGHT | wxALL, 10 );

    SetSizer( mainSizer );
    Layout();

    showPanelForManufacturer( preferred.IsEmpty() ? wxString( wxT( "pcbway" ) ) : preferred );

    // Apply board defaults to populate dimensions, layers, etc.
    applyBoardDefaults();

    // Event bindings
    m_mfrChoice->Bind( wxEVT_CHOICE, &DIALOG_SEND_TO_MANUFACTURER::onManufacturerChanged, this );
    m_generateBtn->Bind( wxEVT_BUTTON, &DIALOG_SEND_TO_MANUFACTURER::onGenerateFiles, this );
    m_sendBtn->Bind( wxEVT_BUTTON, &DIALOG_SEND_TO_MANUFACTURER::onSend, this );

    // Load saved shipping info from settings
    loadShippingSettings();
}


wxString DIALOG_SEND_TO_MANUFACTURER::GetSelectedManufacturer() const
{
    int sel = m_mfrChoice->GetSelection();
    if( sel == 0 )
        return wxT( "pcbway" );
    else if( sel == 1 )
        return wxT( "pikkolo" );
    return wxEmptyString;
}


void DIALOG_SEND_TO_MANUFACTURER::loadShippingSettings()
{
    PCBNEW_SETTINGS* cfg = m_frame->GetPcbNewSettings();
    if( !cfg )
        return;

    const auto& ship = cfg->m_ManufacturerShipping;

    // Load into PCBWay fields
    if( m_pcbwayShipName )
        m_pcbwayShipName->SetValue( ship.name );
    if( m_pcbwayShipEmail )
        m_pcbwayShipEmail->SetValue( ship.email );
    if( m_pcbwayShipPhone )
        m_pcbwayShipPhone->SetValue( ship.phone );
    if( m_pcbwayShipAddress )
        m_pcbwayShipAddress->SetValue( ship.street );
    if( m_pcbwayShipCity )
        m_pcbwayShipCity->SetValue( ship.city );
    if( m_pcbwayShipState )
        m_pcbwayShipState->SetValue( ship.state );
    if( m_pcbwayShipZip )
        m_pcbwayShipZip->SetValue( ship.zip );
    if( m_pcbwayShipCountry )
        m_pcbwayShipCountry->SetValue( ship.country );

    // Load into Pikkolo fields
    if( m_pikkoloName )
        m_pikkoloName->SetValue( ship.name );
    if( m_pikkoloEmail )
        m_pikkoloEmail->SetValue( ship.email );
    if( m_pikkoloCompany )
        m_pikkoloCompany->SetValue( ship.company );
    if( m_pikkoloAddress )
        m_pikkoloAddress->SetValue( ship.street );
    if( m_pikkoloCity )
        m_pikkoloCity->SetValue( ship.city );
    if( m_pikkoloState )
        m_pikkoloState->SetValue( ship.state );
    if( m_pikkoloZip )
        m_pikkoloZip->SetValue( ship.zip );
}


void DIALOG_SEND_TO_MANUFACTURER::saveShippingSettings()
{
    PCBNEW_SETTINGS* cfg = m_frame->GetPcbNewSettings();
    if( !cfg )
        return;

    auto& ship = cfg->m_ManufacturerShipping;

    // Save from whichever panel has data (prefer non-empty values)
    wxString name, email, phone, company, street, city, state, zip, country;

    // Try PCBWay fields first
    if( m_pcbwayShipName && !m_pcbwayShipName->GetValue().IsEmpty() )
        name = m_pcbwayShipName->GetValue();
    if( m_pcbwayShipEmail && !m_pcbwayShipEmail->GetValue().IsEmpty() )
        email = m_pcbwayShipEmail->GetValue();
    if( m_pcbwayShipPhone && !m_pcbwayShipPhone->GetValue().IsEmpty() )
        phone = m_pcbwayShipPhone->GetValue();
    if( m_pcbwayShipAddress && !m_pcbwayShipAddress->GetValue().IsEmpty() )
        street = m_pcbwayShipAddress->GetValue();
    if( m_pcbwayShipCity && !m_pcbwayShipCity->GetValue().IsEmpty() )
        city = m_pcbwayShipCity->GetValue();
    if( m_pcbwayShipState && !m_pcbwayShipState->GetValue().IsEmpty() )
        state = m_pcbwayShipState->GetValue();
    if( m_pcbwayShipZip && !m_pcbwayShipZip->GetValue().IsEmpty() )
        zip = m_pcbwayShipZip->GetValue();
    if( m_pcbwayShipCountry && !m_pcbwayShipCountry->GetValue().IsEmpty() )
        country = m_pcbwayShipCountry->GetValue();

    // Override with Pikkolo fields if they have data
    if( m_pikkoloName && !m_pikkoloName->GetValue().IsEmpty() )
        name = m_pikkoloName->GetValue();
    if( m_pikkoloEmail && !m_pikkoloEmail->GetValue().IsEmpty() )
        email = m_pikkoloEmail->GetValue();
    if( m_pikkoloCompany && !m_pikkoloCompany->GetValue().IsEmpty() )
        company = m_pikkoloCompany->GetValue();
    if( m_pikkoloAddress && !m_pikkoloAddress->GetValue().IsEmpty() )
        street = m_pikkoloAddress->GetValue();
    if( m_pikkoloCity && !m_pikkoloCity->GetValue().IsEmpty() )
        city = m_pikkoloCity->GetValue();
    if( m_pikkoloState && !m_pikkoloState->GetValue().IsEmpty() )
        state = m_pikkoloState->GetValue();
    if( m_pikkoloZip && !m_pikkoloZip->GetValue().IsEmpty() )
        zip = m_pikkoloZip->GetValue();

    // Update settings
    if( !name.IsEmpty() )
        ship.name = name;
    if( !email.IsEmpty() )
        ship.email = email;
    if( !phone.IsEmpty() )
        ship.phone = phone;
    if( !company.IsEmpty() )
        ship.company = company;
    if( !street.IsEmpty() )
        ship.street = street;
    if( !city.IsEmpty() )
        ship.city = city;
    if( !state.IsEmpty() )
        ship.state = state;
    if( !zip.IsEmpty() )
        ship.zip = zip;
    if( !country.IsEmpty() )
        ship.country = country;
}


DIALOG_SEND_TO_MANUFACTURER::~DIALOG_SEND_TO_MANUFACTURER()
{
    m_closing.store( true );

    std::lock_guard<std::mutex> lock( m_threadMutex );
    if( m_autofillThread.joinable() )
        m_autofillThread.join();
    if( m_sendThread.joinable() )
        m_sendThread.join();
}


void DIALOG_SEND_TO_MANUFACTURER::buildPcbwayPanel()
{
    m_pcbwayPanel = new wxScrolledWindow( m_notebook, wxID_ANY );
    m_pcbwayPanel->SetScrollRate( 0, 10 );
    wxBoxSizer* panelSizer = new wxBoxSizer( wxVERTICAL );

    m_pcbwayAutoFillBtn = new wxButton( m_pcbwayPanel, wxID_ANY,
                                        _( "Auto-Fill from Board" ) );
    m_pcbwayAutoFillBtn->SetToolTip(
            _( "Analyze the current PCB and auto-fill order parameters using AI" ) );
    panelSizer->Add( m_pcbwayAutoFillBtn, 0, wxEXPAND | wxALL, 10 );
    m_pcbwayAutoFillBtn->Bind( wxEVT_BUTTON,
                               &DIALOG_SEND_TO_MANUFACTURER::onAutoFill, this );

    // ---- PCB Specification ----
    wxStaticBoxSizer* specBox = new wxStaticBoxSizer( wxVERTICAL, m_pcbwayPanel,
                                                      _( "PCB Specification" ) );
    wxFlexGridSizer* grid = new wxFlexGridSizer( 2, 6, 8 );
    grid->AddGrowableCol( 1, 1 );

    // Board Type
    grid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Board Type:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    wxArrayString boardTypes;
    boardTypes.Add( wxT( "Single pieces" ) );
    boardTypes.Add( wxT( "Panel by Customer" ) );
    boardTypes.Add( wxT( "Panel by Supplier" ) );
    m_pcbwayBoardType = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                      wxDefaultSize, boardTypes );
    m_pcbwayBoardType->SetSelection( 0 );
    grid->Add( m_pcbwayBoardType, 0, wxEXPAND );

    // Layers
    grid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Layers:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    wxArrayString layers;
    layers.Add( wxT( "1" ) );   layers.Add( wxT( "2" ) );
    layers.Add( wxT( "4" ) );   layers.Add( wxT( "6" ) );
    layers.Add( wxT( "8" ) );   layers.Add( wxT( "10" ) );
    layers.Add( wxT( "12" ) );  layers.Add( wxT( "14" ) );
    m_pcbwayLayers = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                   wxDefaultSize, layers );
    m_pcbwayLayers->SetSelection( 1 );
    grid->Add( m_pcbwayLayers, 0, wxEXPAND );

    // Length (mm)
    grid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Length (mm):" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    m_pcbwayLength = new wxTextCtrl( m_pcbwayPanel, wxID_ANY, wxT( "100" ) );
    grid->Add( m_pcbwayLength, 0, wxEXPAND );

    // Width (mm)
    grid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Width (mm):" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    m_pcbwayWidth = new wxTextCtrl( m_pcbwayPanel, wxID_ANY, wxT( "100" ) );
    grid->Add( m_pcbwayWidth, 0, wxEXPAND );

    // Quantity
    grid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Quantity:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    m_pcbwayQty = new wxTextCtrl( m_pcbwayPanel, wxID_ANY, wxT( "5" ) );
    grid->Add( m_pcbwayQty, 0, wxEXPAND );

    // Material
    grid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Material:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    wxArrayString materials;
    materials.Add( wxT( "FR-4" ) );
    materials.Add( wxT( "Aluminum" ) );
    materials.Add( wxT( "Rogers" ) );
    materials.Add( wxT( "HDI (Buried/blind vias)" ) );
    materials.Add( wxT( "Copper Base" ) );
    m_pcbwayMaterial = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                     wxDefaultSize, materials );
    m_pcbwayMaterial->SetSelection( 0 );
    grid->Add( m_pcbwayMaterial, 0, wxEXPAND );

    // FR4-TG
    grid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "FR4-TG:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    wxArrayString tgOptions;
    tgOptions.Add( wxT( "TG 130-140" ) );
    tgOptions.Add( wxT( "TG 150-160" ) );
    tgOptions.Add( wxT( "TG 170-180" ) );
    tgOptions.Add( wxT( "S1000H TG150" ) );
    tgOptions.Add( wxT( "S1000-2M TG170" ) );
    m_pcbwayFR4Tg = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                   wxDefaultSize, tgOptions );
    m_pcbwayFR4Tg->SetSelection( 0 );
    grid->Add( m_pcbwayFR4Tg, 0, wxEXPAND );

    // Thickness
    grid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Thickness (mm):" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    wxArrayString thicknesses;
    thicknesses.Add( wxT( "0.2" ) );   thicknesses.Add( wxT( "0.3" ) );
    thicknesses.Add( wxT( "0.4" ) );   thicknesses.Add( wxT( "0.6" ) );
    thicknesses.Add( wxT( "0.8" ) );   thicknesses.Add( wxT( "1.0" ) );
    thicknesses.Add( wxT( "1.2" ) );   thicknesses.Add( wxT( "1.6" ) );
    thicknesses.Add( wxT( "2.0" ) );   thicknesses.Add( wxT( "2.4" ) );
    thicknesses.Add( wxT( "2.6" ) );   thicknesses.Add( wxT( "2.8" ) );
    thicknesses.Add( wxT( "3.0" ) );   thicknesses.Add( wxT( "3.2" ) );
    m_pcbwayThickness = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                      wxDefaultSize, thicknesses );
    m_pcbwayThickness->SetSelection( 7 );  // 1.6mm default
    grid->Add( m_pcbwayThickness, 0, wxEXPAND );

    // Copper weight
    grid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Copper Weight (oz):" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    wxArrayString weights;
    weights.Add( wxT( "Bare board (0 oz)" ) );
    weights.Add( wxT( "1 oz Cu" ) );   weights.Add( wxT( "2 oz Cu" ) );
    weights.Add( wxT( "3 oz Cu" ) );   weights.Add( wxT( "4 oz Cu" ) );
    weights.Add( wxT( "5 oz Cu" ) );   weights.Add( wxT( "6 oz Cu" ) );
    weights.Add( wxT( "7 oz Cu" ) );   weights.Add( wxT( "8 oz Cu" ) );
    weights.Add( wxT( "9 oz Cu" ) );   weights.Add( wxT( "10 oz Cu" ) );
    weights.Add( wxT( "11 oz Cu" ) );  weights.Add( wxT( "12 oz Cu" ) );
    weights.Add( wxT( "13 oz Cu" ) );
    m_pcbwayCopperWeight = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                         wxDefaultSize, weights );
    m_pcbwayCopperWeight->SetSelection( 1 );  // 1oz default
    grid->Add( m_pcbwayCopperWeight, 0, wxEXPAND );

    // Surface finish
    grid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Surface Finish:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    wxArrayString surfaces;
    surfaces.Add( wxT( "HASL with lead" ) );
    surfaces.Add( wxT( "HASL lead free" ) );
    surfaces.Add( wxT( "Immersion gold (ENIG)" ) );
    surfaces.Add( wxT( "OSP" ) );
    surfaces.Add( wxT( "Hard gold" ) );
    surfaces.Add( wxT( "Immersion silver (Ag)" ) );
    surfaces.Add( wxT( "Immersion tin" ) );
    surfaces.Add( wxT( "HASL LF + Selective Immersion gold" ) );
    surfaces.Add( wxT( "HASL LF + Selective Hard gold" ) );
    surfaces.Add( wxT( "Immersion gold + Selective Hard gold" ) );
    surfaces.Add( wxT( "ENEPIG" ) );
    surfaces.Add( wxT( "None/Plain copper" ) );
    m_pcbwaySurface = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                    wxDefaultSize, surfaces );
    m_pcbwaySurface->SetSelection( 1 );
    grid->Add( m_pcbwaySurface, 0, wxEXPAND );

    // Solder mask
    grid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Solder Mask:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    wxArrayString masks;
    masks.Add( wxT( "Green" ) );       masks.Add( wxT( "Red" ) );
    masks.Add( wxT( "Yellow" ) );      masks.Add( wxT( "Blue" ) );
    masks.Add( wxT( "White" ) );       masks.Add( wxT( "Black" ) );
    masks.Add( wxT( "Purple" ) );      masks.Add( wxT( "None" ) );
    m_pcbwaySolderMask = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                       wxDefaultSize, masks );
    m_pcbwaySolderMask->SetSelection( 0 );
    grid->Add( m_pcbwaySolderMask, 0, wxEXPAND );

    // Silkscreen color
    grid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Silkscreen:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    wxArrayString silks;
    silks.Add( wxT( "White" ) );   silks.Add( wxT( "Black" ) );
    silks.Add( wxT( "Yellow" ) );  silks.Add( wxT( "None" ) );
    m_pcbwaySilkscreen = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                       wxDefaultSize, silks );
    m_pcbwaySilkscreen->SetSelection( 0 );
    grid->Add( m_pcbwaySilkscreen, 0, wxEXPAND );

    // Min track/spacing
    grid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Min Track/Spacing:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    wxArrayString trackSpacing;
    trackSpacing.Add( wxT( "3/3mil" ) );
    trackSpacing.Add( wxT( "4/4mil" ) );
    trackSpacing.Add( wxT( "5/5mil" ) );
    trackSpacing.Add( wxT( "6/6mil" ) );
    trackSpacing.Add( wxT( "8/8mil" ) );
    m_pcbwayMinTrackSpacing = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                            wxDefaultSize, trackSpacing );
    m_pcbwayMinTrackSpacing->SetSelection( 3 );  // 6/6mil default
    grid->Add( m_pcbwayMinTrackSpacing, 0, wxEXPAND );

    // Min hole size
    grid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Min Hole Size (mm):" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    wxArrayString holeSizes;
    holeSizes.Add( wxT( "0.15" ) );  holeSizes.Add( wxT( "0.2" ) );
    holeSizes.Add( wxT( "0.25" ) );  holeSizes.Add( wxT( "0.3" ) );
    holeSizes.Add( wxT( "0.8" ) );   holeSizes.Add( wxT( "1.0" ) );
    holeSizes.Add( wxT( "No Drill" ) );
    m_pcbwayMinHoleSize = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                        wxDefaultSize, holeSizes );
    m_pcbwayMinHoleSize->SetSelection( 3 );  // 0.3mm default
    grid->Add( m_pcbwayMinHoleSize, 0, wxEXPAND );

    // Via process
    grid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Via Process:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    wxArrayString viaProc;
    viaProc.Add( wxT( "Tenting vias" ) );
    viaProc.Add( wxT( "Plugged vias" ) );
    viaProc.Add( wxT( "Vias not covered" ) );
    m_pcbwayViaProcess = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                       wxDefaultSize, viaProc );
    m_pcbwayViaProcess->SetSelection( 0 );
    grid->Add( m_pcbwayViaProcess, 0, wxEXPAND );

    // UV Printing
    grid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "UV Printing:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    wxArrayString uvOptions;
    uvOptions.Add( wxT( "None" ) );
    uvOptions.Add( wxT( "Single-sided: Top" ) );
    uvOptions.Add( wxT( "Single-sided: Bottom" ) );
    uvOptions.Add( wxT( "Double-sided" ) );
    m_pcbwayUVPrinting = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                       wxDefaultSize, uvOptions );
    m_pcbwayUVPrinting->SetSelection( 0 );
    grid->Add( m_pcbwayUVPrinting, 0, wxEXPAND );

    // Edge Connector
    grid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Edge Connector:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    m_pcbwayEdgeConnector = new wxCheckBox( m_pcbwayPanel, wxID_ANY, _( "Yes" ) );
    grid->Add( m_pcbwayEdgeConnector, 0, wxALIGN_CENTER_VERTICAL );

    // Remove Product No
    grid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Remove Product No.:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    wxArrayString removeOpts;
    removeOpts.Add( wxT( "No" ) );
    removeOpts.Add( wxT( "Yes (extra +$1.5)" ) );
    removeOpts.Add( wxT( "Specify a location" ) );
    m_pcbwayRemoveProductNo = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                            wxDefaultSize, removeOpts );
    m_pcbwayRemoveProductNo->SetSelection( 0 );
    grid->Add( m_pcbwayRemoveProductNo, 0, wxEXPAND );

    specBox->Add( grid, 0, wxEXPAND | wxALL, 8 );

    // UV Printing file upload (conditional — shown only when UV printing is not "None")
    m_pcbwayUVFileSizer = new wxBoxSizer( wxVERTICAL );

    wxStaticText* uvNote = new wxStaticText( m_pcbwayPanel, wxID_ANY,
        _( "Color printing requires a high-resolution image (PNG/PDF/AI) at 1:1 scale." ) );
    uvNote->SetForegroundColour( wxColour( 180, 90, 0 ) );
    wxFont smallFont = uvNote->GetFont();
    smallFont.SetPointSize( smallFont.GetPointSize() - 1 );
    uvNote->SetFont( smallFont );
    uvNote->Wrap( 500 );
    m_pcbwayUVFileSizer->Add( uvNote, 0, wxLEFT | wxTOP, 4 );

    wxBoxSizer* uvBtnRow = new wxBoxSizer( wxHORIZONTAL );
    m_pcbwayUVFileBtn = new wxButton( m_pcbwayPanel, wxID_ANY, _( "Select Print Image..." ) );
    uvBtnRow->Add( m_pcbwayUVFileBtn, 0, wxRIGHT, 8 );
    m_pcbwayUVFileLabel = new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "No file selected" ) );
    m_pcbwayUVFileLabel->SetForegroundColour( wxColour( 140, 140, 140 ) );
    uvBtnRow->Add( m_pcbwayUVFileLabel, 1, wxALIGN_CENTER_VERTICAL );
    m_pcbwayUVFileSizer->Add( uvBtnRow, 0, wxEXPAND | wxLEFT | wxTOP | wxBOTTOM, 4 );

    m_pcbwayUVFileSizer->ShowItems( false );
    specBox->Add( m_pcbwayUVFileSizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 8 );

    m_pcbwayUVPrinting->Bind( wxEVT_CHOICE,
                               &DIALOG_SEND_TO_MANUFACTURER::onUVPrintingChanged, this );
    m_pcbwayUVFileBtn->Bind( wxEVT_BUTTON,
                              &DIALOG_SEND_TO_MANUFACTURER::onPickUVFile, this );

    panelSizer->Add( specBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10 );

    // ---- SMD Stencil ----
    wxStaticBoxSizer* stencilBox = new wxStaticBoxSizer( wxVERTICAL, m_pcbwayPanel,
                                                         _( "SMD Stencil (optional)" ) );
    m_pcbwayStencilEnabled = new wxCheckBox( m_pcbwayPanel, wxID_ANY,
                                             _( "Order stencil together with PCB" ) );
    stencilBox->Add( m_pcbwayStencilEnabled, 0, wxALL, 6 );

    wxFlexGridSizer* stGrid = new wxFlexGridSizer( 2, 6, 8 );
    stGrid->AddGrowableCol( 1, 1 );

    stGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Stencil Type:" ) ),
                 0, wxALIGN_CENTER_VERTICAL );
    wxArrayString stTypes;
    stTypes.Add( wxT( "Framework" ) );  stTypes.Add( wxT( "Non-framework" ) );
    m_pcbwayStencilType = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                        wxDefaultSize, stTypes );
    m_pcbwayStencilType->SetSelection( 0 );
    stGrid->Add( m_pcbwayStencilType, 0, wxEXPAND );

    stGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Multi-level/Step:" ) ),
                 0, wxALIGN_CENTER_VERTICAL );
    m_pcbwayStencilMultiLevel = new wxCheckBox( m_pcbwayPanel, wxID_ANY, _( "Yes" ) );
    stGrid->Add( m_pcbwayStencilMultiLevel, 0 );

    stGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Stencil Side:" ) ),
                 0, wxALIGN_CENTER_VERTICAL );
    wxArrayString stSides;
    stSides.Add( wxT( "Top" ) );
    stSides.Add( wxT( "Bottom" ) );
    stSides.Add( wxT( "Top+Bottom (Single Stencil)" ) );
    stSides.Add( wxT( "Top & Bottom (Separate)" ) );
    m_pcbwayStencilSide = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                        wxDefaultSize, stSides );
    m_pcbwayStencilSide->SetSelection( 0 );
    stGrid->Add( m_pcbwayStencilSide, 0, wxEXPAND );

    stGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Quantity:" ) ),
                 0, wxALIGN_CENTER_VERTICAL );
    m_pcbwayStencilQty = new wxSpinCtrl( m_pcbwayPanel, wxID_ANY, wxT( "1" ),
                                         wxDefaultPosition, wxDefaultSize,
                                         wxSP_ARROW_KEYS, 1, 100, 1 );
    stGrid->Add( m_pcbwayStencilQty, 0, wxEXPAND );

    stGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Thickness (mm):" ) ),
                 0, wxALIGN_CENTER_VERTICAL );
    wxArrayString stThick;
    stThick.Add( wxT( "0.08" ) );  stThick.Add( wxT( "0.10" ) );
    stThick.Add( wxT( "0.12" ) );  stThick.Add( wxT( "0.15" ) );
    stThick.Add( wxT( "0.20" ) );  stThick.Add( wxT( "0.25" ) );
    stThick.Add( wxT( "0.30" ) );
    m_pcbwayStencilThickness = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                             wxDefaultSize, stThick );
    m_pcbwayStencilThickness->SetSelection( 2 );  // 0.12mm default
    stGrid->Add( m_pcbwayStencilThickness, 0, wxEXPAND );

    stGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Existing Fiducials:" ) ),
                 0, wxALIGN_CENTER_VERTICAL );
    wxArrayString fidOpts;
    fidOpts.Add( wxT( "None" ) );
    fidOpts.Add( wxT( "Half lasered" ) );
    fidOpts.Add( wxT( "Lasered through" ) );
    m_pcbwayStencilFiducials = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                             wxDefaultSize, fidOpts );
    m_pcbwayStencilFiducials->SetSelection( 0 );
    stGrid->Add( m_pcbwayStencilFiducials, 0, wxEXPAND );

    stGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Electropolishing:" ) ),
                 0, wxALIGN_CENTER_VERTICAL );
    m_pcbwayStencilElectropol = new wxCheckBox( m_pcbwayPanel, wxID_ANY, _( "Yes" ) );
    stGrid->Add( m_pcbwayStencilElectropol, 0 );

    stencilBox->Add( stGrid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8 );
    panelSizer->Add( stencilBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10 );

    // ---- Assembly Service ----
    wxStaticBoxSizer* asmBox = new wxStaticBoxSizer( wxVERTICAL, m_pcbwayPanel,
                                                     _( "Assembly Service (optional)" ) );
    m_pcbwayAssemblyEnabled = new wxCheckBox( m_pcbwayPanel, wxID_ANY,
                                              _( "Add assembly service" ) );
    asmBox->Add( m_pcbwayAssemblyEnabled, 0, wxALL, 6 );

    wxFlexGridSizer* asmGrid = new wxFlexGridSizer( 2, 6, 8 );
    asmGrid->AddGrowableCol( 1, 1 );

    asmGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Assembly Type:" ) ),
                  0, wxALIGN_CENTER_VERTICAL );
    wxArrayString asmTypes;
    asmTypes.Add( wxT( "Turnkey (PCBWay supply parts)" ) );
    asmTypes.Add( wxT( "Kitted or Consigned" ) );
    asmTypes.Add( wxT( "Combo" ) );
    m_pcbwayAssemblyType = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                         wxDefaultSize, asmTypes );
    m_pcbwayAssemblyType->SetSelection( 0 );
    asmGrid->Add( m_pcbwayAssemblyType, 0, wxEXPAND );

    asmGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Assembly Side(s):" ) ),
                  0, wxALIGN_CENTER_VERTICAL );
    wxArrayString asmSides;
    asmSides.Add( wxT( "Top side" ) );
    asmSides.Add( wxT( "Bottom side" ) );
    asmSides.Add( wxT( "Both sides" ) );
    m_pcbwayAssemblySide = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                         wxDefaultSize, asmSides );
    m_pcbwayAssemblySide->SetSelection( 0 );
    asmGrid->Add( m_pcbwayAssemblySide, 0, wxEXPAND );

    asmGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Quantity:" ) ),
                  0, wxALIGN_CENTER_VERTICAL );
    m_pcbwayAssemblyQty = new wxSpinCtrl( m_pcbwayPanel, wxID_ANY, wxT( "5" ),
                                          wxDefaultPosition, wxDefaultSize,
                                          wxSP_ARROW_KEYS, 1, 10000, 5 );
    asmGrid->Add( m_pcbwayAssemblyQty, 0, wxEXPAND );

    asmGrid->Add( 0, 0 );
    m_pcbwayContainsSensitive = new wxCheckBox( m_pcbwayPanel, wxID_ANY,
                                                _( "Contains sensitive components/parts" ) );
    asmGrid->Add( m_pcbwayContainsSensitive, 0 );

    asmGrid->Add( 0, 0 );
    m_pcbwayAcceptAlternatives = new wxCheckBox( m_pcbwayPanel, wxID_ANY,
            _( "Accept alternatives/substitutes made in China" ) );
    asmGrid->Add( m_pcbwayAcceptAlternatives, 0 );

    asmGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Assembly notes:" ) ),
                  0, wxALIGN_TOP );
    m_pcbwayAssemblyNotes = new wxTextCtrl( m_pcbwayPanel, wxID_ANY, wxEmptyString,
                                            wxDefaultPosition, wxSize( -1, 60 ),
                                            wxTE_MULTILINE );
    asmGrid->Add( m_pcbwayAssemblyNotes, 0, wxEXPAND );

    asmBox->Add( asmGrid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8 );
    panelSizer->Add( asmBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10 );

    // ---- Shipping Address ----
    wxStaticBoxSizer* shipBox = new wxStaticBoxSizer( wxVERTICAL, m_pcbwayPanel,
                                                      _( "Shipping Address" ) );
    wxFlexGridSizer* shipGrid = new wxFlexGridSizer( 2, 6, 8 );
    shipGrid->AddGrowableCol( 1, 1 );

    shipGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Name:" ) ),
                   0, wxALIGN_CENTER_VERTICAL );
    m_pcbwayShipName = new wxTextCtrl( m_pcbwayPanel, wxID_ANY );
    shipGrid->Add( m_pcbwayShipName, 0, wxEXPAND );

    shipGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Email:" ) ),
                   0, wxALIGN_CENTER_VERTICAL );
    m_pcbwayShipEmail = new wxTextCtrl( m_pcbwayPanel, wxID_ANY );
    shipGrid->Add( m_pcbwayShipEmail, 0, wxEXPAND );

    shipGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Phone:" ) ),
                   0, wxALIGN_CENTER_VERTICAL );
    m_pcbwayShipPhone = new wxTextCtrl( m_pcbwayPanel, wxID_ANY );
    shipGrid->Add( m_pcbwayShipPhone, 0, wxEXPAND );

    shipGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Street Address:" ) ),
                   0, wxALIGN_CENTER_VERTICAL );
    m_pcbwayShipAddress = new wxTextCtrl( m_pcbwayPanel, wxID_ANY );
    shipGrid->Add( m_pcbwayShipAddress, 0, wxEXPAND );

    shipGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "City:" ) ),
                   0, wxALIGN_CENTER_VERTICAL );
    m_pcbwayShipCity = new wxTextCtrl( m_pcbwayPanel, wxID_ANY );
    shipGrid->Add( m_pcbwayShipCity, 0, wxEXPAND );

    shipGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "State:" ) ),
                   0, wxALIGN_CENTER_VERTICAL );
    m_pcbwayShipState = new wxTextCtrl( m_pcbwayPanel, wxID_ANY );
    shipGrid->Add( m_pcbwayShipState, 0, wxEXPAND );

    shipGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "ZIP Code:" ) ),
                   0, wxALIGN_CENTER_VERTICAL );
    m_pcbwayShipZip = new wxTextCtrl( m_pcbwayPanel, wxID_ANY );
    shipGrid->Add( m_pcbwayShipZip, 0, wxEXPAND );

    shipGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Country:" ) ),
                   0, wxALIGN_CENTER_VERTICAL );
    m_pcbwayShipCountry = new wxTextCtrl( m_pcbwayPanel, wxID_ANY, wxT( "US" ) );
    shipGrid->Add( m_pcbwayShipCountry, 0, wxEXPAND );

    shipBox->Add( shipGrid, 0, wxEXPAND | wxALL, 8 );
    panelSizer->Add( shipBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10 );

    // ---- Order Options ----
    wxStaticBoxSizer* orderBox = new wxStaticBoxSizer( wxVERTICAL, m_pcbwayPanel,
                                                       _( "Order Options" ) );
    wxFlexGridSizer* orderGrid = new wxFlexGridSizer( 2, 6, 8 );
    orderGrid->AddGrowableCol( 1, 1 );

    // Build Days (manufacturing speed) - populated from quote response
    orderGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Build Time:" ) ),
                    0, wxALIGN_CENTER_VERTICAL );
    wxArrayString buildDaysOptions;
    buildDaysOptions.Add( wxT( "3 days (Express)" ) );
    buildDaysOptions.Add( wxT( "5 days (Standard)" ) );
    buildDaysOptions.Add( wxT( "7 days (Economy)" ) );
    buildDaysOptions.Add( wxT( "10 days (Budget)" ) );
    m_pcbwayBuildDays = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                      wxDefaultSize, buildDaysOptions );
    m_pcbwayBuildDays->SetSelection( 1 );  // Default to 5 days
    m_pcbwayBuildDays->SetToolTip( _( "Manufacturing time. Faster options cost more." ) );
    orderGrid->Add( m_pcbwayBuildDays, 0, wxEXPAND );

    // Shipping Carrier
    orderGrid->Add( new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "Shipping Carrier:" ) ),
                    0, wxALIGN_CENTER_VERTICAL );
    wxArrayString shipTypeOptions;
    shipTypeOptions.Add( wxT( "DHL Express" ) );
    shipTypeOptions.Add( wxT( "FedEx International Priority" ) );
    shipTypeOptions.Add( wxT( "FedEx International Economy" ) );
    shipTypeOptions.Add( wxT( "EMS" ) );
    shipTypeOptions.Add( wxT( "ePacket" ) );
    shipTypeOptions.Add( wxT( "China Post" ) );
    shipTypeOptions.Add( wxT( "Global Standard Shipping" ) );
    m_pcbwayShipType = new wxChoice( m_pcbwayPanel, wxID_ANY, wxDefaultPosition,
                                     wxDefaultSize, shipTypeOptions );
    m_pcbwayShipType->SetSelection( 0 );  // Default to DHL
    m_pcbwayShipType->SetToolTip( _( "Shipping carrier. Prices vary by destination." ) );
    orderGrid->Add( m_pcbwayShipType, 0, wxEXPAND );

    orderBox->Add( orderGrid, 0, wxEXPAND | wxALL, 8 );
    panelSizer->Add( orderBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10 );

    // ---- Additional Files (optional) ----
    wxStaticBoxSizer* addFilesBox = new wxStaticBoxSizer( wxVERTICAL, m_pcbwayPanel,
                                                          _( "Additional Files (optional)" ) );
    wxBoxSizer* addRow = new wxBoxSizer( wxHORIZONTAL );
    m_pcbwayAddFilesBtn = new wxButton( m_pcbwayPanel, wxID_ANY, _( "Add Files..." ) );
    m_pcbwayAddFilesBtn->SetToolTip(
            _( "Attach supplementary files: assembly drawings, special instructions, images, etc." ) );
    addRow->Add( m_pcbwayAddFilesBtn, 0, wxRIGHT, 8 );
    m_pcbwayAddFilesLabel = new wxStaticText( m_pcbwayPanel, wxID_ANY, _( "No files attached" ) );
    m_pcbwayAddFilesLabel->SetForegroundColour( wxColour( 140, 140, 140 ) );
    addRow->Add( m_pcbwayAddFilesLabel, 1, wxALIGN_CENTER_VERTICAL );
    addFilesBox->Add( addRow, 0, wxEXPAND | wxALL, 8 );
    panelSizer->Add( addFilesBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10 );

    m_pcbwayAddFilesBtn->Bind( wxEVT_BUTTON, [this]( wxCommandEvent& e ) {
        onPickAdditionalFiles( e, m_pcbwayAddFilesLabel );
    } );

    m_pcbwayPanel->SetSizer( panelSizer );

    m_notebook->AddPage( m_pcbwayPanel, wxT( "PCBWay" ), true );
}


void DIALOG_SEND_TO_MANUFACTURER::buildPikkoloPanel()
{
    m_pikkoloPanel = new wxScrolledWindow( m_notebook, wxID_ANY );
    m_pikkoloPanel->SetScrollRate( 0, 10 );
    wxBoxSizer* panelSizer = new wxBoxSizer( wxVERTICAL );

    // Auto-fill button at top
    m_pikkoloAutoFillBtn = new wxButton( m_pikkoloPanel, wxID_ANY,
                                         _( "Auto-Fill from Board" ) );
    m_pikkoloAutoFillBtn->SetToolTip(
            _( "Use AI to analyze the board and recommend parameters" ) );
    panelSizer->Add( m_pikkoloAutoFillBtn, 0, wxEXPAND | wxALL, 10 );
    m_pikkoloAutoFillBtn->Bind( wxEVT_BUTTON,
                                &DIALOG_SEND_TO_MANUFACTURER::onAutoFill, this );

    wxFlexGridSizer* grid = new wxFlexGridSizer( 2, 8, 8 );
    grid->AddGrowableCol( 1, 1 );

    // PCB source
    grid->Add( new wxStaticText( m_pikkoloPanel, wxID_ANY, _( "PCB Source:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    wxArrayString sources;
    sources.Add( _( "Fabricate with Pikkolo" ) );
    sources.Add( _( "Ship from my fab" ) );
    sources.Add( _( "Ship yourself" ) );
    m_pikkoloPcbSource = new wxChoice( m_pikkoloPanel, wxID_ANY, wxDefaultPosition,
                                       wxDefaultSize, sources );
    m_pikkoloPcbSource->SetSelection( 0 );
    grid->Add( m_pikkoloPcbSource, 0, wxEXPAND );

    // Quantity
    grid->Add( new wxStaticText( m_pikkoloPanel, wxID_ANY, _( "Board Qty:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    m_pikkoloQty = new wxTextCtrl( m_pikkoloPanel, wxID_ANY, wxT( "5" ) );
    grid->Add( m_pikkoloQty, 0, wxEXPAND );

    // Layers (auto-detected, read-only)
    grid->Add( new wxStaticText( m_pikkoloPanel, wxID_ANY, _( "Layers:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    m_pikkoloLayers = new wxStaticText( m_pikkoloPanel, wxID_ANY, wxT( "2" ) );
    grid->Add( m_pikkoloLayers, 0, wxALIGN_CENTER_VERTICAL );

    // Board area (auto-calculated, read-only)
    grid->Add( new wxStaticText( m_pikkoloPanel, wxID_ANY, _( "Board Area (sq in):" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    m_pikkoloBoardArea = new wxStaticText( m_pikkoloPanel, wxID_ANY, wxT( "0.00" ) );
    grid->Add( m_pikkoloBoardArea, 0, wxALIGN_CENTER_VERTICAL );

    // Include fabrication
    grid->Add( new wxStaticText( m_pikkoloPanel, wxID_ANY, _( "Include Fabrication:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    m_pikkoloIncludeFab = new wxCheckBox( m_pikkoloPanel, wxID_ANY, wxEmptyString );
    m_pikkoloIncludeFab->SetValue( true );
    m_pikkoloIncludeFab->SetToolTip( _( "Pikkolo will fabricate the PCB" ) );
    grid->Add( m_pikkoloIncludeFab, 0 );

    // Ship Stencil toggle
    grid->Add( new wxStaticText( m_pikkoloPanel, wxID_ANY, _( "Ship Stencil:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    m_pikkoloShipStencil = new wxCheckBox( m_pikkoloPanel, wxID_ANY, wxEmptyString );
    m_pikkoloShipStencil->SetToolTip(
            _( "Ship the stencil along with your assembled boards" ) );
    grid->Add( m_pikkoloShipStencil, 0 );

    // Double-sided assembly
    grid->Add( new wxStaticText( m_pikkoloPanel, wxID_ANY, _( "Double-Sided Assembly:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    m_pikkoloDblSidedAsm = new wxCheckBox( m_pikkoloPanel, wxID_ANY, wxEmptyString );
    m_pikkoloDblSidedAsm->SetToolTip( _( "Components on both sides of the board" ) );
    grid->Add( m_pikkoloDblSidedAsm, 0 );

    // Notes (free text for Pikkolo)
    grid->Add( new wxStaticText( m_pikkoloPanel, wxID_ANY, _( "Notes:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    m_pikkoloNotes = new wxTextCtrl( m_pikkoloPanel, wxID_ANY, wxEmptyString,
                                     wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE );
    m_pikkoloNotes->SetMinSize( wxSize( -1, 60 ) );
    m_pikkoloNotes->SetToolTip( _( "Optional notes visible to Pikkolo (e.g., special instructions)" ) );
    grid->Add( m_pikkoloNotes, 0, wxEXPAND );

    // Email for order updates
    grid->Add( new wxStaticText( m_pikkoloPanel, wxID_ANY, _( "Email:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    m_pikkoloEmail = new wxTextCtrl( m_pikkoloPanel, wxID_ANY );
    m_pikkoloEmail->SetToolTip( _( "Contact email for order updates and questions" ) );
    grid->Add( m_pikkoloEmail, 0, wxEXPAND );

    // Shipping info
    grid->Add( new wxStaticText( m_pikkoloPanel, wxID_ANY, _( "Name:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    m_pikkoloName = new wxTextCtrl( m_pikkoloPanel, wxID_ANY );
    grid->Add( m_pikkoloName, 0, wxEXPAND );

    grid->Add( new wxStaticText( m_pikkoloPanel, wxID_ANY, _( "Company:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    m_pikkoloCompany = new wxTextCtrl( m_pikkoloPanel, wxID_ANY );
    grid->Add( m_pikkoloCompany, 0, wxEXPAND );

    grid->Add( new wxStaticText( m_pikkoloPanel, wxID_ANY, _( "Street Address:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    m_pikkoloAddress = new wxTextCtrl( m_pikkoloPanel, wxID_ANY );
    grid->Add( m_pikkoloAddress, 0, wxEXPAND );

    grid->Add( new wxStaticText( m_pikkoloPanel, wxID_ANY, _( "City:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    m_pikkoloCity = new wxTextCtrl( m_pikkoloPanel, wxID_ANY );
    grid->Add( m_pikkoloCity, 0, wxEXPAND );

    grid->Add( new wxStaticText( m_pikkoloPanel, wxID_ANY, _( "State:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    m_pikkoloState = new wxTextCtrl( m_pikkoloPanel, wxID_ANY );
    grid->Add( m_pikkoloState, 0, wxEXPAND );

    grid->Add( new wxStaticText( m_pikkoloPanel, wxID_ANY, _( "ZIP Code:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    m_pikkoloZip = new wxTextCtrl( m_pikkoloPanel, wxID_ANY );
    grid->Add( m_pikkoloZip, 0, wxEXPAND );

    // Shipping method
    grid->Add( new wxStaticText( m_pikkoloPanel, wxID_ANY, _( "Shipping:" ) ),
               0, wxALIGN_CENTER_VERTICAL );
    wxArrayString shipping;
    shipping.Add( _( "Standard" ) );
    shipping.Add( _( "2 Day" ) );
    shipping.Add( _( "Overnight" ) );
    shipping.Add( _( "Local Pickup" ) );
    shipping.Add( _( "Local Dropoff (Denver Metro)" ) );
    m_pikkoloShipping = new wxChoice( m_pikkoloPanel, wxID_ANY, wxDefaultPosition,
                                      wxDefaultSize, shipping );
    m_pikkoloShipping->SetSelection( 0 );
    grid->Add( m_pikkoloShipping, 0, wxEXPAND );

    panelSizer->Add( grid, 1, wxEXPAND | wxALL, 10 );

    // ---- Additional Files (optional) ----
    wxStaticBoxSizer* pikAddFilesBox = new wxStaticBoxSizer( wxVERTICAL, m_pikkoloPanel,
                                                             _( "Additional Files (optional)" ) );
    wxBoxSizer* pikAddRow = new wxBoxSizer( wxHORIZONTAL );
    m_pikkoloAddFilesBtn = new wxButton( m_pikkoloPanel, wxID_ANY, _( "Add Files..." ) );
    m_pikkoloAddFilesBtn->SetToolTip(
            _( "Attach supplementary files: assembly drawings, special instructions, images, etc." ) );
    pikAddRow->Add( m_pikkoloAddFilesBtn, 0, wxRIGHT, 8 );
    m_pikkoloAddFilesLabel = new wxStaticText( m_pikkoloPanel, wxID_ANY, _( "No files attached" ) );
    m_pikkoloAddFilesLabel->SetForegroundColour( wxColour( 140, 140, 140 ) );
    pikAddRow->Add( m_pikkoloAddFilesLabel, 1, wxALIGN_CENTER_VERTICAL );
    pikAddFilesBox->Add( pikAddRow, 0, wxEXPAND | wxALL, 8 );
    panelSizer->Add( pikAddFilesBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10 );

    m_pikkoloAddFilesBtn->Bind( wxEVT_BUTTON, [this]( wxCommandEvent& e ) {
        onPickAdditionalFiles( e, m_pikkoloAddFilesLabel );
    } );

    m_pikkoloPanel->SetSizer( panelSizer );

    m_notebook->AddPage( m_pikkoloPanel, wxT( "Pikkolo Assembly" ), false );
}


void DIALOG_SEND_TO_MANUFACTURER::showPanelForManufacturer( const wxString& aMfrId )
{
    if( aMfrId == wxT( "pikkolo" ) )
        m_notebook->SetSelection( 1 );
    else
        m_notebook->SetSelection( 0 );
}


void DIALOG_SEND_TO_MANUFACTURER::onManufacturerChanged( wxCommandEvent& aEvent )
{
    showPanelForManufacturer( GetSelectedManufacturer() );
}


void DIALOG_SEND_TO_MANUFACTURER::appendLog( const wxString& aMsg )
{
    if( !m_aiLogCtrl->IsShown() )
    {
        m_aiLogLabel->Show();
        m_aiLogCtrl->Show();
        GetSizer()->Layout();

        // Grow the dialog to accommodate the log, but cap at 90% of screen height
        wxSize best = GetSizer()->GetMinSize();
        wxSize screen = wxGetDisplaySize();
        int maxH = static_cast<int>( screen.GetHeight() * 0.9 );
        if( best.GetHeight() > maxH )
            best.SetHeight( maxH );
        if( best.GetWidth() < GetSize().GetWidth() )
            best.SetWidth( GetSize().GetWidth() );
        SetSize( best );
    }

    m_aiLogCtrl->AppendText( aMsg + wxT( "\n" ) );
}


void DIALOG_SEND_TO_MANUFACTURER::clearLog()
{
    m_aiLogCtrl->Clear();
    m_aiLogLabel->Show();
    m_aiLogCtrl->Show();
    GetSizer()->Layout();
}


void DIALOG_SEND_TO_MANUFACTURER::onUVPrintingChanged( wxCommandEvent& aEvent )
{
    bool needFile = ( m_pcbwayUVPrinting->GetSelection() != 0 );
    m_pcbwayUVFileSizer->ShowItems( needFile );

    if( !needFile )
    {
        m_uvPrintFilePath.Clear();
        m_pcbwayUVFileLabel->SetLabel( _( "No file selected" ) );
        m_pcbwayUVFileLabel->SetForegroundColour( wxColour( 140, 140, 140 ) );
    }

    m_pcbwayPanel->FitInside();
    m_pcbwayPanel->Layout();
}


void DIALOG_SEND_TO_MANUFACTURER::onPickUVFile( wxCommandEvent& aEvent )
{
    wxFileDialog dlg( this, _( "Select Color Print Image" ), wxEmptyString, wxEmptyString,
                      _( "Image files (*.png;*.jpg;*.jpeg;*.pdf;*.ai;*.tiff;*.bmp)|"
                         "*.png;*.jpg;*.jpeg;*.pdf;*.ai;*.tiff;*.bmp|"
                         "All files (*.*)|*.*" ),
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST );

    if( dlg.ShowModal() == wxID_OK )
    {
        m_uvPrintFilePath = dlg.GetPath();
        wxFileName fn( m_uvPrintFilePath );
        m_pcbwayUVFileLabel->SetLabel( fn.GetFullName() );
        m_pcbwayUVFileLabel->SetForegroundColour( wxColour( 0, 120, 0 ) );
        m_pcbwayPanel->Layout();
    }
}


void DIALOG_SEND_TO_MANUFACTURER::onPickAdditionalFiles( wxCommandEvent& aEvent,
                                                          wxStaticText* aLabel )
{
    wxFileDialog dlg( this, _( "Select Additional Files" ), wxEmptyString, wxEmptyString,
                      _( "All supported files (*.png;*.jpg;*.jpeg;*.pdf;*.ai;*.csv;*.xlsx;*.txt;*.zip)|"
                         "*.png;*.jpg;*.jpeg;*.pdf;*.ai;*.csv;*.xlsx;*.txt;*.zip|"
                         "All files (*.*)|*.*" ),
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE );

    if( dlg.ShowModal() == wxID_OK )
    {
        wxArrayString paths;
        dlg.GetPaths( paths );

        m_additionalFiles.clear();
        for( const auto& p : paths )
            m_additionalFiles.push_back( p );

        if( m_additionalFiles.size() == 1 )
        {
            wxFileName fn( m_additionalFiles[0] );
            aLabel->SetLabel( fn.GetFullName() );
        }
        else
        {
            aLabel->SetLabel( wxString::Format( _( "%zu files selected" ),
                                                m_additionalFiles.size() ) );
        }

        aLabel->SetForegroundColour( wxColour( 0, 120, 0 ) );
        aLabel->GetParent()->Layout();
    }
}


void DIALOG_SEND_TO_MANUFACTURER::onAutoFill( wxCommandEvent& aEvent )
{
    BOARD* board = m_frame->GetBoard();
    if( !board )
    {
        m_statusLabel->SetLabel( _( "No board loaded." ) );
        Layout();
        return;
    }

    clearLog();
    appendLog( _( "[autofill] Starting board analysis..." ) );

    wxString selectedMfr = GetSelectedManufacturer();
    bool isPikkolo = ( selectedMfr == wxT( "pikkolo" ) );

    m_statusLabel->SetLabel( _( "Analyzing board..." ) );

    // Disable the appropriate autofill button
    if( isPikkolo )
        m_pikkoloAutoFillBtn->Enable( false );
    else
        m_pcbwayAutoFillBtn->Enable( false );

    m_progressGauge->Show();
    m_progressGauge->Pulse();
    Layout();
    Update();

    const BOARD_DESIGN_SETTINGS& bds = board->GetDesignSettings();

    // --- Extract board metadata ---
    nlohmann::json metadata;

    metadata["copper_layers"] = board->GetCopperLayerCount();

    BOX2I bbox = board->GetBoardEdgesBoundingBox();
    double lengthMm = bbox.GetWidth() / 1e6;
    double widthMm  = bbox.GetHeight() / 1e6;
    if( widthMm > lengthMm )
        std::swap( lengthMm, widthMm );
    metadata["length_mm"] = std::round( lengthMm * 10.0 ) / 10.0;
    metadata["width_mm"]  = std::round( widthMm * 10.0 ) / 10.0;

    metadata["board_thickness_mm"] = bds.GetBoardThickness() / 1e6;
    metadata["min_track_width_mm"] = bds.m_TrackMinWidth / 1e6;
    metadata["min_clearance_mm"]   = bds.m_MinClearance / 1e6;
    metadata["min_drill_mm"]       = bds.m_MinThroughDrill / 1e6;
    metadata["min_via_drill_mm"]   = bds.m_MinThroughDrill / 1e6;
    metadata["min_via_diameter_mm"]= bds.m_ViasMinSize / 1e6;

    // --- Actual routed trace widths (not just DRC settings) ---
    int    actualMinTrackNm = INT_MAX;
    int    actualMaxTrackNm = 0;
    int    traceCount       = 0;
    int    viaCount         = 0;
    int    microViaCount    = 0;
    int    blindBuriedCount = 0;
    bool   hasBlindBuried   = false;
    bool   hasMicroVias     = false;
    int    actualMinViaDrill = INT_MAX;

    for( PCB_TRACK* track : board->Tracks() )
    {
        if( track->Type() == PCB_VIA_T )
        {
            viaCount++;
            PCB_VIA* via = static_cast<PCB_VIA*>( track );

            int drill = via->GetDrillValue();
            if( drill > 0 && drill < actualMinViaDrill )
                actualMinViaDrill = drill;

            if( via->GetViaType() == VIATYPE::MICROVIA )
            {
                hasMicroVias = true;
                microViaCount++;
            }
            else if( via->GetViaType() != VIATYPE::THROUGH )
            {
                hasBlindBuried = true;
                blindBuriedCount++;
            }
        }
        else
        {
            traceCount++;
            int w = track->GetWidth();

            if( w > 0 && w < actualMinTrackNm )
                actualMinTrackNm = w;
            if( w > actualMaxTrackNm )
                actualMaxTrackNm = w;
        }
    }

    if( actualMinTrackNm == INT_MAX )
        actualMinTrackNm = bds.m_TrackMinWidth;
    if( actualMinViaDrill == INT_MAX )
        actualMinViaDrill = bds.m_MinThroughDrill;

    metadata["actual_min_track_mm"]  = actualMinTrackNm / 1e6;
    metadata["actual_max_track_mm"]  = actualMaxTrackNm / 1e6;
    metadata["actual_min_via_drill_mm"] = actualMinViaDrill / 1e6;
    metadata["trace_count"]          = traceCount;
    metadata["via_count"]            = viaCount;
    metadata["micro_via_count"]      = microViaCount;
    metadata["blind_buried_via_count"] = blindBuriedCount;
    metadata["has_blind_or_buried_vias"] = hasBlindBuried;
    metadata["has_micro_vias"]       = hasMicroVias;

    // --- Component analysis ---
    int  componentCount   = 0;
    int  smdCount         = 0;
    int  connectorPadCount = 0;
    bool hasBga           = false;
    bool hasFinePitch     = false;
    bool hasCastellated   = false;

    for( FOOTPRINT* fp : board->Footprints() )
    {
        componentCount++;
        if( fp->GetAttributes() & FP_SMD )
            smdCount++;

        for( PAD* pad : fp->Pads() )
        {
            if( pad->GetAttribute() == PAD_ATTRIB::CONN )
                connectorPadCount++;

            if( pad->GetProperty() == PAD_PROP::BGA )
                hasBga = true;

            if( pad->GetProperty() == PAD_PROP::CASTELLATED )
                hasCastellated = true;

            if( pad->GetAttribute() == PAD_ATTRIB::SMD )
            {
                if( pad->GetShape( F_Cu ) == PAD_SHAPE::CIRCLE && pad->GetSizeX() < 500000 )
                    hasBga = true;

                if( pad->GetSizeX() < 300000 )
                    hasFinePitch = true;
            }
        }
    }

    metadata["component_count"] = componentCount;
    metadata["smd_percentage"]  = componentCount > 0
            ? ( static_cast<double>( smdCount ) / componentCount ) * 100.0 : 0.0;
    metadata["has_bga"]            = hasBga;
    metadata["has_fine_pitch"]     = hasFinePitch;
    metadata["has_castellated"]    = hasCastellated;
    metadata["edge_connector_pads"] = connectorPadCount;

    // --- Stackup analysis ---
    const BOARD_STACKUP& stackup = bds.GetStackupDescriptor();
    bool hasCustomStackup = bds.m_HasStackup;

    nlohmann::json stackupJson = nlohmann::json::array();
    double maxCopperOz = 0.0;

    if( hasCustomStackup )
    {
        for( const BOARD_STACKUP_ITEM* item : stackup.GetList() )
        {
            if( !item->IsEnabled() )
                continue;

            nlohmann::json layer;

            if( item->GetType() == BS_ITEM_TYPE_COPPER )
            {
                double thickUm = item->GetThickness() / 1000.0;
                double oz = thickUm / 35.0;
                if( oz > maxCopperOz )
                    maxCopperOz = oz;

                layer["type"]        = "copper";
                layer["name"]        = item->GetLayerName().ToStdString();
                layer["thickness_um"] = thickUm;
                layer["weight_oz"]   = std::round( oz * 10.0 ) / 10.0;
            }
            else if( item->GetType() == BS_ITEM_TYPE_DIELECTRIC )
            {
                layer["type"]     = "dielectric";
                layer["name"]     = item->GetLayerName().ToStdString();

                for( int sub = 0; sub < item->GetSublayersCount(); sub++ )
                {
                    layer["material"]     = item->GetMaterial( sub ).ToStdString();
                    layer["thickness_mm"] = item->GetThickness( sub ) / 1e6;
                    layer["epsilon_r"]    = item->GetEpsilonR( sub );
                    layer["loss_tangent"] = item->GetLossTangent( sub );
                }
            }
            else if( item->GetType() == BS_ITEM_TYPE_SOLDERMASK )
            {
                layer["type"]  = "solder_mask";
                layer["color"] = item->GetColor().ToStdString();
            }
            else if( item->GetType() == BS_ITEM_TYPE_SILKSCREEN )
            {
                layer["type"]  = "silkscreen";
                layer["color"] = item->GetColor().ToStdString();
            }
            else
            {
                continue;
            }

            stackupJson.push_back( layer );
        }
    }

    metadata["has_custom_stackup"] = hasCustomStackup;
    metadata["stackup"]            = stackupJson;
    metadata["max_copper_weight_oz"] = maxCopperOz > 0 ? maxCopperOz : 1.0;

    // Stackup-level edge connector flag
    bool stackupEdgeConn = ( stackup.m_EdgeConnectorConstraints != BS_EDGE_CONNECTOR_NONE );
    bool edgePlating     = stackup.m_EdgePlating;

    metadata["has_edge_connector"] = ( stackupEdgeConn || connectorPadCount > 0 );
    metadata["edge_connector_bevelled"] =
            ( stackup.m_EdgeConnectorConstraints == BS_EDGE_CONNECTOR_BEVELLED );
    metadata["has_edge_plating"] = edgePlating;

    wxString finishType = stackup.m_FinishType;
    if( !finishType.IsEmpty() )
        metadata["stackup_finish_type"] = finishType.ToStdString();

    metadata["stackup_has_dielectric_constraints"] = stackup.m_HasDielectricConstrains;

    // --- Net class analysis ---
    bool hasDiffPairs      = false;
    bool hasImpedanceCtrl  = false;
    int  netclassCount     = 0;
    nlohmann::json netclassesJson = nlohmann::json::array();

    if( bds.m_NetSettings )
    {
        auto defaultNc = bds.m_NetSettings->GetDefaultNetclass();
        const auto& netclasses = bds.m_NetSettings->GetNetclasses();
        netclassCount = static_cast<int>( netclasses.size() ) + 1;

        for( const auto& [name, nc] : netclasses )
        {
            nlohmann::json ncJson;
            ncJson["name"] = name.ToStdString();

            if( nc->HasTrackWidth() )
                ncJson["track_width_mm"] = nc->GetTrackWidth() / 1e6;
            if( nc->HasClearance() )
                ncJson["clearance_mm"] = nc->GetClearance() / 1e6;

            if( nc->HasDiffPairWidth() )
            {
                hasDiffPairs = true;
                ncJson["diff_pair_width_mm"] = nc->GetDiffPairWidth() / 1e6;
            }
            if( nc->HasDiffPairGap() )
            {
                hasDiffPairs = true;
                ncJson["diff_pair_gap_mm"] = nc->GetDiffPairGap() / 1e6;
            }

            netclassesJson.push_back( ncJson );
        }
    }

    hasImpedanceCtrl = hasDiffPairs || stackup.m_HasDielectricConstrains;

    metadata["has_diff_pairs"]       = hasDiffPairs;
    metadata["has_impedance_control"] = hasImpedanceCtrl;
    metadata["netclass_count"]       = netclassCount;
    metadata["netclasses"]           = netclassesJson;

    // --- Zone / plane analysis ---
    int  copperZoneCount = 0;
    bool hasGroundPlane  = false;
    bool hasPowerPlane   = false;

    for( ZONE* zone : board->Zones() )
    {
        if( zone->GetIsRuleArea() )
            continue;
        if( !zone->IsOnCopperLayer() )
            continue;

        copperZoneCount++;
        wxString netname = zone->GetNetname().Upper();

        if( netname.Contains( wxT( "GND" ) ) || netname.Contains( wxT( "GROUND" ) )
            || netname.Contains( wxT( "VSS" ) ) || netname.Contains( wxT( "AGND" ) )
            || netname.Contains( wxT( "DGND" ) ) )
        {
            hasGroundPlane = true;
        }

        if( netname.Contains( wxT( "VCC" ) ) || netname.Contains( wxT( "VDD" ) )
            || netname.Contains( wxT( "3V3" ) ) || netname.Contains( wxT( "5V" ) )
            || netname.Contains( wxT( "12V" ) ) || netname.Contains( wxT( "POWER" ) )
            || netname.Contains( wxT( "PWR" ) ) )
        {
            hasPowerPlane = true;
        }
    }

    metadata["copper_zone_count"] = copperZoneCount;
    metadata["has_ground_plane"]  = hasGroundPlane;
    metadata["has_power_plane"]   = hasPowerPlane;

    metadata["manufacturer"] = GetSelectedManufacturer().ToStdString();

    // Log extracted metadata
    appendLog( wxString::Format(
            _( "[autofill] Board: %d layers, %.1f x %.1f mm, %.1fmm thick" ),
            board->GetCopperLayerCount(), lengthMm, widthMm,
            bds.GetBoardThickness() / 1e6 ) );
    appendLog( wxString::Format(
            _( "[autofill] Traces: %d routed, min=%.3fmm, max=%.3fmm" ),
            traceCount, actualMinTrackNm / 1e6, actualMaxTrackNm / 1e6 ) );
    appendLog( wxString::Format(
            _( "[autofill] Vias: %d total (%d blind/buried, %d micro), minDrill=%.3fmm" ),
            viaCount, blindBuriedCount, microViaCount, actualMinViaDrill / 1e6 ) );
    appendLog( wxString::Format(
            _( "[autofill] Components: %d total (%d%% SMD), BGA=%s, FinePitch=%s" ),
            componentCount,
            componentCount > 0 ? ( smdCount * 100 / componentCount ) : 0,
            hasBga ? "yes" : "no",
            hasFinePitch ? "yes" : "no" ) );
    appendLog( wxString::Format(
            _( "[autofill] EdgeConn=%s (pads=%d), Castellated=%s, EdgePlating=%s" ),
            ( stackupEdgeConn || connectorPadCount > 0 ) ? "yes" : "no",
            connectorPadCount,
            hasCastellated ? "yes" : "no",
            edgePlating ? "yes" : "no" ) );
    appendLog( wxString::Format(
            _( "[autofill] Stackup: custom=%s, maxCu=%.1foz, dielectricCtrl=%s" ),
            hasCustomStackup ? "yes" : "no",
            maxCopperOz > 0 ? maxCopperOz : 1.0,
            stackup.m_HasDielectricConstrains ? "yes" : "no" ) );
    appendLog( wxString::Format(
            _( "[autofill] Net classes: %d, diffPairs=%s, impedanceCtrl=%s" ),
            netclassCount,
            hasDiffPairs ? "yes" : "no",
            hasImpedanceCtrl ? "yes" : "no" ) );
    appendLog( wxString::Format(
            _( "[autofill] Zones: %d copper (GND plane=%s, PWR plane=%s)" ),
            copperZoneCount,
            hasGroundPlane ? "yes" : "no",
            hasPowerPlane ? "yes" : "no" ) );

    // --- POST to backend ---
    std::string backendUrl = GetTraceBackendUrl().ToStdString();
    std::string url = backendUrl + "/manufacturer/autofill";
    std::string body = metadata.dump();
    std::string mfrId = selectedMfr.ToStdString();

    appendLog( wxString::Format( _( "[autofill] Sending to %s ..." ),
                                 wxString::FromUTF8( url ) ) );

    wxString authToken = AUTH_MANAGER::Instance().GetAuthToken();

    std::thread t( [this, url, body, authToken, mfrId]()
    {
        // Guard: if dialog is closing, abort early
        if( m_closing.load() ) return;

        nlohmann::json response;
        bool success = false;
        std::string errorDetail;
        std::string rawBody;
        int httpCode = 0;
        bool isPikkolo = ( mfrId == "pikkolo" );

        try
        {
            KICAD_CURL_EASY curl;
            curl.SetURL( url );
            curl.SetPostFields( body );
            curl.SetHeader( "Content-Type", "application/json" );

            if( !authToken.IsEmpty() )
                curl.SetHeader( "Authorization",
                                std::string( "Bearer " ) + authToken.ToStdString() );

            curl_easy_setopt( curl.GetCurl(), CURLOPT_TIMEOUT, 30L );

            int curlResult = curl.Perform();

            if( curlResult != CURLE_OK )
            {
                errorDetail = "CURL error " + std::to_string( curlResult )
                              + ": " + curl.GetErrorText( curlResult );
            }
            else
            {
                httpCode = curl.GetResponseStatusCode();
                rawBody = curl.GetBuffer();

                if( httpCode != 200 )
                {
                    errorDetail = "HTTP " + std::to_string( httpCode );
                    if( rawBody.size() < 500 )
                        errorDetail += ": " + rawBody;
                }
                else
                {
                    response = nlohmann::json::parse( rawBody, nullptr, false );

                    if( response.is_discarded() )
                    {
                        errorDetail = "Invalid JSON in response";
                    }
                    else if( !response.value( "success", false ) )
                    {
                        errorDetail = "Backend returned success=false";
                        if( response.contains( "error" ) )
                            errorDetail += ": " + response["error"].get<std::string>();
                    }
                    else
                    {
                        success = true;
                    }
                }
            }
        }
        catch( const std::exception& ex )
        {
            errorDetail = std::string( "Exception: " ) + ex.what();
        }
        catch( ... )
        {
            errorDetail = "Unknown exception during autofill request";
        }

        CallAfter( [this, success, response, errorDetail, rawBody, isPikkolo]()
        {
            // Re-enable the appropriate autofill button
            if( isPikkolo )
                m_pikkoloAutoFillBtn->Enable( true );
            else
                m_pcbwayAutoFillBtn->Enable( true );

            m_progressGauge->Hide();
            Layout();

            if( !success )
            {
                appendLog( wxString::Format( _( "[autofill] ERROR: %s" ),
                                             wxString::FromUTF8( errorDetail ) ) );
                appendLog( _( "[autofill] Falling back to board defaults." ) );
                m_statusLabel->SetLabel(
                        _( "AI autofill failed \u2014 filled from board data." ) );
                applyBoardDefaults();
                if( isPikkolo )
                    m_pikkoloPanel->Refresh();
                else
                {
                    m_pcbwayPanel->Refresh();
                    m_pcbwayPanel->FitInside();
                }
                Layout();
                return;
            }

            appendLog( _( "[autofill] AI response received. Applying parameters..." ) );

            nlohmann::json params = response["params"];

            if( isPikkolo )
            {
                // Apply Pikkolo-specific parameters
                appendLog( wxString::Format( _( "[autofill] AI recommended: Qty=%d, "
                        "Layers=%d, Area=%.2f sq in" ),
                        params.value( "qty_boards", 5 ),
                        params.value( "fab_layers", 2 ),
                        params.value( "board_area_sq_in", 0.0 ) ) );

                // Quantity
                m_pikkoloQty->SetValue( wxString::Format( wxT( "%d" ),
                        params.value( "qty_boards", 5 ) ) );

                // Layers (read-only display)
                m_pikkoloLayers->SetLabel( wxString::Format( wxT( "%d" ),
                        params.value( "fab_layers", 2 ) ) );

                // Board area (read-only display)
                m_pikkoloBoardArea->SetLabel( wxString::Format( wxT( "%.2f" ),
                        params.value( "board_area_sq_in", 0.0 ) ) );

                // Include fabrication
                m_pikkoloIncludeFab->SetValue( params.value( "include_fab", true ) );

                // Ship stencil
                m_pikkoloShipStencil->SetValue( params.value( "include_stencils", false ) );

                // Double-sided assembly
                m_pikkoloDblSidedAsm->SetValue( params.value( "double_sided_assembly", false ) );

                // Reasoning
                std::string reasoning = params.value( "reasoning", "" );
                if( !reasoning.empty() )
                    appendLog( wxString::Format( _( "[autofill] Reasoning: %s" ),
                                                 wxString::FromUTF8( reasoning ) ) );

                appendLog( _( "[autofill] Done. All parameters applied." ) );
                m_statusLabel->SetLabel( _( "AI auto-fill complete." ) );

                m_pikkoloPanel->Refresh();
                Layout();
            }
            else
            {
                // Apply PCBWay-specific parameters (existing logic)
                // Log the raw AI recommendations
                appendLog( wxString::Format( _( "[autofill] AI recommended: Layers=%d, "
                        "Material=%s, Thickness=%.1f, Surface=%s" ),
                        params.value( "Layers", 0 ),
                        wxString::FromUTF8( params.value( "Material", "?" ) ),
                        params.value( "Thickness", 0.0 ),
                        wxString::FromUTF8( params.value( "SurfaceFinish", "?" ) ) ) );
                appendLog( wxString::Format( _( "[autofill] TrackSpacing=%s, "
                        "HoleSize=%.2f, Via=%s, Copper=%s" ),
                        wxString::FromUTF8( params.value( "MinTrackSpacing", "?" ) ),
                        params.value( "MinHoleSize", 0.0 ),
                        wxString::FromUTF8( params.value( "ViaProcess", "?" ) ),
                        wxString::FromUTF8( params.value( "FinishedCopper", "?" ) ) ) );

                // ---- Apply all parameters ----

                // Board type (string)
                {
                    std::string btStr = params.value( "BoardType", "Single PCB" );
                    static const std::map<std::string, int> btMap = {
                        {"Single PCB", 0}, {"Panel by Customer", 1}, {"Panel by Supplier", 2}
                    };
                    auto it = btMap.find( btStr );
                    m_pcbwayBoardType->SetSelection( it != btMap.end() ? it->second : 0 );
                }

                // Layers
                int layers = params.value( "Layers", 2 );
                wxString layerStr = wxString::Format( wxT( "%d" ), layers );
                int layerIdx = m_pcbwayLayers->FindString( layerStr );
                m_pcbwayLayers->SetSelection( layerIdx != wxNOT_FOUND ? layerIdx : 1 );

                // Dimensions
                m_pcbwayLength->SetValue( wxString::Format( wxT( "%.1f" ),
                        params.value( "Length", 100.0 ) ) );
                m_pcbwayWidth->SetValue( wxString::Format( wxT( "%.1f" ),
                        params.value( "Width", 100.0 ) ) );

                // Quantity
                m_pcbwayQty->SetValue( wxString::Format( wxT( "%d" ),
                        params.value( "Qty", 5 ) ) );

                // Material (string -> index)
                {
                    std::string matStr = params.value( "Material", "FR-4" );
                    static const std::map<std::string, int> matMap = {
                        {"FR-4", 0}, {"Aluminum board", 1}, {"Rogers", 2}, {"HDI", 3}, {"Copper", 4}
                    };
                    auto it = matMap.find( matStr );
                    m_pcbwayMaterial->SetSelection( it != matMap.end() ? it->second : 0 );
                }

                // FR4-TG (string -> index)
                {
                    std::string tgStr = params.value( "FR4Tg", "TG 130-140" );
                    static const std::map<std::string, int> tgMap = {
                        {"TG130", 0}, {"TG150", 1}, {"TG170", 2},
                        {"S1000H TG150", 3}, {"S1000-2M TG170", 4}
                    };
                    auto it = tgMap.find( tgStr );
                    m_pcbwayFR4Tg->SetSelection( it != tgMap.end() ? it->second : 0 );
                }

                // Thickness
                double thickness = params.value( "Thickness", 1.6 );
                m_pcbwayThickness->SetSelection(
                        findClosestChoice( m_pcbwayThickness, thickness ) );

                // Copper weight (string -> index)
                {
                    std::string cwStr = params.value( "FinishedCopper", "1 oz Cu" );
                    static const std::map<std::string, int> cwMap = {
                        {"1 oz Cu", 0}, {"2 oz Cu", 1}, {"3 oz Cu", 2}, {"4 oz Cu", 3},
                        {"5 oz Cu", 4}, {"6 oz Cu", 5}, {"7 oz Cu", 6}, {"8 oz Cu", 7},
                        {"9 oz Cu", 8}, {"10 oz Cu", 9}, {"11 oz Cu", 10}, {"12 oz Cu", 11},
                        {"13 oz Cu", 12}
                    };
                    auto it = cwMap.find( cwStr );
                    m_pcbwayCopperWeight->SetSelection( it != cwMap.end() ? it->second : 0 );
                }

                // Surface finish (string -> index)
                {
                    std::string sfStr = params.value( "SurfaceFinish", "HASL lead free" );
                    static const std::map<std::string, int> sfMap = {
                        {"HASL with lead", 0}, {"HASL lead free", 1}, {"Immersion gold", 2},
                        {"OSP", 3}, {"Hard Gold", 4}, {"Immersion Silver", 5}, {"Immersion Tin", 6},
                        {"HASL lead free+Selective Immersion gold", 7},
                        {"HASL lead free+Selective Hard gold", 8},
                        {"Immersion gold+Selective Hard gold", 9}, {"ENEPIG", 10}, {"None", 11}
                    };
                    auto it = sfMap.find( sfStr );
                    m_pcbwaySurface->SetSelection( it != sfMap.end() ? it->second : 1 );
                }

                // Solder mask
                wxString mask = wxString::FromUTF8( params.value( "SolderMask", "Green" ) );
                int maskIdx = m_pcbwaySolderMask->FindString( mask );
                m_pcbwaySolderMask->SetSelection( maskIdx != wxNOT_FOUND ? maskIdx : 0 );

                // Silkscreen
                wxString silk = wxString::FromUTF8( params.value( "Silkscreen", "White" ) );
                int silkIdx = m_pcbwaySilkscreen->FindString( silk );
                m_pcbwaySilkscreen->SetSelection( silkIdx != wxNOT_FOUND ? silkIdx : 0 );

                // Min track/spacing
                wxString trackStr = wxString::FromUTF8(
                        params.value( "MinTrackSpacing", "6/6mil" ) );
                int trackIdx = m_pcbwayMinTrackSpacing->FindString( trackStr );
                m_pcbwayMinTrackSpacing->SetSelection( trackIdx != wxNOT_FOUND ? trackIdx : 3 );

                // Min hole size
                double holeSize = params.value( "MinHoleSize", 0.3 );
                if( holeSize < 0 )
                {
                    int noDrillIdx = m_pcbwayMinHoleSize->FindString( wxT( "No Drill" ) );
                    m_pcbwayMinHoleSize->SetSelection( noDrillIdx != wxNOT_FOUND ? noDrillIdx : 3 );
                }
                else
                {
                    m_pcbwayMinHoleSize->SetSelection(
                            findClosestChoice( m_pcbwayMinHoleSize, holeSize ) );
                }

                // Via process (string -> index)
                {
                    std::string vpStr = params.value( "ViaProcess", "Tenting vias" );
                    static const std::map<std::string, int> vpMap = {
                        {"Tenting vias", 0}, {"Plugged vias", 1}, {"Vias not covered", 2}
                    };
                    auto it = vpMap.find( vpStr );
                    m_pcbwayViaProcess->SetSelection( it != vpMap.end() ? it->second : 0 );
                }

                // UV printing
                int uvVal = params.value( "UVPrinting", 0 );
                m_pcbwayUVPrinting->SetSelection(
                        std::clamp( uvVal, 0, (int) m_pcbwayUVPrinting->GetCount() - 1 ) );

                // Edge connector
                m_pcbwayEdgeConnector->SetValue( params.value( "EdgeConnector", false ) );

                // Remove product No (string -> index)
                {
                    std::string rpStr = params.value( "RemoveProductNo", "No" );
                    static const std::map<std::string, int> rpMap = {
                        {"No", 0}, {"Yes", 1}, {"Specify a location", 2}
                    };
                    auto it = rpMap.find( rpStr );
                    m_pcbwayRemoveProductNo->SetSelection( it != rpMap.end() ? it->second : 0 );
                }

                // Reasoning
                std::string reasoning = params.value( "reasoning", "" );
                if( !reasoning.empty() )
                    appendLog( wxString::Format( _( "[autofill] Reasoning: %s" ),
                                                 wxString::FromUTF8( reasoning ) ) );

                appendLog( _( "[autofill] Done. All parameters applied." ) );
                m_statusLabel->SetLabel( _( "AI auto-fill complete." ) );

                m_pcbwayPanel->Refresh();
                m_pcbwayPanel->FitInside();
                Layout();
            }
        } );
    } );

    {
        std::lock_guard<std::mutex> lock( m_threadMutex );
        if( m_autofillThread.joinable() )
            m_autofillThread.join();
        m_autofillThread = std::move( t );
    }
}


void DIALOG_SEND_TO_MANUFACTURER::applyBoardDefaults()
{
    BOARD* board = m_frame->GetBoard();
    if( !board )
        return;

    const BOARD_DESIGN_SETTINGS& bds = board->GetDesignSettings();

    // Layers
    int copperLayers = board->GetCopperLayerCount();
    wxString layerStr = wxString::Format( wxT( "%d" ), copperLayers );
    int layerIdx = m_pcbwayLayers->FindString( layerStr );
    m_pcbwayLayers->SetSelection( layerIdx != wxNOT_FOUND ? layerIdx : 1 );

    // Dimensions
    BOX2I bbox = board->GetBoardEdgesBoundingBox();
    if( bbox.GetWidth() > 0 && bbox.GetHeight() > 0 )
    {
        double lengthMm = bbox.GetWidth() / 1e6;
        double widthMm  = bbox.GetHeight() / 1e6;
        if( widthMm > lengthMm )
            std::swap( lengthMm, widthMm );
        m_pcbwayLength->SetValue( wxString::Format( wxT( "%.1f" ), lengthMm ) );
        m_pcbwayWidth->SetValue( wxString::Format( wxT( "%.1f" ), widthMm ) );
    }

    // Thickness
    m_pcbwayThickness->SetSelection(
            findClosestChoice( m_pcbwayThickness, bds.GetBoardThickness() / 1e6 ) );

    // Min track/spacing
    double minTrackMil = bds.m_TrackMinWidth / 25400.0;
    const double trackOptions[] = { 3.0, 3.5, 4.0, 5.0, 6.0, 8.0 };
    int trackIdx = 4;  // 6/6mil default
    for( int i = 0; i < 6; ++i )
    {
        if( minTrackMil <= trackOptions[i] + 0.25 )
        {
            trackIdx = i;
            break;
        }
    }
    m_pcbwayMinTrackSpacing->SetSelection( trackIdx );

    // Min hole size
    m_pcbwayMinHoleSize->SetSelection(
            findClosestChoice( m_pcbwayMinHoleSize, bds.m_MinThroughDrill / 1e6 ) );

    m_pcbwayPanel->Refresh();
    m_pcbwayPanel->FitInside();

    // --- Pikkolo defaults ---
    if( m_pikkoloLayers )
        m_pikkoloLayers->SetLabel( wxString::Format( wxT( "%d" ), copperLayers ) );

    if( m_pikkoloBoardArea && bbox.GetWidth() > 0 && bbox.GetHeight() > 0 )
    {
        double lengthMm = bbox.GetWidth() / 1e6;
        double widthMm  = bbox.GetHeight() / 1e6;
        double areaSqIn = ( lengthMm * widthMm ) / 645.16;  // mm² to sq in
        m_pikkoloBoardArea->SetLabel( wxString::Format( wxT( "%.2f" ), areaSqIn ) );
    }
}


void DIALOG_SEND_TO_MANUFACTURER::onGenerateFiles( wxCommandEvent& aEvent )
{
    BOARD* board = m_frame->GetBoard();

    if( !board )
    {
        m_statusLabel->SetLabel( _( "Error: No board loaded." ) );
        Layout();
        return;
    }

    wxString boardFile = board->GetFileName();

    if( boardFile.IsEmpty() )
    {
        m_statusLabel->SetLabel( _( "Error: Board must be saved before generating files." ) );
        Layout();
        return;
    }

    wxFileName boardFn( boardFile );
    wxString   projectDir = boardFn.GetPath();
    wxString   outDir = projectDir + wxFileName::GetPathSeparator() + wxT( "gerbers" );

    clearLog();
    appendLog( _( "=== Generating production files ===" ) );
    appendLog( wxString::Format( _( "Output directory: %s" ), outDir ) );

    m_statusLabel->SetLabel( _( "Generating Gerber files..." ) );
    m_generateBtn->Enable( false );
    m_progressGauge->Show();
    m_progressGauge->Pulse();
    Layout();
    wxYield();

    // --- Gerber generation via job system ---
    {
        std::unique_ptr<JOB_EXPORT_PCB_GERBERS> gerberJob( new JOB_EXPORT_PCB_GERBERS() );
        gerberJob->m_filename          = boardFile;
        gerberJob->m_useBoardPlotParams = true;
        gerberJob->m_createJobsFile    = true;
        gerberJob->m_useX2Format       = true;
        gerberJob->m_includeNetlistAttributes = true;
        gerberJob->m_useProtelFileExtension   = true;
        gerberJob->m_checkZonesBeforePlot     = true;
        gerberJob->SetConfiguredOutputPath( outDir );

        appendLog( _( "Running Gerber export..." ) );

        NULL_REPORTER reporter;
        int exitCode = m_frame->Kiway().ProcessJob( KIWAY::FACE_PCB, gerberJob.get(),
                                                     &reporter, nullptr );
        if( exitCode != 0 )
        {
            appendLog( wxString::Format( _( "ERROR: Gerber export failed (exit code %d)" ),
                                          exitCode ) );
            m_statusLabel->SetLabel( _( "Gerber generation failed." ) );
            m_generateBtn->Enable( true );
            Layout();
            return;
        }

        appendLog( _( "Gerber files generated successfully." ) );
    }

    m_statusLabel->SetLabel( _( "Generating drill files..." ) );
    Layout();
    wxYield();

    // --- Drill generation via job system ---
    {
        std::unique_ptr<JOB_EXPORT_PCB_DRILL> drillJob( new JOB_EXPORT_PCB_DRILL() );
        drillJob->m_filename              = boardFile;
        drillJob->m_format                = JOB_EXPORT_PCB_DRILL::DRILL_FORMAT::EXCELLON;
        drillJob->m_drillUnits            = JOB_EXPORT_PCB_DRILL::DRILL_UNITS::MM;
        drillJob->m_zeroFormat            = JOB_EXPORT_PCB_DRILL::ZEROS_FORMAT::DECIMAL;
        drillJob->m_drillOrigin           = JOB_EXPORT_PCB_DRILL::DRILL_ORIGIN::ABS;
        drillJob->m_excellonMirrorY       = false;
        drillJob->m_excellonMinimalHeader = false;
        drillJob->m_excellonCombinePTHNPTH = false;
        drillJob->m_excellonOvalDrillRoute = true;
        drillJob->m_generateMap           = false;
        drillJob->m_generateTenting       = false;
        drillJob->SetConfiguredOutputPath( outDir );

        appendLog( _( "Running Excellon drill export..." ) );

        NULL_REPORTER reporter;
        int exitCode = m_frame->Kiway().ProcessJob( KIWAY::FACE_PCB, drillJob.get(),
                                                     &reporter, nullptr );
        if( exitCode != 0 )
        {
            appendLog( wxString::Format( _( "ERROR: Drill export failed (exit code %d)" ),
                                          exitCode ) );
            m_statusLabel->SetLabel( _( "Drill generation failed." ) );
            m_generateBtn->Enable( true );
            Layout();
            return;
        }

        appendLog( _( "Drill files generated successfully." ) );
    }

    m_statusLabel->SetLabel( _( "Creating zip archive..." ) );
    Layout();
    wxYield();

    // --- Zip all gerber + drill files ---
    {
        wxDir dir( outDir );

        if( !dir.IsOpened() )
        {
            appendLog( _( "ERROR: Could not open output directory for zipping." ) );
            m_statusLabel->SetLabel( _( "Zip packaging failed." ) );
            m_generateBtn->Enable( true );
            Layout();
            return;
        }

        std::vector<wxString> filesToZip;
        wxString              fname;
        bool                  found = dir.GetFirst( &fname, wxEmptyString, wxDIR_FILES );

        while( found )
        {
            wxString ext = fname.AfterLast( '.' ).Lower();

            if( ext == wxT( "gbr" ) || ext == wxT( "drl" ) || ext == wxT( "gbrjob" ) )
                filesToZip.push_back( outDir + wxFileName::GetPathSeparator() + fname );

            found = dir.GetNext( &fname );
        }

        if( filesToZip.empty() )
        {
            appendLog( _( "WARNING: No .gbr/.drl files found to zip." ) );
            m_statusLabel->SetLabel( _( "No production files found to package." ) );
            m_generateBtn->Enable( true );
            Layout();
            return;
        }

        wxString zipName = boardFn.GetName() + wxT( "_gerbers.zip" );
        wxString zipPath = projectDir + wxFileName::GetPathSeparator() + zipName;

        wxFFileOutputStream outputStream( zipPath );

        if( !outputStream.IsOk() )
        {
            appendLog( wxString::Format( _( "ERROR: Could not create zip file: %s" ), zipPath ) );
            m_statusLabel->SetLabel( _( "Zip creation failed." ) );
            m_generateBtn->Enable( true );
            Layout();
            return;
        }

        wxZipOutputStream zipStream( outputStream, -1, wxConvUTF8 );

        for( const wxString& fullPath : filesToZip )
        {
            wxFileName fn( fullPath );
            wxString   entryName = fn.GetFullName();

            wxFFileInputStream fileStream( fullPath );

            if( !fileStream.IsOk() )
                continue;

            zipStream.PutNextEntry( entryName );
            zipStream.Write( fileStream );
            zipStream.CloseEntry();
        }

        zipStream.Close();
        outputStream.Close();

        m_generatedZipPath = zipPath;

        appendLog( wxString::Format( _( "Zip archive created: %s" ), zipPath ) );
        appendLog( wxString::Format( _( "%zu files included." ),
                                      filesToZip.size() ) );
    }

    // --- Generate Position File (Pick and Place) ---
    m_statusLabel->SetLabel( _( "Generating position file..." ) );
    Layout();
    wxYield();

    {
        PLACE_FILE_EXPORTER exporter( board, true, false, false, true, false, true, true, true, false, false );
        std::string posData = exporter.GenPositionData();
        int fpCount = exporter.GetFootprintCount();

        m_positionFilePath = projectDir + wxFileName::GetPathSeparator()
                             + boardFn.GetName() + wxT( "_pos_all.csv" );

        wxFile posFile( m_positionFilePath, wxFile::write );
        if( posFile.IsOpened() )
        {
            posFile.Write( wxString::FromUTF8( posData ) );
            posFile.Close();
            appendLog( wxString::Format( _( "Position file created: %s (%d footprints)" ),
                                          m_positionFilePath, fpCount ) );
        }
        else
        {
            appendLog( _( "WARNING: Could not create position file." ) );
            m_positionFilePath.Clear();
        }
    }

    // --- Generate BOM (CSV and JSON) ---
    m_statusLabel->SetLabel( _( "Generating BOM..." ) );
    Layout();
    wxYield();

    {
        m_bomCsvPath = projectDir + wxFileName::GetPathSeparator()
                       + boardFn.GetName() + wxT( "_bom.csv" );
        m_bomJsonPath = projectDir + wxFileName::GetPathSeparator()
                        + boardFn.GetName() + wxT( "_bom.json" );

        struct BomRow
        {
            wxString              val;
            LIB_ID                fpid;
            wxString              mpn;
            wxString              manufacturer;
            wxString              description;
            std::vector<wxString> refs;
            int                   count = 0;
        };

        std::vector<BomRow> bomList;

        for( FOOTPRINT* fp : board->Footprints() )
        {
            if( fp->GetAttributes() & FP_EXCLUDE_FROM_BOM )
                continue;

            wxString ref = fp->Reference().GetShownText( false );
            wxString val = fp->Value().GetShownText( false );
            LIB_ID   fpid = fp->GetFPID();

            wxString mpn;
            wxString mfr;
            wxString desc;

            // Check common field names for MPN (Manufacturer Part Number)
            if( fp->HasField( wxT( "MPN" ) ) )
                mpn = fp->GetField( wxT( "MPN" ) )->GetShownText( false );
            else if( fp->HasField( wxT( "Mpn" ) ) )
                mpn = fp->GetField( wxT( "Mpn" ) )->GetShownText( false );
            else if( fp->HasField( wxT( "PartNumber" ) ) )
                mpn = fp->GetField( wxT( "PartNumber" ) )->GetShownText( false );
            else if( fp->HasField( wxT( "Part Number" ) ) )
                mpn = fp->GetField( wxT( "Part Number" ) )->GetShownText( false );
            else if( fp->HasField( wxT( "Mfr_PN" ) ) )
                mpn = fp->GetField( wxT( "Mfr_PN" ) )->GetShownText( false );
            else if( fp->HasField( wxT( "Mfr. Part #" ) ) )
                mpn = fp->GetField( wxT( "Mfr. Part #" ) )->GetShownText( false );

            // Check common field names for Manufacturer
            if( fp->HasField( wxT( "Manufacturer" ) ) )
                mfr = fp->GetField( wxT( "Manufacturer" ) )->GetShownText( false );
            else if( fp->HasField( wxT( "MF" ) ) )
                mfr = fp->GetField( wxT( "MF" ) )->GetShownText( false );
            else if( fp->HasField( wxT( "Mfr" ) ) )
                mfr = fp->GetField( wxT( "Mfr" ) )->GetShownText( false );

            const PCB_FIELD* descField = fp->GetField( FIELD_T::DESCRIPTION );
            if( descField )
                desc = descField->GetShownText( false );

            bool found = false;
            for( BomRow& row : bomList )
            {
                if( row.val == val && row.fpid == fpid && row.mpn == mpn )
                {
                    row.refs.push_back( ref );
                    row.count++;
                    found = true;
                    break;
                }
            }

            if( !found )
            {
                BomRow row;
                row.val = val;
                row.fpid = fpid;
                row.mpn = mpn;
                row.manufacturer = mfr;
                row.description = desc;
                row.refs.push_back( ref );
                row.count = 1;
                bomList.push_back( row );
            }
        }

        for( BomRow& row : bomList )
        {
            std::sort( row.refs.begin(), row.refs.end(),
                    []( const wxString& a, const wxString& b )
                    { return StrNumCmp( a, b, true ) < 0; } );
        }

        std::sort( bomList.begin(), bomList.end(),
                []( const BomRow& a, const BomRow& b )
                { return StrNumCmp( a.refs[0], b.refs[0], true ) < 0; } );

        // Check if any items are missing MPN and need enrichment
        bool needsEnrichment = false;
        for( const BomRow& row : bomList )
        {
            if( row.mpn.IsEmpty() && !row.refs.empty() && !row.refs[0].StartsWith( wxT( "#" ) ) )
            {
                needsEnrichment = true;
                break;
            }
        }

        // Shared BOM list so the background thread and CallAfter can both access it
        auto sharedBom = std::make_shared<std::vector<BomRow>>( std::move( bomList ) );

        // Lambda that writes BOM CSV/JSON and finalises the generate step.
        // Called on the main thread (directly when no enrichment is needed, or
        // via CallAfter after background enrichment completes).
        auto finaliseBom = [this, sharedBom]()
        {
            int bomCount = 0;

            // Write CSV
            wxFile bomFile( m_bomCsvPath, wxFile::write );
            if( bomFile.IsOpened() )
            {
                bomFile.Write( wxT( "\"Designator\",\"Footprint\",\"Quantity\",\"Value\",\"MPN\",\"Manufacturer\",\"Description\"\n" ) );

                for( const BomRow& row : *sharedBom )
                {
                    wxString refs;
                    for( size_t i = 0; i < row.refs.size(); i++ )
                    {
                        if( i > 0 )
                            refs += wxT( ", " );
                        refs += row.refs[i];
                    }

                    wxString line;
                    line << wxT( "\"" ) << refs << wxT( "\",\"" )
                         << From_UTF8( row.fpid.GetLibItemName().c_str() ) << wxT( "\"," )
                         << row.count << wxT( ",\"" )
                         << row.val << wxT( "\",\"" )
                         << row.mpn << wxT( "\",\"" )
                         << row.manufacturer << wxT( "\",\"" )
                         << row.description << wxT( "\"\n" );
                    bomFile.Write( line );
                }
                bomFile.Close();
                bomCount = (int) sharedBom->size();
            }
            else
            {
                m_bomCsvPath.Clear();
            }

            // Write JSON
            nlohmann::json bomJson = nlohmann::json::array();
            for( const BomRow& row : *sharedBom )
            {
                wxString refs;
                for( size_t i = 0; i < row.refs.size(); i++ )
                {
                    if( i > 0 )
                        refs += wxT( ", " );
                    refs += row.refs[i];
                }

                bomJson.push_back( {
                    { "reference",    refs.ToStdString() },
                    { "quantity",     row.count },
                    { "value",        row.val.ToStdString() },
                    { "footprint",    std::string( row.fpid.GetLibItemName().c_str() ) },
                    { "mpn",          row.mpn.ToStdString() },
                    { "manufacturer", row.manufacturer.ToStdString() },
                    { "description",  row.description.ToStdString() },
                } );
            }

            wxFile jsonFile( m_bomJsonPath, wxFile::write );
            if( jsonFile.IsOpened() )
            {
                jsonFile.Write( wxString::FromUTF8( bomJson.dump( 2 ) ) );
                jsonFile.Close();
            }
            else
            {
                m_bomJsonPath.Clear();
            }

            if( bomCount > 0 )
                appendLog( wxString::Format( _( "BOM generated: %d unique parts" ), bomCount ) );
            else
                appendLog( _( "WARNING: No BOM items found." ) );

            appendLog( _( "=== Production files ready ===" ) );
            m_statusLabel->SetLabel(
                    _( "Production files generated. Click Send to submit to the manufacturer." ) );
            m_sendBtn->Enable( true );
            m_generateBtn->Enable( true );
            m_progressGauge->Hide();
            Layout();
        };

        // Call backend API to enrich BOM items missing MPN
        if( needsEnrichment )
        {
            m_statusLabel->SetLabel( _( "Enriching BOM with supplier data..." ) );
            m_progressGauge->Show();
            m_progressGauge->Pulse();
            Layout();
            wxYield();

            appendLog( _( "Calling backend to enrich BOM items missing MPN..." ) );

            wxString authToken = AUTH_MANAGER::Instance().GetAuthToken();

            if( !authToken.IsEmpty() )
            {
                appendLog( wxString::Format( _( "Auth token present (length: %zu)" ), authToken.length() ) );

                nlohmann::json bomItemsJson = nlohmann::json::array();
                for( const BomRow& row : *sharedBom )
                {
                    wxString refs;
                    for( size_t i = 0; i < row.refs.size(); i++ )
                    {
                        if( i > 0 )
                            refs += wxT( ", " );
                        refs += row.refs[i];
                    }

                    bomItemsJson.push_back( {
                        { "reference",    refs.ToStdString() },
                        { "quantity",     row.count },
                        { "value",        row.val.ToStdString() },
                        { "footprint",    std::string( row.fpid.GetLibItemName().c_str() ) },
                        { "mpn",          row.mpn.ToStdString() },
                        { "manufacturer", row.manufacturer.ToStdString() },
                        { "description",  row.description.ToStdString() },
                    } );
                }

                std::string requestBodyStr = nlohmann::json{ { "bom_items", bomItemsJson } }.dump();
                std::string apiUrl = GetTraceBackendUrl().ToStdString() + "/manufacturer/enrich-bom";
                std::string token = authToken.ToStdString();

                appendLog( wxString::Format( _( "Calling: %s" ), wxString::FromUTF8( apiUrl ) ) );

                std::thread enrichThread( [this, sharedBom, finaliseBom,
                                           apiUrl, requestBodyStr, token]()
                {
                    if( m_closing.load() )
                        return;

                    std::string responseBody;
                    bool        httpOk = false;
                    int         httpCode = 0;
                    std::string errorMsg;

                    try
                    {
                        KICAD_CURL_EASY curl;
                        curl.SetURL( apiUrl );
                        curl.SetPostFields( requestBodyStr );
                        curl.SetHeader( "Content-Type", "application/json" );
                        curl.SetHeader( "Authorization", std::string( "Bearer " ) + token );
                        curl_easy_setopt( curl.GetCurl(), CURLOPT_TIMEOUT, 60L );

                        int curlResult = curl.Perform();
                        httpCode = curl.GetResponseStatusCode();
                        responseBody = curl.GetBuffer();
                        httpOk = ( curlResult == CURLE_OK && httpCode == 200 );
                    }
                    catch( const std::exception& e )
                    {
                        errorMsg = e.what();
                    }

                    if( m_closing.load() )
                        return;

                    CallAfter( [this, sharedBom, finaliseBom,
                                httpOk, httpCode, responseBody, errorMsg]()
                    {
                        if( httpOk )
                        {
                            try
                            {
                                nlohmann::json response = nlohmann::json::parse( responseBody );

                                if( response.value( "success", false ) )
                                {
                                    int enrichedCount = response.value( "enriched_count", 0 );
                                    appendLog( wxString::Format( _( "Enriched %d items with MPN data" ),
                                                                 enrichedCount ) );

                                    auto enrichedItems = response["bom_items"];
                                    if( enrichedItems.is_array()
                                        && enrichedItems.size() == sharedBom->size() )
                                    {
                                        for( size_t i = 0; i < sharedBom->size(); i++ )
                                        {
                                            auto& item = enrichedItems[i];
                                            if( (*sharedBom)[i].mpn.IsEmpty()
                                                && item.contains( "mpn" ) )
                                            {
                                                std::string newMpn = item.value( "mpn", "" );
                                                if( !newMpn.empty() )
                                                {
                                                    (*sharedBom)[i].mpn =
                                                            wxString::FromUTF8( newMpn );
                                                    (*sharedBom)[i].manufacturer =
                                                            wxString::FromUTF8(
                                                                item.value( "manufacturer", "" ) );
                                                }
                                            }
                                        }
                                    }
                                }
                                else
                                {
                                    appendLog( wxString::Format( _( "Enrichment failed: %s" ),
                                            wxString::FromUTF8(
                                                response.value( "error", "Unknown error" ) ) ) );
                                }
                            }
                            catch( const std::exception& e )
                            {
                                appendLog( wxString::Format( _( "Enrichment parse error: %s" ),
                                        wxString::FromUTF8( e.what() ) ) );
                            }
                        }
                        else if( !errorMsg.empty() )
                        {
                            appendLog( wxString::Format( _( "Enrichment error: %s" ),
                                    wxString::FromUTF8( errorMsg ) ) );
                        }
                        else
                        {
                            appendLog( wxString::Format( _( "Enrichment API call failed (HTTP %d)" ),
                                    httpCode ) );
                            if( !responseBody.empty() )
                            {
                                appendLog( wxString::Format( _( "Response: %s" ),
                                        wxString::FromUTF8(
                                            responseBody.substr( 0, 500 ) ) ) );
                            }
                        }

                        m_statusLabel->SetLabel( _( "Writing BOM files..." ) );
                        finaliseBom();
                    } );
                } );
                enrichThread.detach();

                // Return here — finaliseBom will be called from CallAfter
                return;
            }
            else
            {
                appendLog( _( "Skipping enrichment: not authenticated" ) );
                m_progressGauge->Hide();
                Layout();
                wxYield();
            }
        }

        finaliseBom();
    }
}


void DIALOG_SEND_TO_MANUFACTURER::onSend( wxCommandEvent& aEvent )
{
    wxString mfrId = GetSelectedManufacturer();

    if( m_generatedZipPath.IsEmpty() || !wxFileExists( m_generatedZipPath ) )
    {
        wxMessageBox( _( "Please generate production files first." ),
                      _( "Send to Manufacturer" ), wxOK | wxICON_WARNING, this );
        return;
    }

    // Save shipping info for future use
    saveShippingSettings();

    // ---- Input validation (before disabling UI) ----
    if( mfrId == wxT( "pcbway" ) )
    {
        double lenVal = 0, widVal = 0;
        long qtyVal = 0;
        m_pcbwayLength->GetValue().ToDouble( &lenVal );
        m_pcbwayWidth->GetValue().ToDouble( &widVal );
        m_pcbwayQty->GetValue().ToLong( &qtyVal );

        if( lenVal <= 0 || widVal <= 0 )
        {
            wxMessageBox( _( "Board Length and Width must be greater than 0." ),
                          _( "Validation Error" ), wxOK | wxICON_WARNING, this );
            return;
        }
        if( qtyVal < 5 )
        {
            wxMessageBox( _( "Minimum order quantity is 5." ),
                          _( "Validation Error" ), wxOK | wxICON_WARNING, this );
            return;
        }

        if( m_pcbwayUVPrinting->GetSelection() != 0 && m_uvPrintFilePath.IsEmpty() )
        {
            wxMessageBox( _( "UV color printing is selected but no print image was provided.\n\n"
                             "Please select a high-resolution image file (PNG/PDF/AI) at 1:1 scale,\n"
                             "or set UV Printing to \"None\"." ),
                          _( "Validation Error" ), wxOK | wxICON_WARNING, this );
            return;
        }
    }

    m_statusLabel->SetLabel( wxString::Format( _( "Sending to %s..." ),
            m_mfrChoice->GetStringSelection() ) );
    m_sendBtn->Enable( false );
    m_progressGauge->Show();
    m_progressGauge->Pulse();
    Layout();

    appendLog( wxString::Format( _( "[send] Uploading %s to backend..." ), m_generatedZipPath ) );

    // ---- Collect order parameters from the form ----
    nlohmann::json params;

    if( mfrId == wxT( "pcbway" ) )
    {
        // String-valued params: send exact API strings from dropdown labels
        static const char* materialStr[] = { "FR-4", "Aluminum board", "Rogers", "HDI", "Copper" };
        int matSel = m_pcbwayMaterial->GetSelection();
        params["Material"] = ( matSel >= 0 && matSel < 5 ) ? materialStr[matSel] : "FR-4";

        static const char* boardTypeStr[] = { "Single PCB", "Panel by Customer", "Panel by Supplier" };
        int btSel = m_pcbwayBoardType->GetSelection();
        params["BoardType"] = ( btSel >= 0 && btSel < 3 ) ? boardTypeStr[btSel] : "Single PCB";

        static const char* fr4TgStr[] = { "TG130", "TG150", "TG170", "S1000H TG150", "S1000-2M TG170" };
        int tgSel = m_pcbwayFR4Tg->GetSelection();
        params["FR4Tg"] = ( tgSel >= 0 && tgSel < 5 ) ? fr4TgStr[tgSel] : "TG130";

        static const char* surfaceStr[] = {
            "HASL with lead", "HASL lead free", "Immersion gold", "OSP",
            "Hard Gold", "Immersion Silver", "Immersion Tin",
            "HASL lead free+Selective Immersion gold",
            "HASL lead free+Selective Hard gold",
            "Immersion gold+Selective Hard gold", "ENEPIG", "None"
        };
        int sfSel = m_pcbwaySurface->GetSelection();
        params["SurfaceFinish"] = ( sfSel >= 0 && sfSel < 12 ) ? surfaceStr[sfSel] : "HASL lead free";

        static const char* viaStr[] = { "Tenting vias", "Plugged vias", "Vias not covered" };
        int vpSel = m_pcbwayViaProcess->GetSelection();
        params["ViaProcess"] = ( vpSel >= 0 && vpSel < 3 ) ? viaStr[vpSel] : "Tenting vias";

        static const char* removeProdStr[] = { "No", "Yes", "Specify a location" };
        int rpSel = m_pcbwayRemoveProductNo->GetSelection();
        params["RemoveProductNo"] = ( rpSel >= 0 && rpSel < 3 ) ? removeProdStr[rpSel] : "No";

        // Copper weight: index 0="1 oz Cu", index 1="2 oz Cu", etc.
        int cwSel = m_pcbwayCopperWeight->GetSelection();
        params["FinishedCopper"] = std::string( std::to_string( cwSel + 1 ) + " oz Cu" );

        params["UVPrinting"]     = m_pcbwayUVPrinting->GetSelection();
        params["EdgeConnector"]  = m_pcbwayEdgeConnector->GetValue();

        // Goldfingers: same as EdgeConnector for now
        params["Goldfingers"] = m_pcbwayEdgeConnector->GetValue() ? "Yes" : "No";
        params["DesignInPanel"] = 1;
        params["SilkSides"] = 2;

        long layerVal = 2;
        m_pcbwayLayers->GetString( m_pcbwayLayers->GetSelection() ).ToLong( &layerVal );
        params["Layers"] = (int) layerVal;

        double lenVal = 100.0, widVal = 100.0;
        m_pcbwayLength->GetValue().ToDouble( &lenVal );
        m_pcbwayWidth->GetValue().ToDouble( &widVal );
        params["Length"] = lenVal;
        params["Width"]  = widVal;

        long qtyVal = 5;
        m_pcbwayQty->GetValue().ToLong( &qtyVal );
        params["Qty"] = (int) qtyVal;

        double thickVal = 1.6;
        m_pcbwayThickness->GetString( m_pcbwayThickness->GetSelection() ).ToDouble( &thickVal );
        params["Thickness"] = thickVal;

        params["SolderMask"]  = m_pcbwaySolderMask->GetStringSelection().ToStdString();
        params["Silkscreen"]  = m_pcbwaySilkscreen->GetStringSelection().ToStdString();
        params["MinTrackSpacing"] = m_pcbwayMinTrackSpacing->GetStringSelection().ToStdString();

        wxString holeSizeStr = m_pcbwayMinHoleSize->GetStringSelection();
        if( holeSizeStr == wxT( "No Drill" ) )
            params["MinHoleSize"] = -1;
        else
        {
            double holeVal = 0.3;
            holeSizeStr.ToDouble( &holeVal );
            params["MinHoleSize"] = holeVal;
        }

        // Shipping address
        if( m_pcbwayShipName && !m_pcbwayShipName->GetValue().IsEmpty() )
        {
            params["ShipName"]    = m_pcbwayShipName->GetValue().ToStdString();
            params["ShipAddress"] = m_pcbwayShipAddress->GetValue().ToStdString();
            params["ShipCity"]    = m_pcbwayShipCity->GetValue().ToStdString();
            params["ShipState"]   = m_pcbwayShipState->GetValue().ToStdString();
            params["ShipZip"]     = m_pcbwayShipZip->GetValue().ToStdString();
            params["ShipCountry"] = m_pcbwayShipCountry->GetValue().ToStdString();
            if( m_pcbwayShipEmail && !m_pcbwayShipEmail->GetValue().IsEmpty() )
                params["ShipEmail"] = m_pcbwayShipEmail->GetValue().ToStdString();
            if( m_pcbwayShipPhone && !m_pcbwayShipPhone->GetValue().IsEmpty() )
                params["ShipPhone"] = m_pcbwayShipPhone->GetValue().ToStdString();
        }

        // Order options (BuildDays and ShipType)
        if( m_pcbwayBuildDays )
        {
            // Map selection index to actual build days
            static const int buildDaysMap[] = { 3, 5, 7, 10 };
            int sel = m_pcbwayBuildDays->GetSelection();
            params["BuildDays"] = ( sel >= 0 && sel < 4 ) ? buildDaysMap[sel] : 5;
        }

        if( m_pcbwayShipType )
        {
            // Map selection index to PCBWay ShipType enum values
            static const int shipTypeMap[] = { 1, 10, 11, 6, 7, 9, 35 };  // DHL, FedEx IP, FedEx IE, EMS, ePacket, China Post, Global Standard
            int sel = m_pcbwayShipType->GetSelection();
            params["ShipType"] = ( sel >= 0 && sel < 7 ) ? shipTypeMap[sel] : 1;
        }

        // Assembly options
        if( m_pcbwayAssemblyEnabled && m_pcbwayAssemblyEnabled->GetValue() )
        {
            static const char* flexOptStr[] = { "Turnkey", "Kitted or Consigned", "Combo" };
            int foSel = m_pcbwayAssemblyType->GetSelection();
            params["FlexibleOption"] = ( foSel >= 0 && foSel < 3 ) ? flexOptStr[foSel] : "Turnkey";

            static const char* asmSideStr[] = { "Top side", "Bottom side", "Both sides" };
            int asSel = m_pcbwayAssemblySide->GetSelection();
            params["AssemblySide"] = ( asSel >= 0 && asSel < 3 ) ? asmSideStr[asSel] : "Top side";
            params["AssemblyQty"]  = m_pcbwayAssemblyQty->GetValue();
            params["ContainsSensitiveParts"] = m_pcbwayContainsSensitive->GetValue();
            params["AcceptAlternatives"] = m_pcbwayAcceptAlternatives->GetValue();
            if( !m_pcbwayAssemblyNotes->GetValue().IsEmpty() )
                params["AssemblyNotes"] = m_pcbwayAssemblyNotes->GetValue().ToStdString();
        }

        // Stencil options
        if( m_pcbwayStencilEnabled && m_pcbwayStencilEnabled->GetValue() )
        {
            params["StencilType"]      = m_pcbwayStencilType->GetSelection() + 1;
            params["MultiLevelStencil"] = m_pcbwayStencilMultiLevel->GetValue();
            params["StencilSide"]      = m_pcbwayStencilSide->GetSelection() + 1;
            params["StencilQty"]       = m_pcbwayStencilQty->GetValue();
            double stThick = 0.12;
            m_pcbwayStencilThickness->GetStringSelection().ToDouble( &stThick );
            params["StencilThickness"] = stThick;
            params["StencilFiducials"] = m_pcbwayStencilFiducials->GetSelection();
            params["Electropolishing"] = m_pcbwayStencilElectropol->GetValue();
        }
    }
    else if( mfrId == wxT( "pikkolo" ) )
    {
        // Pikkolo Assembly — shipping and assembly fields
        static const char* sourceValues[] = { "fabricate", "ship_from_fab", "ship_yourself" };
        int srcSel = m_pikkoloPcbSource->GetSelection();
        params["pcb_source"] = ( srcSel >= 0 && srcSel < 3 ) ? sourceValues[srcSel] : "fabricate";

        // Quantity
        if( m_pikkoloQty )
        {
            long qty = 5;
            m_pikkoloQty->GetValue().ToLong( &qty );
            params["qty_boards"] = (int) qty;
        }

        // Layers and board area (from auto-detected values)
        if( m_pikkoloLayers )
            params["fab_layers"] = wxAtoi( m_pikkoloLayers->GetLabel() );
        if( m_pikkoloBoardArea )
        {
            double area = 0;
            m_pikkoloBoardArea->GetLabel().ToDouble( &area );
            params["board_area_sq_in"] = area;
        }

        // Assembly options
        params["include_fab"] = m_pikkoloIncludeFab ? m_pikkoloIncludeFab->GetValue() : true;
        params["include_stencils"] = m_pikkoloShipStencil ? m_pikkoloShipStencil->GetValue() : false;
        params["double_sided_assembly"] = m_pikkoloDblSidedAsm ? m_pikkoloDblSidedAsm->GetValue() : false;

        // Notes (optional free text)
        if( m_pikkoloNotes && !m_pikkoloNotes->GetValue().IsEmpty() )
            params["notes"] = m_pikkoloNotes->GetValue().ToStdString();

        params["name"]           = m_pikkoloName->GetValue().ToStdString();
        params["company"]        = m_pikkoloCompany->GetValue().ToStdString();
        params["street_address"] = m_pikkoloAddress->GetValue().ToStdString();
        params["city"]           = m_pikkoloCity->GetValue().ToStdString();
        params["state"]          = m_pikkoloState->GetValue().ToStdString();
        params["zip_code"]       = m_pikkoloZip->GetValue().ToStdString();

        static const char* shippingValues[] = {
            "standard", "2day", "overnight", "local_pickup", "local_dropoff"
        };
        int shipSel = m_pikkoloShipping->GetSelection();
        params["shipping_method"] = ( shipSel >= 0 && shipSel < 5 )
                                    ? shippingValues[shipSel] : "standard";

        // Email for order updates
        if( m_pikkoloEmail && !m_pikkoloEmail->GetValue().IsEmpty() )
            params["email"] = m_pikkoloEmail->GetValue().ToStdString();
    }

    std::string paramsStr = params.dump();
    std::string manufacturer = mfrId.ToStdString();
    std::string zipPath = m_generatedZipPath.ToStdString();

    BOARD* board = m_frame->GetBoard();
    wxFileName boardFn( board ? board->GetFileName() : wxString() );
    std::string boardName = boardFn.GetName().ToStdString();

    std::string backendUrl = GetTraceBackendUrl().ToStdString();
    std::string url = backendUrl + "/manufacturer/submit";
    wxString authToken = AUTH_MANAGER::Instance().GetAuthToken();

    // Capture BOM and position file paths for the upload thread
    // Check file existence before entering thread (wxFileExists may not be thread-safe)
    std::string bomJsonPath;
    std::string positionFilePath;
    std::string uvPrintFilePath;
    std::vector<std::string> additionalFilePaths;
    
    if( !m_bomJsonPath.IsEmpty() && wxFileExists( m_bomJsonPath ) )
        bomJsonPath = m_bomJsonPath.ToStdString();
    
    if( !m_positionFilePath.IsEmpty() && wxFileExists( m_positionFilePath ) )
        positionFilePath = m_positionFilePath.ToStdString();

    if( !m_uvPrintFilePath.IsEmpty() && wxFileExists( m_uvPrintFilePath ) )
        uvPrintFilePath = m_uvPrintFilePath.ToStdString();

    for( const auto& p : m_additionalFiles )
    {
        if( !p.IsEmpty() && wxFileExists( p ) )
            additionalFilePaths.push_back( p.ToStdString() );
    }

    appendLog( wxString::Format( _( "[send] POST %s" ), wxString::FromUTF8( url ) ) );
    appendLog( wxString::Format( _( "[send] manufacturer=%s, board=%s" ),
                                 mfrId, boardFn.GetName() ) );
    if( !bomJsonPath.empty() )
        appendLog( wxString::Format( _( "[send] BOM file: %s" ), wxString::FromUTF8( bomJsonPath ) ) );
    if( !positionFilePath.empty() )
        appendLog( wxString::Format( _( "[send] Position file: %s" ), wxString::FromUTF8( positionFilePath ) ) );
    if( !uvPrintFilePath.empty() )
        appendLog( wxString::Format( _( "[send] UV print image: %s" ), wxString::FromUTF8( uvPrintFilePath ) ) );
    if( !additionalFilePaths.empty() )
        appendLog( wxString::Format( _( "[send] Additional files: %zu" ), additionalFilePaths.size() ) );

    std::thread t( [this, url, paramsStr, manufacturer, zipPath, boardName, authToken,
                     bomJsonPath, positionFilePath, uvPrintFilePath, additionalFilePaths]()
    {
        if( m_closing.load() ) return;
        nlohmann::json response;
        bool success = false;
        std::string errorDetail;

        try
        {
            KICAD_CURL_EASY curl;
            curl.SetURL( url );

            if( !authToken.IsEmpty() )
                curl.SetHeader( "Authorization",
                                std::string( "Bearer " ) + authToken.ToStdString() );

            // Build multipart MIME form
            CURL* handle = curl.GetCurl();
            curl_mime* mime = curl_mime_init( handle );

            // File part
            curl_mimepart* filePart = curl_mime_addpart( mime );
            curl_mime_name( filePart, "file" );
            curl_mime_filedata( filePart, zipPath.c_str() );
            curl_mime_type( filePart, "application/zip" );

            // Params part (JSON string)
            curl_mimepart* paramsPart = curl_mime_addpart( mime );
            curl_mime_name( paramsPart, "params" );
            curl_mime_data( paramsPart, paramsStr.c_str(), CURL_ZERO_TERMINATED );

            // Manufacturer part
            curl_mimepart* mfrPart = curl_mime_addpart( mime );
            curl_mime_name( mfrPart, "manufacturer" );
            curl_mime_data( mfrPart, manufacturer.c_str(), CURL_ZERO_TERMINATED );

            // Board name part
            curl_mimepart* namePart = curl_mime_addpart( mime );
            curl_mime_name( namePart, "board_name" );
            curl_mime_data( namePart, boardName.c_str(), CURL_ZERO_TERMINATED );

            // BOM file part (if available)
            if( !bomJsonPath.empty() )
            {
                curl_mimepart* bomPart = curl_mime_addpart( mime );
                curl_mime_name( bomPart, "bom_file" );
                curl_mime_filedata( bomPart, bomJsonPath.c_str() );
                curl_mime_type( bomPart, "application/json" );
            }

            // Position/PnP file part (if available)
            if( !positionFilePath.empty() )
            {
                curl_mimepart* pnpPart = curl_mime_addpart( mime );
                curl_mime_name( pnpPart, "pnp_file" );
                curl_mime_filedata( pnpPart, positionFilePath.c_str() );
                curl_mime_type( pnpPart, "text/csv" );
            }

            // UV color print image (if provided)
            if( !uvPrintFilePath.empty() )
            {
                curl_mimepart* uvPart = curl_mime_addpart( mime );
                curl_mime_name( uvPart, "uv_print_file" );
                curl_mime_filedata( uvPart, uvPrintFilePath.c_str() );
            }

            // Additional files (if any)
            for( size_t i = 0; i < additionalFilePaths.size(); i++ )
            {
                curl_mimepart* addPart = curl_mime_addpart( mime );
                std::string fieldName = "additional_file_" + std::to_string( i );
                curl_mime_name( addPart, fieldName.c_str() );
                curl_mime_filedata( addPart, additionalFilePaths[i].c_str() );
            }

            curl_easy_setopt( handle, CURLOPT_MIMEPOST, mime );
            curl_easy_setopt( handle, CURLOPT_TIMEOUT, 120L );

            int curlResult = curl.Perform();

            if( curlResult != CURLE_OK )
            {
                errorDetail = "CURL error " + std::to_string( curlResult )
                              + ": " + curl.GetErrorText( curlResult );
            }
            else
            {
                int httpCode = curl.GetResponseStatusCode();
                std::string rawBody = curl.GetBuffer();

                if( httpCode != 200 )
                {
                    errorDetail = "HTTP " + std::to_string( httpCode );
                    if( rawBody.size() < 500 )
                        errorDetail += ": " + rawBody;
                }
                else
                {
                    response = nlohmann::json::parse( rawBody, nullptr, false );

                    if( response.is_discarded() )
                    {
                        errorDetail = "Invalid JSON in response";
                    }
                    else if( !response.value( "success", false ) )
                    {
                        errorDetail = "Backend returned success=false";
                        if( response.contains( "error" ) )
                            errorDetail += ": " + response["error"].get<std::string>();
                    }
                    else
                    {
                        success = true;
                    }
                }
            }

            curl_mime_free( mime );
        }
        catch( const std::exception& ex )
        {
            errorDetail = std::string( "Exception: " ) + ex.what();
        }
        catch( ... )
        {
            errorDetail = "Unknown exception during upload";
        }

        CallAfter( [this, success, response, errorDetail]()
        {
            m_sendBtn->Enable( true );
            m_progressGauge->Hide();
            Layout();

            if( !success )
            {
                appendLog( wxString::Format( _( "[send] ERROR: %s" ),
                                             wxString::FromUTF8( errorDetail ) ) );
                m_statusLabel->SetLabel( _( "Upload failed." ) );

                wxMessageBox(
                    wxString::Format(
                        _( "Failed to submit order to manufacturer.\n\n%s" ),
                        wxString::FromUTF8( errorDetail ) ),
                    _( "Send to Manufacturer" ),
                    wxOK | wxICON_ERROR, this );
                Layout();
                return;
            }

            std::string orderId = response.value( "order_id", "" );
            std::string status  = response.value( "status", "pending_review" );
            std::string paymentUrl = response.value( "payment_url", "" );

            appendLog( wxString::Format( _( "[send] Order submitted! ID: %s, Status: %s" ),
                                         wxString::FromUTF8( orderId ),
                                         wxString::FromUTF8( status ) ) );

            wxString quoteInfo;
            if( response.contains( "quote" ) && !response["quote"].is_null() )
            {
                auto q = response["quote"];
                if( q.contains( "TotalPrice" ) )
                    quoteInfo = wxString::Format( _( "\nEstimated price: $%s" ),
                            wxString::FromUTF8( q["TotalPrice"].dump() ) );
                if( q.contains( "LeadTime" ) )
                    quoteInfo += wxString::Format( _( "\nLead time: %s days" ),
                            wxString::FromUTF8( q["LeadTime"].dump() ) );
            }

            m_statusLabel->SetLabel( _( "Order submitted successfully!" ) );
            Layout();

            // Check if payment is required (awaiting_payment status with payment URL)
            if( status == "awaiting_payment" && !paymentUrl.empty() )
            {
                appendLog( wxString::Format( _( "[send] Payment required. URL: %s" ),
                                             wxString::FromUTF8( paymentUrl ) ) );

                wxDialog payDlg( this, wxID_ANY, _( "Payment Required" ),
                                 wxDefaultPosition, wxSize( 440, 280 ),
                                 wxDEFAULT_DIALOG_STYLE );
                wxBoxSizer* dlgSizer = new wxBoxSizer( wxVERTICAL );

                wxString msg = wxString::Format(
                    _( "Your order has been submitted and is awaiting payment.\n\n"
                       "Order ID: %s%s" ),
                    wxString::FromUTF8( orderId ), quoteInfo );
                dlgSizer->Add( new wxStaticText( &payDlg, wxID_ANY, msg ),
                               0, wxALL | wxEXPAND, 16 );

                wxBoxSizer* btnRow = new wxBoxSizer( wxHORIZONTAL );
                wxButton* payBtn = new wxButton( &payDlg, wxID_ANY, _( "Open Payment Page" ) );
                wxButton* dashBtn = new wxButton( &payDlg, wxID_ANY, _( "View on Dashboard" ) );
                wxButton* closeBtn = new wxButton( &payDlg, wxID_CANCEL, _( "Close" ) );
                btnRow->Add( payBtn, 0, wxRIGHT, 8 );
                btnRow->Add( dashBtn, 0, wxRIGHT, 8 );
                btnRow->AddStretchSpacer();
                btnRow->Add( closeBtn, 0 );
                dlgSizer->Add( btnRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 16 );

                std::string payUrl = paymentUrl;
                std::string oid = orderId;
                payBtn->Bind( wxEVT_BUTTON, [payUrl]( wxCommandEvent& ) {
                    wxLaunchDefaultBrowser( wxString::FromUTF8( payUrl ) );
                } );
                dashBtn->Bind( wxEVT_BUTTON, [oid]( wxCommandEvent& ) {
                    wxLaunchDefaultBrowser( wxString::Format(
                        wxT( "https://buildwithtrace.com/dashboard/orders/%s" ),
                        wxString::FromUTF8( oid ) ) );
                } );

                payDlg.SetSizer( dlgSizer );
                payDlg.Layout();
                payDlg.Centre();
                payDlg.ShowModal();
            }
            else
            {
                wxDialog doneDlg( this, wxID_ANY, _( "Order Submitted" ),
                                  wxDefaultPosition, wxSize( 440, 260 ),
                                  wxDEFAULT_DIALOG_STYLE );
                wxBoxSizer* dlgSizer2 = new wxBoxSizer( wxVERTICAL );

                wxString msg2 = wxString::Format(
                    _( "Your order has been submitted to the manufacturer.\n\n"
                       "Order ID: %s\n"
                       "Status: %s%s" ),
                    wxString::FromUTF8( orderId ),
                    wxString::FromUTF8( status ),
                    quoteInfo );
                dlgSizer2->Add( new wxStaticText( &doneDlg, wxID_ANY, msg2 ),
                                0, wxALL | wxEXPAND, 16 );

                wxBoxSizer* btnRow2 = new wxBoxSizer( wxHORIZONTAL );
                wxButton* dashBtn2 = new wxButton( &doneDlg, wxID_ANY,
                                                   _( "View on Dashboard" ) );
                wxButton* closeBtn2 = new wxButton( &doneDlg, wxID_CANCEL, _( "Close" ) );
                btnRow2->Add( dashBtn2, 0, wxRIGHT, 8 );
                btnRow2->AddStretchSpacer();
                btnRow2->Add( closeBtn2, 0 );
                dlgSizer2->Add( btnRow2, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 16 );

                std::string oid2 = orderId;
                dashBtn2->Bind( wxEVT_BUTTON, [oid2]( wxCommandEvent& ) {
                    wxLaunchDefaultBrowser( wxString::Format(
                        wxT( "https://buildwithtrace.com/dashboard/orders/%s" ),
                        wxString::FromUTF8( oid2 ) ) );
                } );

                doneDlg.SetSizer( dlgSizer2 );
                doneDlg.Layout();
                doneDlg.Centre();
                doneDlg.ShowModal();
            }
        } );
    } );

    {
        std::lock_guard<std::mutex> lock( m_threadMutex );
        if( m_sendThread.joinable() )
            m_sendThread.join();
        m_sendThread = std::move( t );
    }
}
