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

#include <ai_commit.h>
#include <commit.h>
#include <eda_item.h>
#include <wx/log.h>

#ifndef APPEND_UNDO
#define APPEND_UNDO 0x0002
#endif

#ifndef SKIP_CONNECTIVITY
#define SKIP_CONNECTIVITY 0x0008
#endif


AI_COMMIT::AI_COMMIT( bool aIsBoardEditor )
    : m_isBoardEditor( aIsBoardEditor ),
      m_sessionActive( false ),
      m_pushCount( 0 )
{
}


void AI_COMMIT::SetCommit( std::unique_ptr<COMMIT> aCommit )
{
    m_commit = std::move( aCommit );
}


AI_COMMIT::~AI_COMMIT()
{
    if( m_sessionActive )
    {
        wxLogWarning( wxT( "AI_COMMIT: Destroying active session - reverting" ) );
        Revert();
    }
}


void AI_COMMIT::createCommit()
{
}


void AI_COMMIT::BeginSession( const wxString& aDescription )
{
    if( m_sessionActive )
    {
        wxLogWarning( wxT( "AI_COMMIT: BeginSession called with active session - ending previous" ) );
        EndSession();
    }

    m_description = aDescription;
    m_sessionActive = true;
    m_pushCount = 0;

    if( !m_commit )
    {
        wxLogError( wxT( "AI_COMMIT: BeginSession called but no commit object set. "
                         "Call SetCommit() before BeginSession()." ) );
    }

    if( m_undoBlocker )
        m_undoBlocker( true );

    wxLogDebug( wxT( "AI_COMMIT: Session started: %s" ), aDescription );
}


void AI_COMMIT::ApplyOps( const std::vector<AI_EDIT_OP>& aOps )
{
    if( !m_sessionActive )
    {
        wxLogError( wxT( "AI_COMMIT: ApplyOps called without active session" ) );
        return;
    }

    if( aOps.empty() )
        return;

    for( const auto& op : aOps )
    {
        if( m_isBoardEditor )
            applyBoardOp( op );
        else
            applySchematicOp( op );
    }
}


void AI_COMMIT::applySchematicOp( const AI_EDIT_OP& aOp )
{
    if( !m_commit )
    {
        wxLogError( wxT( "AI_COMMIT: No commit object set for schematic ops" ) );
        return;
    }

    BASE_SCREEN* screen = m_screenProvider ? m_screenProvider() : nullptr;

    switch( aOp.type )
    {
    case AI_EDIT_OP_TYPE::ADD:
    {
        if( !m_schItemParser )
        {
            wxLogError( wxT( "AI_COMMIT: No schematic item parser set" ) );
            return;
        }

        EDA_ITEM* newItem = m_schItemParser( aOp.newSexp, screen );
        if( newItem )
        {
            m_commit->Add( newItem, screen );
            wxLogDebug( wxT( "AI_COMMIT: ADD item uuid=%s" ), aOp.itemUuid.AsString() );
        }
        else
        {
            wxLogWarning( wxT( "AI_COMMIT: Failed to parse ADD item uuid=%s" ),
                         aOp.itemUuid.AsString() );
        }
        break;
    }

    case AI_EDIT_OP_TYPE::REMOVE:
    {
        if( !m_schItemResolver )
        {
            wxLogError( wxT( "AI_COMMIT: No schematic item resolver set" ) );
            return;
        }

        EDA_ITEM* item = m_schItemResolver( aOp.itemUuid );
        if( item )
        {
            m_commit->Remove( item, screen );
            wxLogDebug( wxT( "AI_COMMIT: REMOVE item uuid=%s" ), aOp.itemUuid.AsString() );
        }
        else
        {
            wxLogDebug( wxT( "AI_COMMIT: Item not found for REMOVE uuid=%s (may already be absent)" ),
                       aOp.itemUuid.AsString() );
        }
        break;
    }

    case AI_EDIT_OP_TYPE::MODIFY:
    {
        if( !m_schItemResolver || !m_schItemParser )
        {
            wxLogError( wxT( "AI_COMMIT: Missing resolver or parser for MODIFY" ) );
            return;
        }

        EDA_ITEM* liveItem = m_schItemResolver( aOp.itemUuid );
        if( !liveItem )
        {
            wxLogWarning( wxT( "AI_COMMIT: Item not found for MODIFY uuid=%s" ),
                         aOp.itemUuid.AsString() );
            return;
        }

        EDA_ITEM* newItem = m_schItemParser( aOp.newSexp, screen );
        if( !newItem )
        {
            wxLogWarning( wxT( "AI_COMMIT: Failed to parse MODIFY item uuid=%s" ),
                         aOp.itemUuid.AsString() );
            return;
        }

        m_commit->Modify( liveItem, screen );

        if( m_itemSwapper )
        {
            m_itemSwapper( liveItem, newItem );
        }
        else
            delete newItem;

        wxLogDebug( wxT( "AI_COMMIT: MODIFY item uuid=%s" ), aOp.itemUuid.AsString() );
        break;
    }
    }
}


