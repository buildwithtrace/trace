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

#ifndef AI_COMMIT_H
#define AI_COMMIT_H

#include <kicommon.h>
#include <ai_edit_translator.h>
#include <wx/string.h>

#include <memory>
#include <string>
#include <vector>
#include <functional>

class COMMIT;
class EDA_ITEM;
class BASE_SCREEN;
class BOARD;

/**
 * Manages an AI edit session, translating AI_EDIT_OPs into native KiCad COMMIT
 * operations so that AI edits participate in the standard undo/redo system.
 *
 * Usage:
 *   1. Call BeginSession() when the AI streaming response starts
 *   2. Call ApplyOps() each time a batch of translated operations is available
 *   3. Call EndSession() when the AI response is complete to finalize the undo entry
 *   4. Call Revert() if the AI stream is cancelled mid-session
 *
 * All operations within a session are merged into a single undo entry using
 * APPEND_UNDO, so Ctrl+Z undoes the entire AI response at once.
 */
class KICOMMON_API AI_COMMIT
{
public:
    /**
     * @param aIsBoardEditor True for PCBnew, false for eeschema.
     */
    explicit AI_COMMIT( bool aIsBoardEditor );

    ~AI_COMMIT();

    /**
     * Begin a new AI edit session.
     * Must be called before any ApplyOps() calls.
     * Blocks undo/redo until EndSession() is called.
     *
     * @param aDescription Label for the undo entry (e.g., "AI Edit").
     */
    void BeginSession( const wxString& aDescription );

    /**
     * Apply a batch of translated operations to the live document.
     *
     * For each operation:
     *   - ADD: Parses the s-expression to create a new item, adds it to the document,
     *          and stages it in the commit.
     *   - REMOVE: Finds the live item by UUID and stages it for removal.
     *   - MODIFY: Finds the live item by UUID, takes a snapshot (for undo), then
     *             replaces its data with the parsed s-expression content.
     *
     * Can be called multiple times per session (e.g., once per batch timer tick).
     * Each call accumulates changes into the same commit.
     *
     * @param aOps The operations to apply.
     */
    void ApplyOps( const std::vector<AI_EDIT_OP>& aOps );

    /**
     * Finalize the session: push all accumulated changes as a single undo entry.
     * Unblocks undo/redo.
     */
    void EndSession();

    /**
     * Revert all changes made during this session.
     * Called if the AI stream is cancelled or fails.
     * Unblocks undo/redo.
     */
    void Revert();

    /**
     * @return True if a session is currently active (between BeginSession and EndSession/Revert).
     */
    bool IsSessionActive() const { return m_sessionActive; }

    /**
     * @return True if any operations have been applied in this session.
     */
    bool HasChanges() const { return m_pushCount > 0; }

    /**
     * Set the underlying COMMIT object (BOARD_COMMIT or SCH_COMMIT).
     * Must be called before ApplyOps(). The AI_COMMIT takes ownership.
     * Defined out-of-line because unique_ptr needs the complete COMMIT type for deletion.
     */
    void SetCommit( std::unique_ptr<COMMIT> aCommit );

    /**
     * Set callback for resolving a schematic item by UUID.
     * Required for eeschema. The callback should find the live item and return it as EDA_ITEM*.
     */
    void SetSchItemResolver( std::function<EDA_ITEM*( const KIID& )> aResolver )
    {
        m_schItemResolver = std::move( aResolver );
    }

    /**
     * Set callback for resolving a board item by UUID.
     * Required for pcbnew. The callback should find the live item and return it as EDA_ITEM*.
     */
    void SetBoardItemResolver( std::function<EDA_ITEM*( const KIID& )> aResolver )
    {
        m_boardItemResolver = std::move( aResolver );
    }

    /**
     * Set callback for parsing an s-expression string into a new schematic item.
     * Required for eeschema ADD/MODIFY operations.
     * The BASE_SCREEN* is passed for item parenting. Returns EDA_ITEM* to avoid
     * cross-library type dependencies (the callback does the actual SCH_ITEM cast).
     */
    void SetSchItemParser( std::function<EDA_ITEM*( const std::string&, BASE_SCREEN* )> aParser )
    {
        m_schItemParser = std::move( aParser );
    }

    /**
     * Set callback for parsing an s-expression string into a new board item.
     * Required for pcbnew ADD/MODIFY operations. Returns EDA_ITEM* to avoid
     * cross-library type dependencies.
     */
    void SetBoardItemParser( std::function<EDA_ITEM*( const std::string&, BOARD* )> aParser )
    {
        m_boardItemParser = std::move( aParser );
    }

    /**
     * Set callback to block/unblock undo during an active session.
     * On PCBnew this calls SetUndoRedoBlocked(); on eeschema we add the same mechanism.
     */
    void SetUndoBlocker( std::function<void( bool )> aBlocker )
    {
        m_undoBlocker = std::move( aBlocker );
    }

    /**
     * Set callback to get the current screen (for eeschema item parenting).
     * Returns BASE_SCREEN* to avoid dependency on eeschema types.
     */
    void SetScreenProvider( std::function<BASE_SCREEN*()> aProvider )
    {
        m_screenProvider = std::move( aProvider );
    }

    /**
     * Set callback to get the current board (for pcbnew item parenting).
     */
    void SetBoardProvider( std::function<BOARD*()> aProvider )
    {
        m_boardProvider = std::move( aProvider );
    }

    /**
     * Set callback for swapping item data (MODIFY operations).
     * The callback receives (liveItem, newParsedItem) and should call
     * liveItem->SwapItemData(newItem) then delete newItem.
     * Required because SwapItemData is defined in eeschema/pcbnew, not common.
     */
    void SetItemSwapper( std::function<void( EDA_ITEM*, EDA_ITEM* )> aSwapper )
    {
        m_itemSwapper = std::move( aSwapper );
    }

private:
    void createCommit();
    void applySchematicOp( const AI_EDIT_OP& aOp );
    void applyBoardOp( const AI_EDIT_OP& aOp );

    bool                 m_isBoardEditor;
    bool                 m_sessionActive;
    int                  m_pushCount;   ///< Number of Push() calls made this session
    wxString             m_description;
    std::unique_ptr<COMMIT> m_commit;   ///< The underlying BOARD_COMMIT or SCH_COMMIT

    // Callbacks set by the editor-specific chat panel.
    // All item pointers are typed as EDA_ITEM* to avoid depending on eeschema/pcbnew
    // headers from common/. The callback providers cast to the concrete types internally.
    std::function<EDA_ITEM*( const KIID& )>                      m_schItemResolver;
    std::function<EDA_ITEM*( const KIID& )>                     m_boardItemResolver;
    std::function<EDA_ITEM*( const std::string&, BASE_SCREEN* )> m_schItemParser;
    std::function<EDA_ITEM*( const std::string&, BOARD* )>      m_boardItemParser;
    std::function<void( bool )>                                 m_undoBlocker;
    std::function<BASE_SCREEN*()>                               m_screenProvider;
    std::function<BOARD*()>                                     m_boardProvider;
    std::function<void( EDA_ITEM*, EDA_ITEM* )>                 m_itemSwapper;
};

#endif // AI_COMMIT_H
