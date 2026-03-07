import { useRef, useEffect, useState } from 'react'
import type { TabInfo, ConversationInfo } from '../vite-env.d'

interface TabBarProps {
  tabs: TabInfo[]
  activeTabId: string | null
  onTabSelect: (tabId: string) => void
  onTabClose: (tabId: string) => void
  onNewTab: () => void
  onHistoryClick: () => void
  historyMenuOpen: boolean
  historyConversations: ConversationInfo[]
  onHistorySelect: (conversationId: string) => void
}

export default function TabBar({
  tabs,
  activeTabId,
  onTabSelect,
  onTabClose,
  onNewTab,
  onHistoryClick,
  historyMenuOpen,
  historyConversations,
  onHistorySelect,
}: TabBarProps) {
  const tabsContainerRef = useRef<HTMLDivElement>(null)
  const historyButtonRef = useRef<HTMLButtonElement>(null)
  const searchInputRef = useRef<HTMLInputElement>(null)
  const [menuPosition, setMenuPosition] = useState<{ top: number; right: number } | null>(null)
  const [searchQuery, setSearchQuery] = useState('')

  // Scroll active tab into view
  useEffect(() => {
    if (activeTabId && tabsContainerRef.current) {
      const activeTab = tabsContainerRef.current.querySelector(`[data-tab-id="${activeTabId}"]`)
      if (activeTab) {
        activeTab.scrollIntoView({ behavior: 'smooth', block: 'nearest', inline: 'nearest' })
      }
    }
  }, [activeTabId])

  // Calculate menu position when opened
  useEffect(() => {
    if (historyMenuOpen && historyButtonRef.current) {
      const rect = historyButtonRef.current.getBoundingClientRect()
      setMenuPosition({
        top: rect.bottom + 2,
        right: window.innerWidth - rect.right,
      })
      // Focus search input when menu opens
      setTimeout(() => searchInputRef.current?.focus(), 50)
    } else {
      // Clear search when menu closes
      setSearchQuery('')
    }
  }, [historyMenuOpen])

  // Filter conversations based on search query
  const filteredConversations = historyConversations.filter(conv =>
    conv.title.toLowerCase().includes(searchQuery.toLowerCase())
  )

  // Handle mouse wheel for horizontal scrolling
  const handleWheel = (e: React.WheelEvent) => {
    if (tabsContainerRef.current) {
      e.preventDefault()
      tabsContainerRef.current.scrollLeft += e.deltaY * 0.5
    }
  }

  const handleTabClick = (tabId: string, e: React.MouseEvent) => {
    e.stopPropagation()
    onTabSelect(tabId)
  }

  const handleCloseClick = (tabId: string, e: React.MouseEvent) => {
    e.stopPropagation()
    onTabClose(tabId)
  }

  // Middle-click to close tab
  const handleTabMouseDown = (tabId: string, e: React.MouseEvent) => {
    if (e.button === 1) { // Middle mouse button
      e.preventDefault()
      onTabClose(tabId)
    }
  }

  return (
    <div className="tab-bar">
      <div 
        className="tabs-container" 
        ref={tabsContainerRef}
        onWheel={handleWheel}
      >
        {tabs.map((tab) => (
          <div
            key={tab.id}
            data-tab-id={tab.id}
            className={`tab ${activeTabId === tab.id ? 'active' : ''}`}
            onClick={(e) => handleTabClick(tab.id, e)}
            onMouseDown={(e) => handleTabMouseDown(tab.id, e)}
          >
            <span className="tab-title">{tab.title}</span>
            {/* Close button */}
            <button
              className="tab-close"
              onClick={(e) => handleCloseClick(tab.id, e)}
              title="Close tab"
            >
              <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <line x1="18" y1="6" x2="6" y2="18" />
                <line x1="6" y1="6" x2="18" y2="18" />
              </svg>
            </button>
          </div>
        ))}
      </div>
      
      <div className="tab-bar-actions">
        {/* New tab button */}
        <button
          className="new-tab-button"
          onClick={onNewTab}
          title="New conversation"
        >
          <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
            <line x1="12" y1="5" x2="12" y2="19" />
            <line x1="5" y1="12" x2="19" y2="12" />
          </svg>
        </button>
        
        {/* History button */}
        <div className="history-dropdown">
          <button
            ref={historyButtonRef}
            className="history-button"
            onClick={onHistoryClick}
            title="Conversation history"
          >
            <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
              <circle cx="12" cy="12" r="10" />
              <polyline points="12 6 12 12 16 14" />
            </svg>
          </button>
          
          {historyMenuOpen && menuPosition && (
            <div 
              className="history-menu"
              style={{
                position: 'fixed',
                top: menuPosition.top,
                right: menuPosition.right,
              }}
            >
              <div className="history-search">
                <input
                  ref={searchInputRef}
                  type="text"
                  placeholder="Search..."
                  value={searchQuery}
                  onChange={(e) => setSearchQuery(e.target.value)}
                  className="history-search-input"
                />
              </div>
              <div className="history-menu-list">
                {filteredConversations.length === 0 ? (
                  <div className="history-empty">
                    {searchQuery ? 'No matches found' : 'No conversations'}
                  </div>
                ) : (
                  filteredConversations.map((conv) => (
                    <button
                      key={conv.id}
                      className="history-item"
                      onClick={() => onHistorySelect(conv.id)}
                    >
                      <span className="history-item-title">{conv.title}</span>
                      {conv.timestamp && (
                        <span className="history-item-time">{conv.timestamp}</span>
                      )}
                    </button>
                  ))
                )}
              </div>
            </div>
          )}
        </div>
      </div>
    </div>
  )
}