void AI_COMMIT::applyBoardOp( const AI_EDIT_OP& aOp )
{
    if( !m_commit )
    {
        wxLogError( wxT( "AI_COMMIT: No commit object set for board ops" ) );
        return;
    }

    switch( aOp.type )
    {
    case AI_EDIT_OP_TYPE::ADD:
    {
        if( !m_boardItemParser || !m_boardProvider )
        {
            wxLogError( wxT( "AI_COMMIT: No board item parser or board provider set" ) );
            return;
        }

        EDA_ITEM* newItem = m_boardItemParser( aOp.newSexp, m_boardProvider() );
        if( newItem )
        {
            m_commit->Add( newItem );
            wxLogDebug( wxT( "AI_COMMIT: ADD board item uuid=%s" ), aOp.itemUuid.AsString() );
        }
        else
        {
            wxLogWarning( wxT( "AI_COMMIT: Failed to parse ADD board item uuid=%s" ),
                         aOp.itemUuid.AsString() );
        }
        break;
    }

    case AI_EDIT_OP_TYPE::REMOVE:
    {
        if( !m_boardItemResolver )
        {
            wxLogError( wxT( "AI_COMMIT: No board item resolver set" ) );
            return;
        }

        EDA_ITEM* item = m_boardItemResolver( aOp.itemUuid );
        if( item )
        {
            m_commit->Remove( item );
            wxLogDebug( wxT( "AI_COMMIT: REMOVE board item uuid=%s" ), aOp.itemUuid.AsString() );
        }
        else
        {
            wxLogWarning( wxT( "AI_COMMIT: Board item not found for REMOVE uuid=%s" ),
                         aOp.itemUuid.AsString() );
        }
        break;
    }

    case AI_EDIT_OP_TYPE::MODIFY:
    {
        if( !m_boardItemResolver || !m_boardItemParser || !m_boardProvider )
        {
            wxLogError( wxT( "AI_COMMIT: Missing resolver/parser/board for MODIFY" ) );
            return;
        }

        EDA_ITEM* liveItem = m_boardItemResolver( aOp.itemUuid );
        if( !liveItem )
        {
            wxLogWarning( wxT( "AI_COMMIT: Board item not found for MODIFY uuid=%s" ),
                         aOp.itemUuid.AsString() );
            return;
        }

        // Parse the new item BEFORE staging the modify so we don't stage a change
        // we can't complete (Unstage is not available in kicommon).
        EDA_ITEM* newItem = m_boardItemParser( aOp.newSexp, m_boardProvider() );
        if( !newItem )
        {
            wxLogWarning( wxT( "AI_COMMIT: Failed to parse MODIFY board item uuid=%s" ),
                         aOp.itemUuid.AsString() );
            return;
        }

        m_commit->Modify( liveItem );

        if( m_itemSwapper )
            m_itemSwapper( liveItem, newItem );
        else
            delete newItem;

        wxLogDebug( wxT( "AI_COMMIT: MODIFY board item uuid=%s" ), aOp.itemUuid.AsString() );
        break;
    }
    }
}


void AI_COMMIT::EndSession()
{
    if( !m_sessionActive )
        return;

    if( m_commit && !m_commit->Empty() )
    {
        int flags = ( m_pushCount > 0 ) ? APPEND_UNDO : 0;
        flags |= SKIP_CONNECTIVITY;
        m_commit->Push( m_description, flags );
        m_pushCount++;
        wxLogDebug( wxT( "AI_COMMIT: Pushed commit #%d: %s" ), m_pushCount, m_description );
    }

    m_sessionActive = false;
    m_commit.reset();

    if( m_undoBlocker )
        m_undoBlocker( false );

    wxLogDebug( wxT( "AI_COMMIT: Session ended (%d push(es))" ), m_pushCount );
}


void AI_COMMIT::Revert()
{
    if( !m_sessionActive )
        return;

    if( m_commit && !m_commit->Empty() )
    {
        m_commit->Revert();
        wxLogDebug( wxT( "AI_COMMIT: Session reverted" ) );
    }

    m_sessionActive = false;
    m_commit.reset();

    if( m_undoBlocker )
        m_undoBlocker( false );
}
