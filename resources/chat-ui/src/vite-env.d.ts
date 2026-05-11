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
  // Called from C++ to show structured plan questions UI
  showPlanQuestions: (questionsJson: string, tabId?: string) => void;
  // Called from C++ to hide plan questions UI
  hidePlanQuestions: (tabId?: string) => void;
  // Called from C++ to set plan document on a message
  setPlanDocument: (messageId: string, markdown: string, renderMode: string) => void;
  
  // Symbol preview methods
  showSymbolPreview: (messageId: string, symbolJson: string, toolCallId: string) => void;
  showSymbolApproval: (tabId?: string) => void;
  hideSymbolApproval: (tabId?: string) => void;

  // Footprint preview methods
  showFootprintPreview: (messageId: string, footprintJson: string, toolCallId: string) => void;
  showFootprintApproval: (tabId?: string) => void;
  hideFootprintApproval: (tabId?: string) => void;

  // BOM preview methods
  showBomPreview: (messageId: string, bomJson: string, toolCallId: string) => void;
  showBomApproval: (tabId?: string) => void;
  hideBomApproval: (tabId?: string) => void;

  // Todo methods
  // Called from C++ to update the todo list for a specific tab
  setTodos: (todos: TodoItem[], tabId?: string) => void;

  // Selected components context
  setSelectedComponents: (components: ComponentContext[]) => void;
  
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

// Structured question types for plan mode questions UI
export interface QuestionOption {
  id: string;          // "a", "b", "c", "d", "e"
  label: string;
  is_custom?: boolean; // true for the "Other/Custom" option
}

export interface StructuredQuestion {
  id: number;
  text: string;
  options: QuestionOption[];
  is_freeform?: boolean;
}

// Symbol preview for inline symbol rendering
export interface SymbolPreview {
  symbolJson: Record<string, unknown>;
  toolCallId: string;
  options: Array<{ id: string; label: string }>;
}

// Footprint preview for inline footprint rendering
export interface FootprintPreview {
  footprintJson: Record<string, unknown>;
  toolCallId: string;
  options: Array<{ id: string; label: string }>;
}

// BOM supplier info
export interface BomSupplier {
  name: string;
  url: string;
  price: number | null;
  stock: number | null;
}

// BOM line item
export interface BomItem {
  reference: string;
  quantity: number;
  value: string;
  footprint: string;
  mpn: string;
  manufacturer: string;
  description: string;
  unit_price: number | null;
  suppliers: BomSupplier[];
  datasheet_url: string;
}

// BOM preview for inline BOM table rendering
export interface BomPreview {
  bomItems: BomItem[];
  projectName: string;
  totalCost: number | null;
  toolCallId: string;
  options: Array<{ id: string; label: string }>;
}

export interface ChatMessage {
  id: string;
  role: 'user' | 'assistant' | 'system';
  content: string;
  isStreaming?: boolean;
  planDocument?: PlanDocument;
  symbolPreview?: SymbolPreview;
  footprintPreview?: FootprintPreview;
  bomPreview?: BomPreview;
  attachments?: FileAttachment[];
  versionId?: string;
  segments?: Array<
    | { type: 'text'; content: string }
    | { type: 'activity'; labels: string[]; timestamps?: number[]; thinkingContexts?: string[]; durationMs?: number[] }
    | { type: 'thinking'; content: string; durationMs: number; agentId?: string; subAgents?: Array<{ agentId: string; content: string; durationMs: number; label: string }> }
    | { type: 'symbol_preview' }
    | { type: 'footprint_preview' }
    | { type: 'bom_preview' }
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
  | { type: 'symbol_preview' }
  | { type: 'footprint_preview' }
  | { type: 'bom_preview' };

// Selected component context for AI chat
export interface ComponentContext {
  reference: string;  // Reference designator (e.g. "R1", "U1")
  value: string;      // Component value (e.g. "10k", "STM32F103")
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
  allProjects?: boolean;
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

export interface PlanQuestionsSubmitPayload {
  type: 'plan_questions_submit';
  answers: Record<string, { option?: string; custom_text?: string; text?: string }>;
  tabId?: string;
}

// Symbol approval action payloads
export interface SymbolApprovePayload {
  type: 'symbol_approve';
  toolCallId: string;
  tabId?: string;
}

export interface SymbolModifyPayload {
  type: 'symbol_modify';
  toolCallId: string;
  feedback: string;
  tabId?: string;
}

export interface SymbolCancelPayload {
  type: 'symbol_cancel';
  toolCallId: string;
  tabId?: string;
}

// Footprint approval action payloads
export interface FootprintApprovePayload {
  type: 'footprint_approve';
  toolCallId: string;
  tabId?: string;
}

export interface FootprintModifyPayload {
  type: 'footprint_modify';
  toolCallId: string;
  feedback: string;
  tabId?: string;
}

export interface FootprintCancelPayload {
  type: 'footprint_cancel';
  toolCallId: string;
  tabId?: string;
}

// BOM approval action payloads
export interface BomApprovePayload {
  type: 'bom_approve';
  toolCallId: string;
  tabId?: string;
}

export interface BomModifyPayload {
  type: 'bom_modify';
  toolCallId: string;
  feedback: string;
  tabId?: string;
}

export interface BomCancelPayload {
  type: 'bom_cancel';
  toolCallId: string;
  tabId?: string;
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
  | PlanQuestionsSubmitPayload
  | SymbolApprovePayload
  | SymbolModifyPayload
  | SymbolCancelPayload
  | FootprintApprovePayload
  | FootprintModifyPayload
  | FootprintCancelPayload
  | BomApprovePayload
  | BomModifyPayload
  | BomCancelPayload
  | SignInClickPayload
  | UpgradeClickPayload
  | EditMessagePayload
  | UndoToMessagePayload
  | RegenerateMessagePayload;
