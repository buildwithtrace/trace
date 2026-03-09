import { useState, useCallback, useEffect, useRef } from 'react'
import TabBar from './components/TabBar'
import MessageList from './components/MessageList'
import InputArea from './components/InputArea'
import PlanApprovalBar from './components/PlanApprovalBar'
import AuthHeader from './components/AuthHeader'
import QuotaBanner from './components/QuotaBanner'
import { useBridge } from './hooks/useBridge'
import type { ChatMessage, ChatMode, TabInfo, ConversationInfo, PhaseIndicatorState, PlanDocument, FileAttachment, TodoItem, ActivityItem, TurnSegment } from './vite-env.d'

let activityIdCounter = 0
function nextActivityId(): string {
  return `act-${++activityIdCounter}`
}

interface TabState {
  messages: ChatMessage[]
  activityItems: ActivityItem[]
  activeThinkingId: string | null
  activeThinkingByAgent: Record<string, string>
  isStreaming: boolean
  todos: TodoItem[]
  phaseIndicator: PhaseIndicatorState | null
  showPlanApproval: boolean
  turnSegments: TurnSegment[]
  currentAssistantMsgId: string | null
}

const DEFAULT_TAB_STATE: TabState = {
  messages: [], activityItems: [], activeThinkingId: null, activeThinkingByAgent: {},
  isStreaming: false, todos: [],
  phaseIndicator: null, showPlanApproval: false,
  turnSegments: [], currentAssistantMsgId: null
}

