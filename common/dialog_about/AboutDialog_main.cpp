/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2010 Rafael Sokolowski <Rafael.Sokolowski@web.de>
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

#include <boost/version.hpp>
#include <wx/aboutdlg.h>
#include <wx/arrimpl.cpp>
#include <wx/textctrl.h>
#include <wx/utils.h>

/* Used icons:
 *  show_3d_xpm;      // 3D icon
 *  module_xpm;
 *  icon_kicad_xpm;   // Icon of the application
 */
#include <bitmaps.h>
#include <build_version.h>
#include <common.h>
#include <kiplatform/app.h>
#include <pgm_base.h>
#include <eda_base_frame.h>

#include "aboutinfo.h"
#include "dialog_about.h"


WX_DEFINE_OBJARRAY( CONTRIBUTORS )

// Helper functions:
static wxString HtmlHyperlink( const wxString& url, const wxString& description = wxEmptyString );
static wxString HtmlNewline( const unsigned int amount = 1 );


/**
 * Initializes the <code>ABOUT_APP_INFO</code> object with application specific information.
 * This is the object which holds all information about the application
 */
static void buildKicadAboutBanner( EDA_BASE_FRAME* aParent, ABOUT_APP_INFO& aInfo )
{
    // Set application specific icon
    aInfo.SetAppIcon( aParent->GetIcon() );

    /* Set title */
    aInfo.SetAppName( Pgm().App().GetAppName() );

    /* Trace build version */
    wxString version;
    version << ( KIPLATFORM::APP::IsOperatingSystemUnsupported() ? wxString( wxS( "(UNSUPPORTED)" ) )
                                                                 : GetTraceBuildVersion() )
#ifdef DEBUG
            << wxT( ", debug" )
#else
            << wxT( ", release" )
#endif
            << wxT( " build" );

    aInfo.SetBuildVersion( version );
    aInfo.SetBuildDate( GetBuildDate() );

    /* wxWidgets version */
    wxString libVersion;
    libVersion << wxGetLibraryVersionInfo().GetVersionString();

    /* Unicode or ANSI version */
#if wxUSE_UNICODE
    libVersion << wxT( " Unicode " );
#else
    libVersion << wxT( " ANSI " );
#endif

    // Just in case someone builds with the platform native of Boost instead of
    // the version included with the source.
    libVersion << wxT( "and Boost " ) << ( BOOST_VERSION / 100000 ) << wxT( "." )
               << ( BOOST_VERSION / 100 % 1000 ) << wxT( "." ) << ( BOOST_VERSION % 100 )
               << wxT( "\n" );

    // Operating System Information

    wxPlatformInfo platformInfo;

    libVersion << wxT( "Platform: " ) << wxGetOsDescription() << wxT( ", " )
               << GetPlatformGetBitnessName();

    aInfo.SetLibVersion( libVersion );

    // info/description part HTML formatted:
    wxString description;

    /* short description */
    description << wxT( "<p>" );
    description << wxT( "<b><u>" )
                << _( "Description" )
                << wxT( "</u></b>" ); // bold & underlined font for caption

    description << wxT( "<p>" )
                << _( "Trace is a free and open-source electronics design automation (EDA) suite, "
                      "forked from KiCad. It features schematic capture, integrated circuit simulation, "
                      "printed circuit board (PCB) layout, 3D rendering, and plotting/data export to "
                      "numerous formats." )
                << wxT( "</p>" );

    description << wxT( "<p>" )
                << _( "Trace version: " ) << GetTraceBuildVersion() << wxT( "<br>" )
                << _( "Based on KiCad version: " ) << GetBuildVersion()
                << wxT( "</p>" );

    description << wxT( "<p style='font-size: small;'>" )
                << _( "Trace is an independent product not affiliated with or endorsed by the KiCad project. "
                      "KiCad is a registered trademark of The Linux Foundation." )
                << wxT( "</p>" );

    aInfo.SetDescription( description );


    // License information also HTML formatted:
    wxString license;
    license
        << wxT( "<div align='center'>" )
        << HtmlNewline( 4 )
        << _( "Trace is released under the" ) << HtmlNewline( 2 )
        << HtmlHyperlink( wxS( "http://www.gnu.org/licenses" ),
                          _( "GNU General Public License (GPL) version 3 or any later version" ) )
        << wxT( "</div>" );

    aInfo.SetLicense( license );
}


void ShowAboutDialog( EDA_BASE_FRAME* aParent )
{
    ABOUT_APP_INFO info;
    buildKicadAboutBanner( aParent, info );

    DIALOG_ABOUT dlg( aParent, info );
    dlg.ShowModal();
}


///////////////////////////////////////////////////////////////////////////////
/// Helper functions
///////////////////////////////////////////////////////////////////////////////

/**
 * Wrap \a aUrl with a HTML anchor tag containing a hyperlink text reference
 * to form a HTML hyperlink.
 *
 * @param aUrl the url that will be embedded in an anchor tag containing a hyperlink reference
 * @param aDescription the optional describing text that will be represented as a hyperlink.
 *  If not specified the url will be used as hyperlink.
 * @return a HTML conform hyperlink like <a href='url'>description</a>
 */
static wxString HtmlHyperlink( const wxString& aUrl, const wxString& aDescription )
{
    wxString hyperlink = wxEmptyString;

    if( aDescription.IsEmpty() )
        hyperlink << wxS( "<a href='" ) << aUrl << wxS( "'>" ) << aUrl << wxS( "</a>" );
    else
        hyperlink << wxS( "<a href='" )<< aUrl << wxS( "'>" ) << aDescription << wxS( "</a>" );

    return hyperlink;
}


/**
 * Create an HTML newline character sequence of \a aCount.
 *
 * @param aCount the number of HTML newline tags to concatenate, default is to return just
 *               one <br> tag.
 * @return the concatenated amount of HTML newline tag(s) <br>
 */
static wxString HtmlNewline( const unsigned int aCount )
{
    wxString newlineTags = wxEmptyString;

    for( size_t i = 0; i<aCount; ++i )
        newlineTags << wxS( "<br>" );

    return newlineTags;
}
