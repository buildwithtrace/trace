/// <reference types="vite/client" />

// Bridge types for C++ <-> React communication

// Override the built-in External interface to add invoke method
interface External {
  invoke?: (message: string) => void;
}

declare global {
  interface Window {
    kichat: KiChatBridge;
    webkit?: {
      messageHandlers?: {
        kicad?: {
          postMessage: (message: string) => void;
        };
      };
    };
    chrome?: {
      webview?: {
        postMessage: (message: string) => void;
      };
    };
    external?: External;
  }
}

export interface KiChatBridge {
  // Tab management
  addTab: (tab: TabInfo) => void;
  removeTab: (id: string) => void;
  selectTab: (id: string) => void;
  setTabTitle: (id: string, title: string) => void;
  
  // Called from C++ to add a new message
  addMessage: (message: ChatMessage) => void;
  // Called from C++ to append text to an existing message (streaming)
  appendToMessage: (id: string, text: string) => void;
  // Called from C++ to finalize a streaming message
  finalizeMessage: (id: string) => void;
  // Called from C++ to show typing indicator
  showTypingIndicator: () => void;
  // Called from C++ to hide typing indicator
  hideTypingIndicator: () => void;
  // Called from C++ to show status text
  showStatus: (text: string) => void;
  // Called from C++ to hide status
  hideStatus: () => void;
  // Called from C++ to set theme
  setTheme: (isDark: boolean) => void;
  // Called from C++ to clear all messages
  clearMessages: () => void;
  // Called from C++ to set the current mode
  setMode: (mode: ChatMode) => void;
  // Called from C++ to enable/disable input
  setInputEnabled: (enabled: boolean) => void;
  // Called from C++ to set input placeholder
  setInputPlaceholder: (placeholder: string) => void;
  // Called from C++ to set streaming state (controls send/stop button)
  setStreaming: (isStreaming: boolean) => void;
  // Called from C++ to show history menu
  showHistoryMenu: (conversations: ConversationInfo[]) => void;
  
  // Plan mode methods
  // Called from C++ to show phase indicator (e.g., "Research Phase")
  showPhaseIndicator: (label: string, color: string) => void;
  // Called from C++ to hide phase indicator
  hidePhaseIndicator: () => void;
  // Called from C++ to show plan approval buttons
  showPlanApproval: () => void;
  // Called from C++ to hide plan approval buttons
  hidePlanApproval: () => void;
  // Called from C++ to set plan document on a message
  setPlanDocument: (messageId: string, markdown: string, renderMode: string) => void;
  
  // Auth/payment methods
  // Called from C++ to update authentication state
  setAuthState: (isAuthenticated: boolean) => void;
  // Called from C++ to show a quota/plan banner with optional upgrade button
  showBanner: (message: string, showUpgrade: boolean) => void;
  // Called from C++ to hide the quota/plan banner
  hideBanner: () => void;
}

export interface TabInfo {
  id: string;
  title: string;
}

export interface ConversationInfo {
  id: string;
  title: string;
  lastMessage?: string;
  timestamp?: string;
}

// Plan document for collapsible plan widget
export interface PlanDocument {
  markdown: string;
  renderMode: 'full' | 'summary';  // full = show approval buttons, summary = display only
}

export interface ChatMessage {
  id: string;
  role: 'user' | 'assistant' | 'system';
  content: string;
  isStreaming?: boolean;
  planDocument?: PlanDocument;  // If present, render as collapsible plan widget
  attachments?: FileAttachment[];  // File attachments (images, PDFs) for the message
}

export type ChatMode = 'ask' | 'plan' | 'agent';

// Phase indicator state
export interface PhaseIndicatorState {
  label: string;
  color: string;  // CSS color value
}

// File attachment for multimodal input
export interface FileAttachment {
  name: string;
  type: string;       // MIME type
  data: string;       // Base64 encoded content
  size: number;       // File size in bytes
}

// Messages sent from React to C++
export interface SendMessagePayload {
  type: 'send_message';
  content: string;
  attachments?: FileAttachment[];
}

export interface StopRequestPayload {
  type: 'stop_request';
}

export interface ModeChangedPayload {
  type: 'mode_changed';
  mode: ChatMode;
}

// Tab events
export interface TabSelectedPayload {
  type: 'tab_selected';
  tabId: string;
}

export interface TabClosePayload {
  type: 'tab_close';
  tabId: string;
}

export interface NewTabPayload {
  type: 'new_tab';
}

export interface HistoryClickPayload {
  type: 'history_click';
}

export interface HistorySelectPayload {
  type: 'history_select';
  conversationId: string;
}

// Plan mode action payloads
export interface PlanApprovePayload {
  type: 'plan_approve';
}

export interface PlanMoreResearchPayload {
  type: 'plan_more_research';
  feedback: string;
}

export interface PlanAdjustPayload {
  type: 'plan_adjust';
  feedback: string;
}

export interface PlanCancelPayload {
  type: 'plan_cancel';
}

// Auth/payment action payloads
export interface SignInClickPayload {
  type: 'sign_in_click';
}

export interface UpgradeClickPayload {
  type: 'upgrade_click';
}

export type BridgePayload = 
  | SendMessagePayload 
  | StopRequestPayload 
  | ModeChangedPayload 
  | TabSelectedPayload
  | TabClosePayload
  | NewTabPayload
  | HistoryClickPayload
  | HistorySelectPayload
  | PlanApprovePayload
  | PlanMoreResearchPayload
  | PlanAdjustPayload
  | PlanCancelPayload
  | SignInClickPayload
  | UpgradeClickPayload;
