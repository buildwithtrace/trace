import { useState, useCallback, useEffect, useRef } from 'react'
import TabBar from './components/TabBar'
import MessageList from './components/MessageList'
import InputArea from './components/InputArea'
import PhaseIndicator from './components/PhaseIndicator'
import PlanApprovalBar from './components/PlanApprovalBar'
import AuthHeader from './components/AuthHeader'
import QuotaBanner from './components/QuotaBanner'
import { useBridge } from './hooks/useBridge'
import type { ChatMessage, ChatMode, TabInfo, ConversationInfo, PhaseIndicatorState, PlanDocument, FileAttachment } from './vite-env.d'

// Per-tab state
interface TabState {
  messages: ChatMessage[]
  isTyping: boolean
  statusText: string | null
  isStreaming: boolean  // Explicit streaming state from backend
}

function App() {
  // Tab state
  const [tabs, setTabs] = useState<TabInfo[]>([])
  const [activeTabId, setActiveTabId] = useState<string | null>(null)
  const [tabStates, setTabStates] = useState<Map<string, TabState>>(new Map())
  
  // Ref to track active tab ID synchronously (for bridge callbacks that fire in rapid succession)
  const activeTabIdRef = useRef<string | null>(null)
  
  // History menu state
  const [historyMenuOpen, setHistoryMenuOpen] = useState(false)
  const [historyConversations, setHistoryConversations] = useState<ConversationInfo[]>([])
  
  // Global UI state
  const [theme, setTheme] = useState<'light' | 'dark'>('light')
  const [mode, setMode] = useState<ChatMode>('agent')
  const [inputEnabled, setInputEnabled] = useState(true)
  const [inputPlaceholder, setInputPlaceholder] = useState('Ask Trace AI...')
  
  // Plan mode state
  const [phaseIndicator, setPhaseIndicator] = useState<PhaseIndicatorState | null>(null)
  const [showPlanApproval, setShowPlanApproval] = useState(false)
  
  // Auth/payment state
  const [isAuthenticated, setIsAuthenticated] = useState(false)
  const [bannerMessage, setBannerMessage] = useState<string | null>(null)
  const [bannerShowUpgrade, setBannerShowUpgrade] = useState(false)

  // Keep ref in sync with state
  useEffect(() => {
    activeTabIdRef.current = activeTabId
  }, [activeTabId])

  // Get current tab's state
  const currentTabState = activeTabId ? tabStates.get(activeTabId) : null
  const messages = currentTabState?.messages ?? []
  const isTyping = currentTabState?.isTyping ?? false
  const statusText = currentTabState?.statusText ?? null
  const isStreaming = currentTabState?.isStreaming ?? false

  // Helper to update a specific tab's state
  const updateTabState = useCallback((tabId: string, updater: (state: TabState) => TabState) => {
    setTabStates(prev => {
      const newMap = new Map(prev)
      const currentState = newMap.get(tabId) ?? { messages: [], isTyping: false, statusText: null, isStreaming: false }
      newMap.set(tabId, updater(currentState))
      return newMap
    })
  }, [])

  // Bridge handlers - Tab management
  const handleAddTab = useCallback((tab: TabInfo) => {
    setTabs(prev => {
      // Check if tab already exists
      if (prev.some(t => t.id === tab.id)) {
        return prev
      }
      return [...prev, tab]
    })
    setTabStates(prev => {
      const newMap = new Map(prev)
      if (!newMap.has(tab.id)) {
        newMap.set(tab.id, { messages: [], isTyping: false, statusText: null, isStreaming: false })
      }
      return newMap
    })
  }, [])

  const handleRemoveTab = useCallback((id: string) => {
    // If removing the active tab, clear the ref immediately
    if (activeTabIdRef.current === id) {
      activeTabIdRef.current = null
    }
    setTabs(prev => prev.filter(t => t.id !== id))
    setTabStates(prev => {
      const newMap = new Map(prev)
      newMap.delete(id)
      return newMap
    })
    // Update state if we removed the active tab
    setActiveTabId(prev => prev === id ? null : prev)
  }, [])

  const handleSelectTab = useCallback((id: string) => {
    // Update ref synchronously so subsequent bridge calls can use it immediately
    activeTabIdRef.current = id
    setActiveTabId(id)
  }, [])

  const handleSetTabTitle = useCallback((id: string, title: string) => {
    setTabs(prev => prev.map(t => t.id === id ? { ...t, title } : t))
  }, [])

  // Bridge handlers - Messages (operate on active tab using ref for synchronous access)
  const addMessage = useCallback((message: ChatMessage) => {
    const tabId = activeTabIdRef.current
    if (!tabId) return
    updateTabState(tabId, state => ({
      ...state,
      messages: [...state.messages, message],
      isTyping: false
    }))
  }, [updateTabState])

  const appendToMessage = useCallback((id: string, text: string) => {
    const tabId = activeTabIdRef.current
    if (!tabId) return
    updateTabState(tabId, state => ({
      ...state,
      messages: state.messages.map(msg => 
        msg.id === id 
          ? { ...msg, content: msg.content + text, isStreaming: true }
          : msg
      )
    }))
  }, [updateTabState])

  const finalizeMessage = useCallback((_id: string) => {
    const tabId = activeTabIdRef.current
    if (!tabId) return
    updateTabState(tabId, state => ({
      ...state,
      // Clear isStreaming on all messages (for visual purposes like streaming animation)
      messages: state.messages.map(msg => 
        msg.isStreaming ? { ...msg, isStreaming: false } : msg
      )
    }))
  }, [updateTabState])

  const showTypingIndicator = useCallback(() => {
    const tabId = activeTabIdRef.current
    if (!tabId) return
    updateTabState(tabId, state => ({ ...state, isTyping: true }))
  }, [updateTabState])

  const hideTypingIndicator = useCallback(() => {
    const tabId = activeTabIdRef.current
    if (!tabId) return
    updateTabState(tabId, state => ({ ...state, isTyping: false }))
  }, [updateTabState])

  const showStatus = useCallback((text: string) => {
    const tabId = activeTabIdRef.current
    if (!tabId) return
    updateTabState(tabId, state => ({ ...state, statusText: text }))
  }, [updateTabState])

  const hideStatus = useCallback(() => {
    const tabId = activeTabIdRef.current
    if (!tabId) return
    updateTabState(tabId, state => ({ ...state, statusText: null }))
  }, [updateTabState])

  const handleSetTheme = useCallback((isDark: boolean) => {
    setTheme(isDark ? 'dark' : 'light')
  }, [])

  const clearMessages = useCallback(() => {
    const tabId = activeTabIdRef.current
    if (!tabId) return
    updateTabState(tabId, state => ({ ...state, messages: [] }))
  }, [updateTabState])

  const handleSetMode = useCallback((newMode: ChatMode) => {
    setMode(newMode)
  }, [])

  const handleSetInputEnabled = useCallback((enabled: boolean) => {
    setInputEnabled(enabled)
  }, [])

  const handleSetInputPlaceholder = useCallback((placeholder: string) => {
    setInputPlaceholder(placeholder)
  }, [])

  const handleSetStreaming = useCallback((streaming: boolean) => {
    const tabId = activeTabIdRef.current
    if (!tabId) return
    updateTabState(tabId, state => ({ ...state, isStreaming: streaming }))
  }, [updateTabState])

  const handleShowHistoryMenu = useCallback((conversations: ConversationInfo[]) => {
    setHistoryConversations(conversations)
    setHistoryMenuOpen(true)
  }, [])

  // Plan mode bridge handlers
  const handleShowPhaseIndicator = useCallback((label: string, color: string) => {
    setPhaseIndicator({ label, color })
  }, [])

  const handleHidePhaseIndicator = useCallback(() => {
    setPhaseIndicator(null)
  }, [])

  const handleShowPlanApproval = useCallback(() => {
    setShowPlanApproval(true)
  }, [])

  const handleHidePlanApproval = useCallback(() => {
    setShowPlanApproval(false)
  }, [])

  // Auth/payment bridge handlers
  const handleSetAuthState = useCallback((authenticated: boolean) => {
    setIsAuthenticated(authenticated)
  }, [])

  const handleShowBanner = useCallback((message: string, showUpgrade: boolean) => {
    setBannerMessage(message)
    setBannerShowUpgrade(showUpgrade)
  }, [])

  const handleHideBanner = useCallback(() => {
    setBannerMessage(null)
    setBannerShowUpgrade(false)
  }, [])

  const handleSetPlanDocument = useCallback((messageId: string, markdown: string, renderMode: string) => {
    const tabId = activeTabIdRef.current
    if (!tabId) return
    
    const planDocument: PlanDocument = {
      markdown,
      renderMode: renderMode as 'full' | 'summary'
    }
    
    updateTabState(tabId, state => {
      // Check if message already exists
      const existingMessage = state.messages.find(msg => msg.id === messageId)
      
      if (existingMessage) {
        // Update existing message with plan document
        return {
          ...state,
          messages: state.messages.map(msg =>
            msg.id === messageId
              ? { ...msg, planDocument }
              : msg
          )
        }
      } else {
        // Message doesn't exist yet - add it with the plan document
        const newMessage: ChatMessage = {
          id: messageId,
          role: 'assistant',
          content: '',
          planDocument
        }
        return {
          ...state,
          messages: [...state.messages, newMessage],
          isTyping: false
        }
      }
    })
  }, [updateTabState])

  // Initialize bridge
  const { postMessage } = useBridge({
    addTab: handleAddTab,
    removeTab: handleRemoveTab,
    selectTab: handleSelectTab,
    setTabTitle: handleSetTabTitle,
    addMessage,
    appendToMessage,
    finalizeMessage,
    showTypingIndicator,
    hideTypingIndicator,
    showStatus,
    hideStatus,
    setTheme: handleSetTheme,
    clearMessages,
    setMode: handleSetMode,
    setInputEnabled: handleSetInputEnabled,
    setInputPlaceholder: handleSetInputPlaceholder,
    setStreaming: handleSetStreaming,
    showHistoryMenu: handleShowHistoryMenu,
    // Plan mode handlers
    showPhaseIndicator: handleShowPhaseIndicator,
    hidePhaseIndicator: handleHidePhaseIndicator,
    showPlanApproval: handleShowPlanApproval,
    hidePlanApproval: handleHidePlanApproval,
    setPlanDocument: handleSetPlanDocument,
    // Auth/payment handlers
    setAuthState: handleSetAuthState,
    showBanner: handleShowBanner,
    hideBanner: handleHideBanner,
  })

  // Handle send message
  const handleSendMessage = useCallback((content: string, attachments?: FileAttachment[]) => {
    // Send to C++ backend (C++ will add the message to UI with attachments)
    postMessage({ type: 'send_message', content, attachments })
  }, [postMessage])

  // Handle stop request
  const handleStopRequest = useCallback(() => {
    postMessage({ type: 'stop_request' })
  }, [postMessage])

  // Handle mode change
  const handleModeChange = useCallback((newMode: ChatMode) => {
    setMode(newMode)
    postMessage({ type: 'mode_changed', mode: newMode })
  }, [postMessage])

  // Tab event handlers
  const handleTabSelect = useCallback((tabId: string) => {
    setActiveTabId(tabId)
    postMessage({ type: 'tab_selected', tabId })
  }, [postMessage])

  const handleTabClose = useCallback((tabId: string) => {
    postMessage({ type: 'tab_close', tabId })
  }, [postMessage])

  const handleNewTab = useCallback(() => {
    postMessage({ type: 'new_tab' })
  }, [postMessage])

  const handleHistoryClick = useCallback(() => {
    postMessage({ type: 'history_click' })
  }, [postMessage])

  const handleHistorySelect = useCallback((conversationId: string) => {
    setHistoryMenuOpen(false)
    postMessage({ type: 'history_select', conversationId })
  }, [postMessage])

  // Plan action handlers
  const handlePlanApprove = useCallback(() => {
    postMessage({ type: 'plan_approve' })
  }, [postMessage])

  const handlePlanMoreResearch = useCallback((feedback: string) => {
    postMessage({ type: 'plan_more_research', feedback })
  }, [postMessage])

  const handlePlanAdjust = useCallback((feedback: string) => {
    postMessage({ type: 'plan_adjust', feedback })
  }, [postMessage])

  const handlePlanCancel = useCallback(() => {
    postMessage({ type: 'plan_cancel' })
  }, [postMessage])

  // Auth/payment action handlers
  const handleSignIn = useCallback(() => {
    postMessage({ type: 'sign_in_click' })
  }, [postMessage])

  const handleUpgrade = useCallback(() => {
    postMessage({ type: 'upgrade_click' })
  }, [postMessage])

  const handleBannerDismiss = useCallback(() => {
    setBannerMessage(null)
    setBannerShowUpgrade(false)
  }, [])

  // Apply theme to document
  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme)
  }, [theme])

  // Close history menu when clicking outside
  useEffect(() => {
    const handleClickOutside = (e: MouseEvent) => {
      const target = e.target as HTMLElement
      if (historyMenuOpen && !target.closest('.history-menu') && !target.closest('.history-button')) {
        setHistoryMenuOpen(false)
      }
    }
    document.addEventListener('mousedown', handleClickOutside)
    return () => document.removeEventListener('mousedown', handleClickOutside)
  }, [historyMenuOpen])

  return (
    <div className={`chat-container ${theme}`}>
      {!isAuthenticated && (
        <AuthHeader onSignIn={handleSignIn} />
      )}
      {isAuthenticated && (
        <TabBar
          tabs={tabs}
          activeTabId={activeTabId}
          onTabSelect={handleTabSelect}
          onTabClose={handleTabClose}
          onNewTab={handleNewTab}
          onHistoryClick={handleHistoryClick}
          historyMenuOpen={historyMenuOpen}
          historyConversations={historyConversations}
          onHistorySelect={handleHistorySelect}
        />
      )}
      {bannerMessage && (
        <QuotaBanner
          message={bannerMessage}
          showUpgrade={bannerShowUpgrade}
          onUpgrade={handleUpgrade}
          onDismiss={handleBannerDismiss}
        />
      )}
      <PhaseIndicator phase={phaseIndicator} />
      <MessageList 
        messages={messages} 
        isTyping={isTyping}
        statusText={statusText}
      />
      {showPlanApproval && (
        <PlanApprovalBar
          onApprove={handlePlanApprove}
          onMoreResearch={handlePlanMoreResearch}
          onAdjust={handlePlanAdjust}
          onCancel={handlePlanCancel}
        />
      )}
      <InputArea
        onSendMessage={handleSendMessage}
        onStopRequest={handleStopRequest}
        onModeChange={handleModeChange}
        mode={mode}
        inputEnabled={inputEnabled && isAuthenticated}
        placeholder={isAuthenticated ? inputPlaceholder : 'Sign in to start chatting...'}
        isStreaming={isStreaming}
      />
    </div>
  )
}

export default App