function App() {
  const [tabs, setTabs] = useState<TabInfo[]>([])
  const [activeTabId, setActiveTabId] = useState<string | null>(null)
  const [tabStates, setTabStates] = useState<Map<string, TabState>>(new Map())
  const activeTabIdRef = useRef<string | null>(null)

  const [historyMenuOpen, setHistoryMenuOpen] = useState(false)
  const [historyConversations, setHistoryConversations] = useState<ConversationInfo[]>([])

  const [theme, setTheme] = useState<'light' | 'dark'>('light')
  const [mode, setMode] = useState<ChatMode>('agent')
  const [inputEnabled, setInputEnabled] = useState(true)
  const [inputPlaceholder, setInputPlaceholder] = useState('Ask Trace AI...')

  const [isAuthenticated, setIsAuthenticated] = useState(false)
  const [, setUserName] = useState<string | null>(null)
  const [, setUserEmail] = useState<string | null>(null)
  const [bannerMessage, setBannerMessage] = useState<string | null>(null)
  const [bannerShowUpgrade, setBannerShowUpgrade] = useState(false)
  const [isOnline, setIsOnline] = useState(typeof navigator !== 'undefined' ? navigator.onLine : true)
  const [draftMessage, setDraftMessage] = useState<string | null>(null)

  useEffect(() => {
    const goOnline = () => setIsOnline(true)
    const goOffline = () => setIsOnline(false)
    window.addEventListener('online', goOnline)
    window.addEventListener('offline', goOffline)
    return () => {
      window.removeEventListener('online', goOnline)
      window.removeEventListener('offline', goOffline)
    }
  }, [])

  useEffect(() => {
    activeTabIdRef.current = activeTabId
  }, [activeTabId])

  const currentTabState = activeTabId ? tabStates.get(activeTabId) : null
  const messages = currentTabState?.messages ?? []
  const activityItems = currentTabState?.activityItems ?? []
  const isStreaming = currentTabState?.isStreaming ?? false
  const todos = currentTabState?.todos ?? []
  const phaseIndicator = currentTabState?.phaseIndicator ?? null
  const showPlanApproval = currentTabState?.showPlanApproval ?? false
  const turnSegments = currentTabState?.turnSegments ?? []

  const updateTabState = useCallback((tabId: string, updater: (state: TabState) => TabState) => {
    setTabStates(prev => {
      const newMap = new Map(prev)
      const currentState = newMap.get(tabId) ?? { ...DEFAULT_TAB_STATE }
      newMap.set(tabId, updater(currentState))
      return newMap
    })
  }, [])

  const resolveTabId = useCallback((explicitTabId?: string): string | null => {
    return explicitTabId || activeTabIdRef.current
  }, [])

  // --- Tab management ---
  const handleAddTab = useCallback((tab: TabInfo) => {
    setTabs(prev => {
      if (prev.some(t => t.id === tab.id)) return prev
      return [...prev, tab]
    })
    setTabStates(prev => {
      const newMap = new Map(prev)
      if (!newMap.has(tab.id)) {
        newMap.set(tab.id, { ...DEFAULT_TAB_STATE })
      }
      return newMap
    })
  }, [])

  const handleRemoveTab = useCallback((id: string) => {
    if (activeTabIdRef.current === id) {
      activeTabIdRef.current = null
    }
    setTabs(prev => prev.filter(t => t.id !== id))
    setTabStates(prev => {
      const newMap = new Map(prev)
      newMap.delete(id)
      return newMap
    })
    setActiveTabId(prev => prev === id ? null : prev)
  }, [])

  const handleSelectTab = useCallback((id: string) => {
    activeTabIdRef.current = id
    setActiveTabId(id)
  }, [])

  const handleSetTabTitle = useCallback((id: string, title: string) => {
    setTabs(prev => prev.map(t => t.id === id ? { ...t, title } : t))
  }, [])

  const handleReplaceTabId = useCallback((oldId: string, newId: string, title: string) => {
    setTabs(prev => prev.map(t => t.id === oldId ? { ...t, id: newId, title } : t))
    setTabStates(prev => {
      const newMap = new Map(prev)
      const oldState = newMap.get(oldId)
      if (oldState) {
        newMap.delete(oldId)
        newMap.set(newId, oldState)
      }
      return newMap
    })
    if (activeTabIdRef.current === oldId) {
      activeTabIdRef.current = newId
    }
    setActiveTabId(prev => prev === oldId ? newId : prev)
  }, [])

  // --- Messages ---
  const addMessage = useCallback((message: ChatMessage, tabId?: string) => {
    const tid = resolveTabId(tabId)
    if (!tid) return
    console.log('[DEBUG addMessage]', message.id, message.role, 
      'segments:', message.segments ? message.segments.length : 0)
    updateTabState(tid, state => {
      const newState = {
        ...state,
        messages: [...state.messages, message],
      }
      if (message.role === 'assistant') {
        if (message.segments && message.segments.length > 0) {
          newState.turnSegments = []
        } else {
          const preserved = state.turnSegments.filter(seg => seg.type === 'activity')
          if (preserved.length > 0 && message.content) {
            newState.turnSegments = [...preserved, { type: 'text' as const, content: message.content }]
          }
        }
        newState.currentAssistantMsgId = message.id
      }
      return newState
    })
  }, [updateTabState, resolveTabId])

  const appendToMessage = useCallback((id: string, text: string, tabId?: string) => {
    const tid = resolveTabId(tabId)
    if (!tid) return
    updateTabState(tid, state => {
      const isWorkingPlaceholder = (a: ActivityItem) =>
        a.type === 'status' && a.isActive && a.label === 'Working...'
      let items = state.activityItems
        .filter(a => !isWorkingPlaceholder(a))
        .map(a => {
          if (a.type === 'status' && a.isActive) {
            return { ...a, isActive: false, endTime: a.endTime ?? Date.now() }
          }
          return a
        })
      if (state.activeThinkingId) {
        items = items.map(a =>
          a.id === state.activeThinkingId
            ? { ...a, isActive: false, endTime: a.endTime ?? Date.now() }
            : a
        )
      }
      let segs = state.turnSegments.map(seg => {
        if (seg.type !== 'activity') return seg
        let updated = (seg as { type: 'activity'; items: ActivityItem[] }).items
          .filter(a => !isWorkingPlaceholder(a))
          .map(a => {
            if (a.type === 'status' && a.isActive) {
              return { ...a, isActive: false, endTime: a.endTime ?? Date.now() }
            }
            return a
          })
        if (state.activeThinkingId) {
          updated = updated.map(a =>
            a.id === state.activeThinkingId
              ? { ...a, isActive: false, endTime: a.endTime ?? Date.now() }
              : a
          )
        }
        return updated.length > 0
          ? { type: 'activity' as const, items: updated }
          : null
      }).filter(Boolean) as TurnSegment[]
      const inResearchPhase = state.phaseIndicator?.label?.toLowerCase().includes('research')
        && !state.phaseIndicator?.label?.toLowerCase().includes('attempt')
      const hasResearchSeg = segs.some(s => s.type === 'research')
      if (inResearchPhase && !hasResearchSeg) {
        segs = [...segs, { type: 'research' as const, title: 'Research Summary', content: '' }]
      }
      let lastTextIdx = -1
      const lastSeg = segs.length > 0 ? segs[segs.length - 1] : null
      if (lastSeg && (lastSeg.type === 'text' || lastSeg.type === 'research')) {
        lastTextIdx = segs.length - 1
      } else if (lastSeg && lastSeg.type === 'activity') {
        for (let i = segs.length - 1; i >= 0; i--) {
          if (segs[i].type === 'research') { lastTextIdx = i; break }
          if (segs[i].type === 'text') {
            const isWordContinuation = text.length > 0 && /^[a-z]/.test(text)
            if (isWordContinuation) { lastTextIdx = i }
            break
          }
          if (segs[i].type !== 'activity') break
        }
      }
      if (lastTextIdx >= 0) {
        segs = [...segs]
        const seg = segs[lastTextIdx]
        if (seg.type === 'research') {
          const rSeg = seg as { type: 'research'; title: string; content: string }
          segs[lastTextIdx] = { type: 'research', title: rSeg.title, content: rSeg.content + text }
        } else {
          const textSeg = seg as { type: 'text'; content: string }
          segs[lastTextIdx] = { type: 'text', content: textSeg.content + text }
        }
      } else {
        segs = [...segs, { type: 'text', content: text }]
      }
      return {
        ...state,
        activeThinkingId: null,
        activityItems: items,
        turnSegments: segs,
        messages: state.messages.map(msg =>
          msg.id === id
            ? { ...msg, content: msg.content + text, isStreaming: true }
            : msg
        )
      }
    })
  }, [updateTabState, resolveTabId])

  const finalizeMessage = useCallback((id: string, tabId?: string) => {
    const tid = resolveTabId(tabId)
    if (!tid) return
    updateTabState(tid, state => {
      const items = state.activityItems
        .map(a => a.isActive ? { ...a, isActive: false, endTime: a.endTime ?? Date.now() } : a)
      const segs = state.turnSegments.map(seg => {
        if (seg.type === 'activity') {
          return {
            type: 'activity' as const,
            items: seg.items
              .map(a => a.isActive ? { ...a, isActive: false, endTime: a.endTime ?? Date.now() } : a)
          }
        }
        return seg
      }).filter(seg => seg.type !== 'activity' || (seg as { items: ActivityItem[] }).items.length > 0)

      const thinkingItems = items.filter(a => a.type === 'thinking')
      let thinkingData: { content: string; durationMs: number } | null = null
      if (thinkingItems.length > 0) {
        const allContent = thinkingItems.map(a => a.content).join('\n')
        const earliest = Math.min(...thinkingItems.map(a => a.startTime))
        const latest = Math.max(...thinkingItems.map(a => a.endTime ?? Date.now()))
        thinkingData = { content: allContent, durationMs: latest - earliest }
      }

      console.log('[DEBUG finalizeMessage]', id, 
        'thinkingItems:', thinkingItems.length,
        'thinkingData:', thinkingData ? { len: thinkingData.content.length, dur: thinkingData.durationMs } : null,
        'activityItems count:', state.activityItems.length)

      // Convert live turnSegments to stored segments so the message survives
      // the next setStreaming(true) which clears turnSegments
      type StoredSegment =
        | { type: 'text'; content: string }
        | { type: 'research'; title: string; content: string }
        | { type: 'activity'; labels: string[]; timestamps?: number[]; thinkingContexts?: string[]; durationMs?: number[] }
        | { type: 'thinking'; content: string; durationMs: number; agentId?: string; subAgents?: Array<{ agentId: string; content: string; durationMs: number; label: string }> }
      const storedSegments: StoredSegment[] = []
      for (const seg of segs) {
        if (seg.type === 'text') {
          storedSegments.push({ type: 'text', content: seg.content })
        } else if (seg.type === 'research') {
          storedSegments.push({ type: 'research', title: seg.title, content: seg.content })
        } else if (seg.type === 'activity') {
          const statusItems = seg.items.filter(a => a.type === 'status' && !a.isIdle)
          const thinkItems = seg.items.filter(a => a.type === 'thinking')
          if (thinkItems.length > 0 && statusItems.length === 0) {
            const subAgentItems = thinkItems.filter(t => t.agentId && t.agentId.startsWith('research_'))
            const mainItems = thinkItems.filter(t => !t.agentId || !t.agentId.startsWith('research_'))
            for (const t of mainItems) {
              const dur = (t.endTime ?? Date.now()) - t.startTime
              storedSegments.push({ type: 'thinking', content: t.content, durationMs: dur, agentId: t.agentId })
            }
            if (subAgentItems.length > 0) {
              const subAgents = subAgentItems.map(t => ({
                agentId: t.agentId!,
                content: t.content,
                durationMs: (t.endTime ?? Date.now()) - t.startTime,
                label: t.label.replace(/^\[.*?\]\s*/, ''),
              }))
              const totalDur = subAgents.reduce((sum, s) => sum + s.durationMs, 0)
              storedSegments.push({ type: 'thinking', content: '', durationMs: totalDur, subAgents })
            }
          } else if (statusItems.length > 0) {
            const labels = statusItems.map(a => a.label)
            const timestamps = statusItems.map(a => a.startTime)
            const thinkingContexts = statusItems.map(a => a.thinkingContext ?? '')
            const durationMs = statusItems.map(a => {
              const end = a.endTime ?? Date.now()
              return end - a.startTime
            })
            storedSegments.push({ type: 'activity', labels, timestamps, thinkingContexts, durationMs })
          }
        }
      }

      return {
        ...state,
        activeThinkingId: null,
        activityItems: items,
        turnSegments: [],
        currentAssistantMsgId: null,
        messages: state.messages.map(msg =>
          msg.id === id
            ? { ...msg, isStreaming: false, segments: storedSegments.length > 0 ? storedSegments : undefined }
            : msg
        )
      }
    })
  }, [updateTabState, resolveTabId])

  // --- Activity feed: Typing indicator ---
  const showTypingIndicator = useCallback((tabId?: string) => {
    const tid = resolveTabId(tabId)
    if (!tid) return
    updateTabState(tid, state => {
      if (state.activityItems.some(a => a.type === 'status' && a.label === 'Working...' && a.isActive)) {
        return state
      }
      const item: ActivityItem = {
        id: nextActivityId(), type: 'status', label: 'Working...',
        content: '', startTime: Date.now(), isActive: true,
      }
      const segs = addItemToLatestActivitySeg(state.turnSegments, item)
      return { ...state, activityItems: [...state.activityItems, item], turnSegments: segs }
    })
  }, [updateTabState, resolveTabId])

  const hideTypingIndicator = useCallback((tabId?: string) => {
    const tid = resolveTabId(tabId)
    if (!tid) return
    updateTabState(tid, state => {
      const items = state.activityItems.filter(a =>
        !(a.type === 'status' && a.label === 'Working...' && a.isActive)
      )
      const segs = state.turnSegments.map(seg => {
        if (seg.type === 'activity') {
          return {
            type: 'activity' as const,
            items: seg.items.filter(a =>
              !(a.type === 'status' && a.label === 'Working...' && a.isActive)
            )
          }
        }
        return seg
      }).filter(seg => seg.type !== 'activity' || (seg as { items: ActivityItem[] }).items.length > 0)
      return { ...state, activityItems: items, turnSegments: segs }
    })
  }, [updateTabState, resolveTabId])

  // --- Activity feed: Status ---
  const addItemToLatestActivitySeg = (segs: TurnSegment[], item: ActivityItem): TurnSegment[] => {
    const result = [...segs]
    const lastSeg = result[result.length - 1]
    if (lastSeg && lastSeg.type === 'activity') {
      result[result.length - 1] = { type: 'activity', items: [...lastSeg.items, item] }
    } else {
      result.push({ type: 'activity', items: [item] })
    }
    return result
  }

  const showStatus = useCallback((text: string, agentId?: string, isIdle?: boolean, tabId?: string) => {
    const tid = resolveTabId(tabId)
    if (!tid) return
    updateTabState(tid, state => {
      const existing = state.activityItems.find(a => a.type === 'status' && a.isActive)
      let items = state.activityItems
      let segs = state.turnSegments

      if (existing) {
        const elapsed = Date.now() - existing.startTime
        const isEphemeral = existing.isIdle || elapsed < 500

        if (isEphemeral) {
          items = items.filter(a => a.id !== existing.id)
          segs = segs.map(seg => {
            if (seg.type === 'activity') {
              const filtered = seg.items.filter(a => a.id !== existing.id)
              return { type: 'activity' as const, items: filtered }
            }
            return seg
          }).filter(seg => seg.type !== 'activity' || (seg as { items: ActivityItem[] }).items.length > 0)
        } else {
          items = items.map(a =>
            a.id === existing.id ? { ...a, isActive: false, endTime: a.endTime ?? Date.now() } : a
          )
          segs = segs.map(seg => {
            if (seg.type === 'activity') {
              return {
                type: 'activity' as const,
                items: seg.items.map(a =>
                  a.id === existing.id ? { ...a, isActive: false, endTime: a.endTime ?? Date.now() } : a
                )
              }
            }
            return seg
          })
        }
      }

      const item: ActivityItem = {
        id: nextActivityId(), type: 'status', label: text,
        content: '', startTime: Date.now(), isActive: true,
        agentId: agentId || undefined,
        isIdle: isIdle || false,
      }
      segs = addItemToLatestActivitySeg(segs, item)
      return { ...state, activityItems: [...items, item], turnSegments: segs }
    })
  }, [updateTabState, resolveTabId])

  const hideStatus = useCallback((tabId?: string) => {
    const tid = resolveTabId(tabId)
    if (!tid) return
    updateTabState(tid, state => {
      const activeIdle = state.activityItems.find(a => a.type === 'status' && a.isActive && a.isIdle)
      let items: ActivityItem[]
      let segs: TurnSegment[]
      if (activeIdle) {
        items = state.activityItems.filter(a => a.id !== activeIdle.id)
        segs = state.turnSegments.map(seg => {
          if (seg.type === 'activity') {
            return { type: 'activity' as const, items: seg.items.filter(a => a.id !== activeIdle.id) }
          }
          return seg
        }).filter(seg => seg.type !== 'activity' || (seg as { items: ActivityItem[] }).items.length > 0)
      } else {
        items = state.activityItems.map(a =>
          (a.type === 'status' && a.isActive)
            ? { ...a, isActive: false, endTime: a.endTime ?? Date.now() }
            : a
        )
        segs = state.turnSegments.map(seg => {
          if (seg.type === 'activity') {
            return {
              type: 'activity' as const,
              items: seg.items.map(a =>
                (a.type === 'status' && a.isActive)
                  ? { ...a, isActive: false, endTime: a.endTime ?? Date.now() }
                  : a
              )
            }
          }
          return seg
        }).filter(seg => seg.type !== 'activity' || (seg as { items: ActivityItem[] }).items.length > 0)
      }
      return { ...state, activityItems: items, turnSegments: segs }
    })
  }, [updateTabState, resolveTabId])

  // --- Activity feed: Thinking ---
  const handleShowThinking = useCallback((content: string, agentId?: string, tabId?: string) => {
    const tid = resolveTabId(tabId)
    if (!tid) return
    const agentKey = agentId || ''
    updateTabState(tid, state => {
      const existingId = state.activeThinkingByAgent[agentKey]
      if (existingId) {
        const items = state.activityItems.map(a =>
          a.id === existingId
            ? { ...a, content: a.content + content }
            : a
        )
        const segs = state.turnSegments.map(seg => {
          if (seg.type === 'activity') {
            return {
              type: 'activity' as const,
              items: seg.items.map(a =>
                a.id === existingId ? { ...a, content: a.content + content } : a
              )
            }
          }
          return seg
        })
        return { ...state, activityItems: items, turnSegments: segs }
      }
      if (!content) return state
      const id = nextActivityId()
      const item: ActivityItem = {
        id, type: 'thinking', label: agentId ? `[${agentId}] Thinking...` : 'Thinking...',
        content, startTime: Date.now(), isActive: true,
      }
      const filtered = state.activityItems.filter(a =>
        !(a.type === 'status' && a.label === 'Working...' && a.isActive)
      )
      const isWorkingStatus = (a: ActivityItem) =>
        a.type === 'status' && a.label === 'Working...' && a.isActive
      const cleanedSegs = state.turnSegments.map(seg => {
        if (seg.type === 'activity') {
          return { type: 'activity' as const, items: seg.items.filter(a => !isWorkingStatus(a)) }
        }
        return seg
      }).filter(seg => seg.type !== 'activity' || (seg as { items: ActivityItem[] }).items.length > 0)
      const segs = addItemToLatestActivitySeg(cleanedSegs, item)
      return {
        ...state,
        activeThinkingId: id,
        activeThinkingByAgent: { ...state.activeThinkingByAgent, [agentKey]: id },
        activityItems: [...filtered, item],
        turnSegments: segs,
      }
    })
  }, [updateTabState, resolveTabId])

  const handleHideThinking = useCallback((tabId?: string) => {
    const tid = resolveTabId(tabId)
    if (!tid) return
    updateTabState(tid, state => {
      const allThinkingIds = Object.values(state.activeThinkingByAgent)
      if (!state.activeThinkingId && allThinkingIds.length === 0) return state
      const idsToClose = new Set(allThinkingIds)
      if (state.activeThinkingId) idsToClose.add(state.activeThinkingId)
      const items = state.activityItems.map(a =>
        idsToClose.has(a.id)
          ? { ...a, isActive: false, endTime: a.endTime ?? Date.now() }
          : a
      )
      const segs = state.turnSegments.map(seg => {
        if (seg.type === 'activity') {
          return {
            type: 'activity' as const,
            items: seg.items.map(a =>
              idsToClose.has(a.id)
                ? { ...a, isActive: false, endTime: a.endTime ?? Date.now() }
                : a
            )
          }
        }
        return seg
      })
      return { ...state, activeThinkingId: null, activeThinkingByAgent: {}, activityItems: items, turnSegments: segs }
    })
  }, [updateTabState, resolveTabId])

  // --- Activity feed: Tool calls (no-op for UI; friendly status already arrives via showStatus) ---
  const handleShowToolCall = useCallback((_toolName: string, statusText: string, tabId?: string) => {
    if (!statusText) return
    const tid = resolveTabId(tabId)
    if (!tid) return
    updateTabState(tid, state => {
      const existing = state.activityItems.find(a => a.type === 'status' && a.isActive)
      if (existing) {
        const items = state.activityItems.map(a =>
          a.id === existing.id ? { ...a, isActive: false, endTime: a.endTime ?? Date.now() } : a
        )
        const segs = state.turnSegments.map(seg => {
          if (seg.type === 'activity') {
            return {
              type: 'activity' as const,
              items: seg.items.map(a =>
                a.id === existing.id ? { ...a, isActive: false, endTime: a.endTime ?? Date.now() } : a
              )
            }
          }
          return seg
        })
        const item: ActivityItem = {
          id: nextActivityId(), type: 'status', label: statusText,
          content: '', startTime: Date.now(), isActive: true,
        }
        const newSegs = addItemToLatestActivitySeg(segs, item)
        return { ...state, activityItems: [...items, item], turnSegments: newSegs }
      }
      const item: ActivityItem = {
        id: nextActivityId(), type: 'status', label: statusText,
        content: '', startTime: Date.now(), isActive: true,
      }
      const segs = addItemToLatestActivitySeg(state.turnSegments, item)
      return { ...state, activityItems: [...state.activityItems, item], turnSegments: segs }
    })
  }, [updateTabState, resolveTabId])

  const handleShowFileEdit = useCallback((filename: string, oldContent: string, newContent: string, tabId?: string) => {
    const tid = resolveTabId(tabId)
    if (!tid) return
    updateTabState(tid, state => {
      const writeLabels = ['Wrote ', 'Edited ']
      const lastWriteIdx = [...state.activityItems].reverse().findIndex(a =>
        a.type === 'status' && writeLabels.some(p => a.label.startsWith(p))
      )
      if (lastWriteIdx === -1) return state

      const realIdx = state.activityItems.length - 1 - lastWriteIdx
      const target = state.activityItems[realIdx]
      const updated = { ...target, diffInfo: { filename, oldContent, newContent } }
      const items = [...state.activityItems]
      items[realIdx] = updated

      const segs = state.turnSegments.map(seg => {
        if (seg.type !== 'activity') return seg
        return {
          type: 'activity' as const,
          items: seg.items.map(a => a.id === target.id ? updated : a)
        }
      })
      return { ...state, activityItems: items, turnSegments: segs }
    })
  }, [updateTabState, resolveTabId])

  const handleSetTheme = useCallback((isDark: boolean) => {
    setTheme(isDark ? 'dark' : 'light')
  }, [])

  const clearMessages = useCallback((tabId?: string) => {
    const tid = resolveTabId(tabId)
    if (!tid) return
    updateTabState(tid, state => ({ ...state, messages: [], activityItems: [], turnSegments: [], currentAssistantMsgId: null }))
  }, [updateTabState, resolveTabId])

  const handleSetMode = useCallback((newMode: ChatMode) => {
    setMode(newMode)
  }, [])

  const handleSetInputEnabled = useCallback((enabled: boolean) => {
    setInputEnabled(enabled)
  }, [])

  const handleSetInputPlaceholder = useCallback((placeholder: string) => {
    setInputPlaceholder(placeholder)
  }, [])

  const handleSetStreaming = useCallback((streaming: boolean, tabId?: string) => {
    const tid = resolveTabId(tabId)
    if (!tid) return
    updateTabState(tid, state => {
      if (streaming) {
        return {
          ...state,
          isStreaming: true,
          activityItems: [],
          turnSegments: [],
          activeThinkingId: null,
          currentAssistantMsgId: null,
        }
      }
      return {
        ...state,
        isStreaming: false,
        activityItems: state.activityItems.map(a =>
          a.isActive ? { ...a, isActive: false, endTime: a.endTime ?? Date.now() } : a
        ).filter(a => !a.isIdle),
        turnSegments: state.turnSegments.map(seg => {
          if (seg.type === 'activity') {
            return {
              type: 'activity' as const,
              items: seg.items.map(a =>
                a.isActive ? { ...a, isActive: false, endTime: a.endTime ?? Date.now() } : a
              ).filter(a => !a.isIdle)
            }
          }
          return seg
        }).filter(seg => seg.type !== 'activity' || (seg as { items: ActivityItem[] }).items.length > 0),
        activeThinkingId: null,
        activeThinkingByAgent: {},
        messages: state.messages.map(msg =>
          msg.isStreaming ? { ...msg, isStreaming: false } : msg
        ),
      }
    })
  }, [updateTabState, resolveTabId])

  const handleShowHistoryMenu = useCallback((conversations: ConversationInfo[]) => {
    setHistoryConversations(conversations)
    setHistoryMenuOpen(true)
  }, [])

  // --- Plan mode ---
  const handleShowPhaseIndicator = useCallback((label: string, color: string, tabId?: string) => {
    const tid = resolveTabId(tabId)
    if (!tid) return
    console.log('[DEBUG] showPhaseIndicator:', label, color)
    updateTabState(tid, state => {
      const newState = { ...state, phaseIndicator: { label, color } }
      const lowerLabel = label.toLowerCase()
      if (lowerLabel.includes('research') && !lowerLabel.includes('attempt')) {
        const hasResearchAlready = state.turnSegments.some(s => s.type === 'research')
        if (!hasResearchAlready) {
          let absorbedContent = ''
          const segs = state.turnSegments.filter(seg => {
            if (seg.type === 'text') {
              absorbedContent += (seg as { type: 'text'; content: string }).content
              return false
            }
            return true
          })
          segs.push({ type: 'research', title: 'Research Summary', content: absorbedContent })
          newState.turnSegments = segs
        }
      }
      return newState
    })
  }, [updateTabState, resolveTabId])

  const handleHidePhaseIndicator = useCallback((tabId?: string) => {
    const tid = resolveTabId(tabId)
    if (!tid) return
    console.warn('[DEBUG] hidePhaseIndicator called!', new Error().stack)
    updateTabState(tid, state => ({ ...state, phaseIndicator: null }))
  }, [updateTabState, resolveTabId])

  const handleShowPlanApproval = useCallback((tabId?: string) => {
    const tid = resolveTabId(tabId)
    if (!tid) return
    updateTabState(tid, state => ({ ...state, showPlanApproval: true }))
  }, [updateTabState, resolveTabId])

  const handleHidePlanApproval = useCallback((tabId?: string) => {
    const tid = resolveTabId(tabId)
    if (!tid) return
    updateTabState(tid, state => ({ ...state, showPlanApproval: false }))
  }, [updateTabState, resolveTabId])

  // --- Auth/payment ---
  const handleSetAuthState = useCallback((authenticated: boolean) => {
    setIsAuthenticated(authenticated)
    if (!authenticated) {
      setUserName(null)
      setUserEmail(null)
    }
  }, [])

  const handleSetUserInfo = useCallback((name: string, email: string) => {
    setUserName(name || null)
    setUserEmail(email || null)
  }, [])

  const handleShowBanner = useCallback((message: string, showUpgrade: boolean) => {
    setBannerMessage(message)
    setBannerShowUpgrade(showUpgrade)
  }, [])

  const handleHideBanner = useCallback(() => {
    setBannerMessage(null)
    setBannerShowUpgrade(false)
  }, [])

  const handleSetPlanDocument = useCallback((messageId: string, markdown: string, renderMode: string, tabId?: string) => {
    const tid = resolveTabId(tabId)
    if (!tid) return

    const planDocument: PlanDocument = {
      markdown,
      renderMode: renderMode as 'full' | 'summary'
    }

    updateTabState(tid, state => {
      const existingMessage = state.messages.find(msg => msg.id === messageId)

      if (existingMessage) {
        return {
          ...state,
          messages: state.messages.map(msg =>
            msg.id === messageId ? { ...msg, planDocument } : msg
          )
        }
      } else {
        const newMessage: ChatMessage = {
          id: messageId, role: 'assistant', content: '', planDocument
        }
        return {
          ...state,
          messages: [...state.messages, newMessage],
        }
      }
    })
  }, [updateTabState, resolveTabId])

  const handleSetTodos = useCallback((newTodos: TodoItem[], tabId?: string) => {
    const tid = resolveTabId(tabId)
    if (!tid) return
    updateTabState(tid, state => ({ ...state, todos: newTodos }))
  }, [updateTabState, resolveTabId])

  // --- Bridge ---
  const { postMessage } = useBridge({
    addTab: handleAddTab,
    removeTab: handleRemoveTab,
    selectTab: handleSelectTab,
    setTabTitle: handleSetTabTitle,
    replaceTabId: handleReplaceTabId,
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
    showPhaseIndicator: handleShowPhaseIndicator,
    hidePhaseIndicator: handleHidePhaseIndicator,
    showPlanApproval: handleShowPlanApproval,
    hidePlanApproval: handleHidePlanApproval,
    setPlanDocument: handleSetPlanDocument,
    setTodos: handleSetTodos,
    showThinking: handleShowThinking,
    hideThinking: handleHideThinking,
    showToolCall: handleShowToolCall,
    showFileEdit: handleShowFileEdit,
    setAuthState: handleSetAuthState,
    setUserInfo: handleSetUserInfo,
    showBanner: handleShowBanner,
    hideBanner: handleHideBanner,
  })

  // --- User actions ---
  const handleSendMessage = useCallback((content: string, attachments?: FileAttachment[]) => {
    if (!isOnline) {
      setDraftMessage(content)
      return
    }
    postMessage({ type: 'send_message', content, attachments })
  }, [postMessage, isOnline])

  const handleStopRequest = useCallback(() => {
    postMessage({ type: 'stop_request', tabId: activeTabIdRef.current ?? undefined })
  }, [postMessage])

  const handleModeChange = useCallback((newMode: ChatMode) => {
    setMode(newMode)
    postMessage({ type: 'mode_changed', mode: newMode })
  }, [postMessage])

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

  const handlePlanApprove = useCallback(() => {
    postMessage({ type: 'plan_approve', tabId: activeTabIdRef.current ?? undefined })
  }, [postMessage])

  const handlePlanMoreResearch = useCallback((feedback: string) => {
    postMessage({ type: 'plan_more_research', feedback, tabId: activeTabIdRef.current ?? undefined })
  }, [postMessage])

  const handlePlanAdjust = useCallback((feedback: string) => {
    postMessage({ type: 'plan_adjust', feedback, tabId: activeTabIdRef.current ?? undefined })
  }, [postMessage])

  const handlePlanCancel = useCallback(() => {
    postMessage({ type: 'plan_cancel', tabId: activeTabIdRef.current ?? undefined })
  }, [postMessage])

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

  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme)
  }, [theme])

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
      <div className="chat-body">
        <MessageList
          messages={messages}
          activityItems={activityItems}
          isStreaming={isStreaming}
          postMessage={postMessage}
          turnSegments={turnSegments}
          todos={todos}
          activeTabId={activeTabId}
        />
      </div>
      {showPlanApproval && (
        <PlanApprovalBar
          onApprove={handlePlanApprove}
          onMoreResearch={handlePlanMoreResearch}
          onAdjust={handlePlanAdjust}
          onCancel={handlePlanCancel}
        />
      )}
      {!isOnline && (
        <div className="connection-banner">
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
            <line x1="1" y1="1" x2="23" y2="23" />
            <path d="M16.72 11.06A10.94 10.94 0 0 1 19 12.55" />
            <path d="M5 12.55a10.94 10.94 0 0 1 5.17-2.39" />
            <path d="M10.71 5.05A16 16 0 0 1 22.56 9" />
            <path d="M1.42 9a15.91 15.91 0 0 1 4.7-2.88" />
            <path d="M8.53 16.11a6 6 0 0 1 6.95 0" />
            <line x1="12" y1="20" x2="12.01" y2="20" />
          </svg>
          <span>No internet connection</span>
        </div>
      )}
      {bannerMessage && (
        <QuotaBanner
          message={bannerMessage}
          showUpgrade={bannerShowUpgrade}
          onUpgrade={handleUpgrade}
          onDismiss={handleBannerDismiss}
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
        draftMessage={draftMessage}
        onDraftConsumed={() => setDraftMessage(null)}
        phaseIndicator={phaseIndicator}
      />
    </div>
  )
}

export default App
