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
  replaceTabId: (oldId: string, newId: string, title: string) => void;
  
  // Called from C++ to add a new message (tabId identifies which tab to target)
  addMessage: (message: ChatMessage, tabId?: string) => void;
  // Called from C++ to append text to an existing message (streaming)
  appendToMessage: (id: string, text: string, tabId?: string) => void;
  // Called from C++ to finalize a streaming message
  finalizeMessage: (id: string, tabId?: string) => void;
  // Called from C++ to show typing indicator
  showTypingIndicator: () => void;
  // Called from C++ to hide typing indicator
  hideTypingIndicator: () => void;
  // Called from C++ to show status text
  showStatus: (text: string, agentId?: string, isIdle?: boolean, tabId?: string) => void;
  // Called from C++ to hide status
  hideStatus: (tabId?: string) => void;
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
  setStreaming: (isStreaming: boolean, tabId?: string) => void;
  // Called from C++ to show history menu
  showHistoryMenu: (conversations: ConversationInfo[]) => void;
  
  // Plan mode methods
  // Called from C++ to show phase indicator (e.g., "Research Phase")
  showPhaseIndicator: (label: string, color: string, tabId?: string) => void;
  // Called from C++ to hide phase indicator
  hidePhaseIndicator: (tabId?: string) => void;
  // Called from C++ to show plan approval buttons
  showPlanApproval: (tabId?: string) => void;
  // Called from C++ to hide plan approval buttons
  hidePlanApproval: (tabId?: string) => void;
  // Called from C++ to set plan document on a message
  setPlanDocument: (messageId: string, markdown: string, renderMode: string) => void;
  
  // Todo methods
  // Called from C++ to update the todo list for a specific tab
  setTodos: (todos: TodoItem[], tabId?: string) => void;
  
  // Thinking methods
  showThinking: (content: string, agentId?: string, tabId?: string) => void;
  hideThinking: (tabId?: string) => void;
  
  // Tool call activity feed
  showToolCall: (toolName: string, statusText: string, tabId?: string) => void;
  
  // File edit methods
  // Called from C++ to display a file diff widget
  showFileEdit: (filename: string, oldContent: string, newContent: string, tabId?: string) => void;
  
  // Auth/payment methods
  // Called from C++ to update authentication state
  setAuthState: (isAuthenticated: boolean) => void;
  setUserInfo: (name: string, email: string) => void;
  // Called from C++ to show a quota/plan banner with optional upgrade button
  showBanner: (message: string, showUpgrade: boolean) => void;
  // Called from C++ to hide the quota/plan banner
  hideBanner: () => void;
  // Called from C++ when approaching monthly usage cap
  onQuotaWarning: (data: string) => void;
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
  planDocument?: PlanDocument;
  attachments?: FileAttachment[];
  versionId?: string;
  segments?: Array<
    | { type: 'text'; content: string }
    | { type: 'research'; title: string; content: string }
    | { type: 'activity'; labels: string[]; timestamps?: number[]; thinkingContexts?: string[]; durationMs?: number[] }
    | { type: 'thinking'; content: string; durationMs: number; agentId?: string; subAgents?: Array<{ agentId: string; content: string; durationMs: number; label: string }> }
    | { type: 'research'; title: string; content: string }
  >;
}

export type ChatMode = 'ask' | 'plan' | 'agent';

// Todo item for AI task tracking
export interface TodoItem {
  id: string;
  content: string;
  status: 'pending' | 'in_progress' | 'completed' | 'cancelled';
}

// Phase indicator state
export interface PhaseIndicatorState {
  label: string;
  color: string;  // CSS color value
}

// Activity feed item types
export type ActivityType = 'thinking' | 'tool_call' | 'status';

export interface ActivityItem {
  id: string;
  type: ActivityType;
  label: string;
  content: string;
  toolName?: string;
  agentId?: string;
  startTime: number;
  endTime?: number;
  isActive: boolean;
  isIdle?: boolean;
  thinkingContext?: string;
  diffInfo?: { filename: string; oldContent: string; newContent: string };
}

// Interleaved timeline segment for assistant turns
export type TurnSegment =
  | { type: 'activity'; items: ActivityItem[] }
  | { type: 'text'; content: string }
  | { type: 'research'; title: string; content: string };

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
  tabId?: string;
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
  tabId?: string;
}

export interface PlanMoreResearchPayload {
  type: 'plan_more_research';
  feedback: string;
  tabId?: string;
}

export interface PlanAdjustPayload {
  type: 'plan_adjust';
  feedback: string;
  tabId?: string;
}

export interface PlanCancelPayload {
  type: 'plan_cancel';
  tabId?: string;
}

export interface PlanEditSavePayload {
  type: 'plan_edit_save';
  markdown: string;
  tabId?: string;
}

export interface PlanSaveToWorkspacePayload {
  type: 'plan_save_to_workspace';
  markdown: string;
}

// Auth/payment action payloads
export interface SignInClickPayload {
  type: 'sign_in_click';
}

export interface UpgradeClickPayload {
  type: 'upgrade_click';
}

export interface EditMessagePayload {
  type: 'edit_message';
  messageId: string;
  newContent: string;
  tabId?: string;
}

export interface UndoToMessagePayload {
  type: 'undo_to_message';
  messageId: string;
  tabId?: string;
}

export interface RegenerateMessagePayload {
  type: 'regenerate_message';
  messageId: string;
  tabId?: string;
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
  | PlanEditSavePayload
  | PlanSaveToWorkspacePayload
  | SignInClickPayload
  | UpgradeClickPayload
  | EditMessagePayload
  | UndoToMessagePayload
  | RegenerateMessagePayload;
