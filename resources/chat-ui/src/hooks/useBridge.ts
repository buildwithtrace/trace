import { useEffect, useCallback, useRef } from 'react'
import type { ChatMessage, ChatMode, TabInfo, ConversationInfo, BridgePayload } from '../vite-env.d'

interface BridgeHandlers {
  // Tab management
  addTab: (tab: TabInfo) => void
  removeTab: (id: string) => void
  selectTab: (id: string) => void
  setTabTitle: (id: string, title: string) => void
  
  // Messages
  addMessage: (message: ChatMessage) => void
  appendToMessage: (id: string, text: string) => void
  finalizeMessage: (id: string) => void
  showTypingIndicator: () => void
  hideTypingIndicator: () => void
  showStatus: (text: string) => void
  hideStatus: () => void
  setTheme: (isDark: boolean) => void
  clearMessages: () => void
  setMode: (mode: ChatMode) => void
  setInputEnabled: (enabled: boolean) => void
  setInputPlaceholder: (placeholder: string) => void
  setStreaming: (isStreaming: boolean) => void
  showHistoryMenu: (conversations: ConversationInfo[]) => void
  
  // Plan mode
  showPhaseIndicator: (label: string, color: string) => void
  hidePhaseIndicator: () => void
  showPlanApproval: () => void
  hidePlanApproval: () => void
  setPlanDocument: (messageId: string, markdown: string, renderMode: string) => void
  
  // Auth/payment
  setAuthState: (isAuthenticated: boolean) => void
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
    console.log('[KiChat Bridge] Would send to C++:', payload)
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
      
      // Messages
      addMessage: (message) => handlersRef.current.addMessage(message),
      appendToMessage: (id, text) => handlersRef.current.appendToMessage(id, text),
      finalizeMessage: (id) => handlersRef.current.finalizeMessage(id),
      showTypingIndicator: () => handlersRef.current.showTypingIndicator(),
      hideTypingIndicator: () => handlersRef.current.hideTypingIndicator(),
      showStatus: (text) => handlersRef.current.showStatus(text),
      hideStatus: () => handlersRef.current.hideStatus(),
      setTheme: (isDark) => handlersRef.current.setTheme(isDark),
      clearMessages: () => handlersRef.current.clearMessages(),
      setMode: (mode) => handlersRef.current.setMode(mode),
      setInputEnabled: (enabled) => handlersRef.current.setInputEnabled(enabled),
      setInputPlaceholder: (placeholder) => handlersRef.current.setInputPlaceholder(placeholder),
      setStreaming: (isStreaming) => handlersRef.current.setStreaming(isStreaming),
      showHistoryMenu: (conversations) => handlersRef.current.showHistoryMenu(conversations),
      
      // Plan mode
      showPhaseIndicator: (label, color) => handlersRef.current.showPhaseIndicator(label, color),
      hidePhaseIndicator: () => handlersRef.current.hidePhaseIndicator(),
      showPlanApproval: () => handlersRef.current.showPlanApproval(),
      hidePlanApproval: () => handlersRef.current.hidePlanApproval(),
      setPlanDocument: (messageId, markdown, renderMode) => handlersRef.current.setPlanDocument(messageId, markdown, renderMode),
      
      // Auth/payment
      setAuthState: (isAuthenticated) => handlersRef.current.setAuthState(isAuthenticated),
      showBanner: (message, showUpgrade) => handlersRef.current.showBanner(message, showUpgrade),
      hideBanner: () => handlersRef.current.hideBanner(),
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
