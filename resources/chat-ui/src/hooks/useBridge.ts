import { useEffect, useCallback, useRef } from 'react'
import type { ChatMessage, ChatMode, TabInfo, ConversationInfo, TodoItem, BridgePayload } from '../vite-env.d'

interface BridgeHandlers {
  // Tab management
  addTab: (tab: TabInfo) => void
  removeTab: (id: string) => void
  selectTab: (id: string) => void
  setTabTitle: (id: string, title: string) => void
  replaceTabId: (oldId: string, newId: string, title: string) => void
  
  // Messages (tabId optional -- if provided, targets that tab; otherwise uses active tab)
  addMessage: (message: ChatMessage, tabId?: string) => void
  appendToMessage: (id: string, text: string, tabId?: string) => void
  finalizeMessage: (id: string, tabId?: string) => void
  showTypingIndicator: (tabId?: string) => void
  hideTypingIndicator: (tabId?: string) => void
  showStatus: (text: string, agentId?: string, isIdle?: boolean, tabId?: string) => void
  hideStatus: (tabId?: string) => void
  setTheme: (isDark: boolean) => void
  clearMessages: (tabId?: string) => void
  setMode: (mode: ChatMode) => void
  setInputEnabled: (enabled: boolean) => void
  setInputPlaceholder: (placeholder: string) => void
  setStreaming: (isStreaming: boolean, tabId?: string) => void
  showHistoryMenu: (conversations: ConversationInfo[]) => void
  
  // Plan mode
  showPhaseIndicator: (label: string, color: string, tabId?: string) => void
  hidePhaseIndicator: (tabId?: string) => void
  showPlanApproval: (tabId?: string) => void
  hidePlanApproval: (tabId?: string) => void
  setPlanDocument: (messageId: string, markdown: string, renderMode: string, tabId?: string) => void
  
  // Todos
  setTodos: (todos: TodoItem[], tabId?: string) => void
  
  // Thinking
  showThinking: (content: string, agentId?: string, tabId?: string) => void
  hideThinking: (tabId?: string) => void
  
  // Tool calls
  showToolCall: (toolName: string, statusText: string, tabId?: string) => void
  
  // File edits
  showFileEdit: (filename: string, oldContent: string, newContent: string, tabId?: string) => void
  
  // Auth/payment
  setAuthState: (isAuthenticated: boolean) => void
  setUserInfo: (name: string, email: string) => void
  showBanner: (message: string, showUpgrade: boolean) => void
  hideBanner: () => void
}

export function useBridge(handlers: BridgeHandlers) {
  // Store handlers in a ref to avoid re-running effect when handlers change
  const handlersRef = useRef(handlers)
  handlersRef.current = handlers

  // Post message to C++ host
  const postMessage = useCallback((payload: BridgePayload) => {
    const message = JSON.stringify(payload)
    
    // Try WebKit (macOS)
    if (window.webkit?.messageHandlers?.kicad) {
      window.webkit.messageHandlers.kicad.postMessage(message)
      return
    }
    
    // Try Chrome WebView2 (Windows)
    if (window.chrome?.webview?.postMessage) {
      window.chrome.webview.postMessage(message)
      return
    }
    
    // Try external.invoke (Linux/GTK WebKit)
    // Use type assertion to work around TypeScript's built-in External type
    const ext = window.external as { invoke?: (msg: string) => void } | undefined
    if (ext?.invoke) {
      try {
        ext.invoke(message)
        return
      } catch (err) {
        console.error('external.invoke failed:', err)
      }
    }
    
    // Fallback: log to console for development
    console.log('[Trace Bridge] Would send to C++:', payload)
  }, [])

  // Register bridge handlers on window.kichat - only run once on mount
  useEffect(() => {
    // Create wrapper functions that always call the latest handlers via ref
    window.kichat = {
      // Tab management
      addTab: (tab) => handlersRef.current.addTab(tab),
      removeTab: (id) => handlersRef.current.removeTab(id),
      selectTab: (id) => handlersRef.current.selectTab(id),
      setTabTitle: (id, title) => handlersRef.current.setTabTitle(id, title),
      replaceTabId: (oldId, newId, title) => handlersRef.current.replaceTabId(oldId, newId, title),
      
      // Messages -- tabId parameter is optional, passed through from C++
      addMessage: (message, tabId?) => handlersRef.current.addMessage(message, tabId),
      appendToMessage: (id, text, tabId?) => handlersRef.current.appendToMessage(id, text, tabId),
      finalizeMessage: (id, tabId?) => handlersRef.current.finalizeMessage(id, tabId),
      showTypingIndicator: () => handlersRef.current.showTypingIndicator(),
      hideTypingIndicator: () => handlersRef.current.hideTypingIndicator(),
      showStatus: (text, agentId?, isIdle?, tabId?) => handlersRef.current.showStatus(text, agentId, isIdle, tabId),
      hideStatus: (tabId?) => handlersRef.current.hideStatus(tabId),
      setTheme: (isDark) => handlersRef.current.setTheme(isDark),
      clearMessages: () => handlersRef.current.clearMessages(),
      setMode: (mode) => handlersRef.current.setMode(mode),
      setInputEnabled: (enabled) => handlersRef.current.setInputEnabled(enabled),
      setInputPlaceholder: (placeholder) => handlersRef.current.setInputPlaceholder(placeholder),
      setStreaming: (isStreaming, tabId?) => handlersRef.current.setStreaming(isStreaming, tabId),
      showHistoryMenu: (conversations) => handlersRef.current.showHistoryMenu(conversations),
      
      // Plan mode
      showPhaseIndicator: (label, color, tabId?) => handlersRef.current.showPhaseIndicator(label, color, tabId),
      hidePhaseIndicator: (tabId?) => handlersRef.current.hidePhaseIndicator(tabId),
      showPlanApproval: (tabId?) => handlersRef.current.showPlanApproval(tabId),
      hidePlanApproval: (tabId?) => handlersRef.current.hidePlanApproval(tabId),
      setPlanDocument: (messageId, markdown, renderMode) => handlersRef.current.setPlanDocument(messageId, markdown, renderMode),
      
      // Todos
      setTodos: (todos, tabId?) => handlersRef.current.setTodos(todos, tabId),
      
      // Thinking
      showThinking: (content: string, agentId?: string, tabId?: string) => handlersRef.current.showThinking(content, agentId, tabId),
      hideThinking: (tabId?) => handlersRef.current.hideThinking(tabId),
      
      // Tool calls
      showToolCall: (toolName, statusText, tabId?) => handlersRef.current.showToolCall(toolName, statusText, tabId),
      
      // File edits
      showFileEdit: (filename, oldContent, newContent, tabId?) => handlersRef.current.showFileEdit(filename, oldContent, newContent, tabId),
      
      // Auth/payment
      setAuthState: (isAuthenticated) => handlersRef.current.setAuthState(isAuthenticated),
      setUserInfo: (name, email) => handlersRef.current.setUserInfo(name, email),
      showBanner: (message, showUpgrade) => handlersRef.current.showBanner(message, showUpgrade),
      hideBanner: () => handlersRef.current.hideBanner(),
      onQuotaWarning: (dataStr: string) => {
        try {
          const data = typeof dataStr === 'string' ? JSON.parse(dataStr) : dataStr
          const showUpgrade = data.level === 'critical'
          handlersRef.current.showBanner(data.content || 'Approaching usage limit', showUpgrade)
        } catch {
          handlersRef.current.showBanner('Approaching usage limit', true)
        }
      },
    }

    // Notify C++ that the bridge is ready
    // Retry multiple times because the message handler may not be registered yet
    // (OnWebViewLoaded fires after the page loads, but React runs immediately)
    let retryCount = 0
    const maxRetries = 10
    const retryInterval = 100 // ms
    
    const sendBridgeReady = () => {
      postMessage({ type: 'send_message', content: '__BRIDGE_READY__' })
      retryCount++
      
      if (retryCount < maxRetries) {
        setTimeout(sendBridgeReady, retryInterval)
      }
    }
    
    // Start sending bridge ready signals
    sendBridgeReady()

    return () => {
      // Cleanup is not strictly necessary but good practice
    }
  }, [postMessage]) // Only depend on postMessage, not handlers

  return { postMessage }
}
