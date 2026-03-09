import React, { useEffect, useRef, useState, useMemo, useCallback } from 'react'
import MessageBubble from './MessageBubble'
import ActivityItemComponent from './ActivityItem'
import TodoWidget from './TodoWidget'
import type { ChatMessage, BridgePayload, ActivityItem, TurnSegment, TodoItem } from '../vite-env.d'
import PlanDocumentWidget from './PlanDocumentWidget'
import traceLogoDark from '../assets/trace_logo_dark.svg'

interface MessageListProps {
  messages: ChatMessage[]
  activityItems: ActivityItem[]
  isStreaming: boolean
  postMessage: (payload: BridgePayload) => void
  turnSegments: TurnSegment[]
  todos: TodoItem[]
  activeTabId: string | null
}

function formatDuration(startTime: number, endTime?: number): string | null {
  const end = endTime ?? Date.now()
  const ms = end - startTime
  if (ms <= 0) return null
  if (ms < 1000) return '<1s'
  const seconds = Math.round(ms / 1000)
  if (seconds < 60) return `${seconds}s`
  const minutes = Math.floor(seconds / 60)
  const remaining = seconds % 60
  return `${minutes}m ${remaining}s`
}

function formatMs(ms: number): string {
  if (ms < 1000) return '<1s'
  const seconds = Math.round(ms / 1000)
  if (seconds < 60) return `${seconds}s`
  const minutes = Math.floor(seconds / 60)
  const remaining = seconds % 60
  return `${minutes}m ${remaining}s`
}

const pastTenseMap: Record<string, string> = {
  'Running tool...': 'Ran tool',
  'Working...': 'Completed',
  'Calculating positions...': 'Calculated positions',
  'Finding placement...': 'Found placement',
  'Determining layout...': 'Determined layout',
  'Generating IDs...': 'Generated IDs',
  'Creating identifiers...': 'Created identifiers',
  'Preparing references...': 'Prepared references',
  'Running design rule check...': 'Ran design rule check',
  'Checking for violations...': 'Checked for violations',
  'Running electrical rule check...': 'Ran electrical rule check',
  'Checking connections...': 'Checked connections',
  'Annotating components...': 'Annotated components',
  'Labeling parts...': 'Labeled parts',
  'Taking a screenshot...': 'Took screenshot',
  'Capturing the current view...': 'Captured view',
  'One moment...': 'Completed',
  'Still working on it...': 'Completed',
}

function toPastTense(label: string): string {
  if (pastTenseMap[label]) return pastTenseMap[label]
  if (label.startsWith('Using ') && label.endsWith('...'))
    return 'Used ' + label.slice(6, -3)
  if (label.startsWith('Reading ') && label.endsWith('...'))
    return 'Read ' + label.slice(8, -3)
  if (label.startsWith('Writing ') && label.endsWith('...'))
    return 'Wrote ' + label.slice(8, -3)
  if (label.startsWith('Editing ') && label.endsWith('...'))
    return 'Edited ' + label.slice(8, -3)
  if (label.startsWith('Browsing ') && label.endsWith('...'))
    return 'Browsed ' + label.slice(9, -3)
  if (label.startsWith('Searching footprints') && label.endsWith('...'))
    return 'Found footprints' + label.slice(20, -3)
  if (label.startsWith('Searching ') && label.endsWith('...'))
    return 'Searched ' + label.slice(10, -3)
  if (label.startsWith('Generating ') && label.endsWith('...'))
    return 'Generated ' + label.slice(11, -3)
  if (label.startsWith('Researching') && label.endsWith('...'))
    return 'Researched' + label.slice(11, -3)
  if (label.endsWith('...'))
    return label.slice(0, -3)
  return label
}

function categorizeLabel(label: string): 'file' | 'search' | 'browse' | 'other' {
  const l = toPastTense(label)
  if (/^(Read|Wrote|Edited) /i.test(l)) return 'file'
  if (/^(Searched|Found) /i.test(l)) return 'search'
  if (/^Browsed /i.test(l)) return 'browse'
  return 'other'
}

function buildActivitySummary(statusItems: ActivityItem[]): string {
  const counts = { file: 0, search: 0, browse: 0, other: 0 }
  for (const item of statusItems) {
    counts[categorizeLabel(item.label)]++
  }
  const parts: string[] = []
  if (counts.file > 0) parts.push(`${counts.file} file${counts.file > 1 ? 's' : ''}`)
  if (counts.search > 0) parts.push(`${counts.search} search${counts.search > 1 ? 'es' : ''}`)
  if (counts.browse > 0) parts.push(`${counts.browse} lookup${counts.browse > 1 ? 's' : ''}`)
  if (counts.other > 0) parts.push(`${counts.other} action${counts.other > 1 ? 's' : ''}`)
  return parts.join(', ')
}

function StoredThinkingToggle({ thinking }: { thinking: { content: string; durationMs: number; agentId?: string; subAgents?: Array<{ agentId: string; content: string; durationMs: number; label: string }> } }) {
  const [expanded, setExpanded] = useState(false)
  const [expandedSubs, setExpandedSubs] = useState<Set<number>>(new Set())
  const scrollRef = useRef<HTMLDivElement>(null)
  const [scrollState, setScrollState] = useState<'fits' | 'scrollable' | 'at-bottom'>('fits')

  const checkScroll = useCallback(() => {
    const el = scrollRef.current
    if (!el) return
    if (el.scrollHeight <= el.clientHeight) {
      setScrollState('fits')
    } else if (el.scrollTop + el.clientHeight >= el.scrollHeight - 5) {
      setScrollState('at-bottom')
    } else {
      setScrollState('scrollable')
    }
  }, [])

  useEffect(() => {
    if (expanded) {
      requestAnimationFrame(checkScroll)
    }
  }, [expanded, checkScroll])

  const isGrouped = thinking.subAgents && thinking.subAgents.length > 0

  if (isGrouped) {
    const subs = thinking.subAgents!
    const totalDur = formatMs(thinking.durationMs)
    const label = `Researched ${subs.length} topic${subs.length > 1 ? 's' : ''}`

    const toggleSub = (idx: number) => {
      setExpandedSubs(prev => {
        const next = new Set(prev)
        if (next.has(idx)) next.delete(idx)
        else next.add(idx)
        return next
      })
    }

    return (
      <div className="sub-agent-group">
        <button
          className="sub-agent-group-header"
          onClick={() => setExpanded(prev => !prev)}
        >
          <span className="sub-agent-group-label">{label}</span>
          <span className="activity-duration-badge">{totalDur}</span>
          <svg
            className={`activity-caret ${expanded ? 'expanded' : ''}`}
            viewBox="0 0 24 24" width="10" height="10"
            fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round"
          >
            <polyline points="9 6 15 12 9 18" />
          </svg>
        </button>
        {expanded && (
          <div className="sub-agent-group-children">
            {subs.map((sub, idx) => {
              const dur = formatMs(sub.durationMs)
              const agentLabel = sub.label.replace(/^research_/, '').replace(/_/g, ' ')
              const isSubExpanded = expandedSubs.has(idx)
              const hasContent = sub.content && sub.content.length > 0
              return (
                <div key={idx} className="sub-agent-item">
                  <button
                    className="sub-agent-item-header"
                    onClick={() => hasContent && toggleSub(idx)}
                    disabled={!hasContent}
                  >
                    <span className="sub-agent-item-dot" />
                    <span className="sub-agent-item-label">{agentLabel}</span>
                    <span className="activity-duration-badge">{dur}</span>
                    {hasContent && (
                      <svg
                        className={`activity-caret ${isSubExpanded ? 'expanded' : ''}`}
                        viewBox="0 0 24 24" width="9" height="9"
                        fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round"
                      >
                        <polyline points="9 6 15 12 9 18" />
                      </svg>
                    )}
                  </button>
                  {isSubExpanded && hasContent && (
                    <div className="sub-agent-item-content">
                      {sub.content}
                    </div>
                  )}
                </div>
              )
            })}
          </div>
        )}
      </div>
    )
  }

  const label = `Thought for ${formatMs(thinking.durationMs)}`

  return (
    <div className="activity-feed">
      <button
        className="activity-feed-header"
        onClick={() => setExpanded(prev => !prev)}
      >
        <span className="activity-feed-label">{label}</span>
        <svg
          className={`activity-caret ${expanded ? 'expanded' : ''}`}
          viewBox="0 0 24 24"
          width="10"
          height="10"
          fill="none"
          stroke="currentColor"
          strokeWidth="2.5"
          strokeLinecap="round"
          strokeLinejoin="round"
        >
          <polyline points="9 6 15 12 9 18" />
        </svg>
      </button>
      {expanded && thinking.content.length > 0 && (
        <div className="activity-feed-children">
          <div
            ref={scrollRef}
            className={`thinking-content-scrollable ${scrollState === 'fits' || scrollState === 'at-bottom' ? scrollState : ''}`}
            onScroll={checkScroll}
          >
            <div className="activity-child-content">
              {thinking.content}
            </div>
          </div>
        </div>
      )}
    </div>
  )
}

function ActivityFeed({ items, isLive }: { items: ActivityItem[], isLive: boolean }) {
  const [expanded, setExpanded] = useState(false)
  const prevIsLive = useRef(isLive)
  const scrollRef = useRef<HTMLDivElement>(null)
  const [scrollState, setScrollState] = useState<'fits' | 'scrollable' | 'at-bottom'>('fits')
  const thinkingItems = items.filter(a => a.type === 'thinking')
  const subAgentThinking = thinkingItems.filter(a => a.agentId && a.agentId.startsWith('research_'))
  const mainThinking = thinkingItems.filter(a => !a.agentId || !a.agentId.startsWith('research_'))
  const statusItems = items.filter(a => a.type === 'status' && !a.isActive && !a.isIdle && a.label !== 'Working...')
  const hasThinking = thinkingItems.length > 0
  const hasContent = hasThinking || statusItems.length > 0
  const canExpand = hasThinking || statusItems.length > 1
  const hasSubAgentGroup = subAgentThinking.length > 0

  const checkScroll = useCallback(() => {
    const el = scrollRef.current
    if (!el) return
    if (el.scrollHeight <= el.clientHeight) {
      setScrollState('fits')
    } else if (el.scrollTop + el.clientHeight >= el.scrollHeight - 5) {
      setScrollState('at-bottom')
    } else {
      setScrollState('scrollable')
    }
  }, [])

  useEffect(() => {
    if (prevIsLive.current && !isLive) {
      setExpanded(false)
    }
    prevIsLive.current = isLive
  }, [isLive])

  useEffect(() => {
    if (expanded) {
      requestAnimationFrame(checkScroll)
    }
  }, [expanded, checkScroll, thinkingItems])

  let label: React.ReactNode = 'Working...'
  let activeAgentId: string | undefined
  if (isLive) {
    const activeSubAgents = subAgentThinking.filter(a => a.isActive)
    if (activeSubAgents.length > 0) {
      label = `Researching ${subAgentThinking.length} topic${subAgentThinking.length > 1 ? 's' : ''}...`
    } else {
      const activeStatus = items.find(a => a.type === 'status' && a.isActive && !a.isIdle)
      if (activeStatus) {
        label = activeStatus.label
        activeAgentId = activeStatus.agentId
      } else if (items.some(a => a.type === 'thinking' && a.isActive)) {
        label = 'Thinking...'
      }
    }
  } else {
    const parts: React.ReactNode[] = []

    if (statusItems.length > 0) {
      const uniqueLabels = [...new Set(statusItems.map(a => toPastTense(a.label)))]
      if (uniqueLabels.length <= 2) {
        parts.push(uniqueLabels.join(', '))
      } else {
        const summary = buildActivitySummary(statusItems)
        parts.push(<span key="explored"><strong>Explored</strong>{' '}{summary}</span>)
      }
    }

    if (hasThinking && parts.length === 0) {
      const earliest = Math.min(...thinkingItems.map(a => a.startTime))
      const latest = Math.max(...thinkingItems.map(a => a.endTime ?? Date.now()))
      const dur = formatDuration(earliest, latest)
      parts.push(`Thought for ${dur ?? '<1s'}`)
    }

    if (parts.length === 0) {
      label = 'Completed'
    } else {
      label = parts.length === 1 ? parts[0] : <>{parts[0]} {'·'} {parts[1]}</>
    }
  }

  if (!hasContent && !isLive) return null

  return (
    <div className={`activity-feed ${isLive ? 'active' : ''}`}>
      <button
        className="activity-feed-header"
        onClick={() => canExpand && setExpanded(prev => !prev)}
        disabled={!canExpand}
      >
        <span className={`activity-feed-label ${isLive ? 'shimmer' : ''}`}>
          {activeAgentId && <span className="agent-badge">{activeAgentId}</span>}
          {label}
        </span>
        {canExpand && (
          <svg
            className={`activity-caret ${expanded ? 'expanded' : ''}`}
            viewBox="0 0 24 24"
            width="10"
            height="10"
            fill="none"
            stroke="currentColor"
            strokeWidth="2.5"
            strokeLinecap="round"
            strokeLinejoin="round"
          >
            <polyline points="9 6 15 12 9 18" />
          </svg>
        )}
      </button>
      {canExpand && expanded && (
        <div className="activity-feed-children">
          <div
            ref={scrollRef}
            className={`thinking-content-scrollable ${scrollState === 'fits' || scrollState === 'at-bottom' ? scrollState : ''}`}
            onScroll={checkScroll}
          >
            {hasSubAgentGroup && (
              <div className="sub-agent-group" style={{ margin: '2px 0' }}>
                <div className="sub-agent-group-children" style={{ padding: '6px 10px' }}>
                  {subAgentThinking.map(item => (
                    <div key={item.id} className="sub-agent-item">
                      <div className="sub-agent-item-header">
                        <span className="sub-agent-item-dot" />
                        <span className={`sub-agent-item-label ${item.isActive ? 'shimmer' : ''}`}>
                          {(item.agentId || '').replace(/^research_/, '').replace(/_/g, ' ')}
                        </span>
                        {!item.isActive && (
                          <span className="activity-duration-badge">
                            {formatDuration(item.startTime, item.endTime) || '<1s'}
                          </span>
                        )}
                      </div>
                    </div>
                  ))}
                </div>
              </div>
            )}
            {statusItems.length > 0 ? (
              [...mainThinking, ...statusItems]
                .sort((a, b) => a.startTime - b.startTime)
                .map((item) => {
                  if (item.type === 'thinking') {
                    return <ActivityItemComponent key={item.id} item={item} />
                  }
                  const dur = formatDuration(item.startTime, item.endTime)
                  const hasTC = !!item.thinkingContext
                  return (
                    <div key={item.id} className="activity-child">
                      <button className="activity-child-header" disabled={!hasTC}>
                        <span className="activity-child-label">{toPastTense(item.label)}</span>
                        {dur && <span className="activity-duration-badge">{dur}</span>}
                        {hasTC && (
                          <svg className="activity-caret" viewBox="0 0 24 24" width="9" height="9"
                            fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round">
                            <polyline points="9 6 15 12 9 18" />
                          </svg>
                        )}
                      </button>
                    </div>
                  )
                })
            ) : !hasSubAgentGroup ? (
              mainThinking.map((item) => (
                <div key={item.id} className="activity-child-content">
                  {item.content}
                </div>
              ))
            ) : null}
          </div>
        </div>
      )}
    </div>
  )
}

function InterleavedTextSegment({ content, message, postMessage, tabIsStreaming, showActions }: {
  content: string
  message: ChatMessage
  postMessage: (payload: BridgePayload) => void
  tabIsStreaming?: boolean
  showActions?: boolean
}) {
  const textMsg = useMemo(() => {
    const { planDocument: _pd, ...rest } = message
    return { ...rest, content }
  }, [message, content])

  return <MessageBubble message={textMsg} postMessage={postMessage} tabIsStreaming={tabIsStreaming} showActions={showActions} />
}

function StoredActivityLabel({ labels: rawLabels, timestamps: rawTimestamps, thinkingContexts: rawThinkingContexts, durationMs: rawDurationMs }: { labels: string[]; timestamps?: number[]; thinkingContexts?: string[]; durationMs?: number[] }) {
  const [expanded, setExpanded] = useState(false)
  const [expandedChildren, setExpandedChildren] = useState<Set<number>>(new Set())

  const { labels, timestamps, thinkingContexts, durationMs } = useMemo(() => {
    const filteredIndices: number[] = []
    for (let i = 0; i < rawLabels.length; i++) {
      const past = toPastTense(rawLabels[i])
      const isGeneric = /^(Read|Wrote|Edited) file$/i.test(past)
      if (isGeneric && i + 1 < rawLabels.length) {
        const nextPast = toPastTense(rawLabels[i + 1])
        if (/^(Read|Wrote|Edited) /i.test(nextPast) && nextPast !== past) {
          continue
        }
      }
      filteredIndices.push(i)
    }
    return {
      labels: filteredIndices.map(i => rawLabels[i]),
      timestamps: rawTimestamps ? filteredIndices.map(i => rawTimestamps[i]) : undefined,
      thinkingContexts: rawThinkingContexts ? filteredIndices.map(i => rawThinkingContexts[i]) : undefined,
      durationMs: rawDurationMs ? filteredIndices.map(i => rawDurationMs[i]) : undefined,
    }
  }, [rawLabels, rawTimestamps, rawThinkingContexts, rawDurationMs])

  const pastLabels = labels.map(l => toPastTense(l))
  const unique = [...new Set(pastLabels)]
  const hasAnyThinkingContext = thinkingContexts?.some(tc => tc && tc.length > 0) ?? false
  const canExpand = labels.length > 1 || hasAnyThinkingContext

  const durations: (string | null)[] = useMemo(() => {
    return labels.map((_, i) => {
      let ms = 0
      if (durationMs && durationMs[i] > 0) {
        ms = durationMs[i]
      } else if (timestamps && i + 1 < timestamps.length && timestamps[i] > 0 && timestamps[i + 1] > 0) {
        ms = timestamps[i + 1] - timestamps[i]
      }
      if (ms <= 0) return null
      if (ms < 1000) return '<1s'
      const sec = Math.round(ms / 1000)
      if (sec < 60) return `${sec}s`
      const min = Math.floor(sec / 60)
      return `${min}m ${sec % 60}s`
    })
  }, [labels, timestamps, durationMs])

  let collapsedContent: React.ReactNode
  if (unique.length <= 2) {
    collapsedContent = unique.join(', ')
  } else {
    const counts = { file: 0, search: 0, browse: 0, other: 0 }
    for (const l of pastLabels) {
      if (/^(Read|Wrote|Edited) /i.test(l)) counts.file++
      else if (/^(Searched|Found) /i.test(l)) counts.search++
      else if (/^Browsed /i.test(l)) counts.browse++
      else counts.other++
    }
    const parts: string[] = []
    if (counts.file > 0) parts.push(`${counts.file} file${counts.file > 1 ? 's' : ''}`)
    if (counts.search > 0) parts.push(`${counts.search} search${counts.search > 1 ? 'es' : ''}`)
    if (counts.browse > 0) parts.push(`${counts.browse} lookup${counts.browse > 1 ? 's' : ''}`)
    if (counts.other > 0) parts.push(`${counts.other} action${counts.other > 1 ? 's' : ''}`)
    collapsedContent = <><strong>Explored</strong>{' '}{parts.join(', ')}</>
  }

  return (
    <div className="activity-feed">
      <button
        className="activity-feed-header"
        onClick={() => canExpand && setExpanded(prev => !prev)}
        disabled={!canExpand}
      >
        <span className="activity-feed-label">{collapsedContent}</span>
        {canExpand && (
          <svg
            className={`activity-caret ${expanded ? 'expanded' : ''}`}
            viewBox="0 0 24 24"
            width="10"
            height="10"
            fill="none"
            stroke="currentColor"
            strokeWidth="2.5"
            strokeLinecap="round"
            strokeLinejoin="round"
          >
            <polyline points="9 6 15 12 9 18" />
          </svg>
        )}
      </button>
      {canExpand && expanded && (
        <div className={`activity-feed-children${labels.length === 1 ? ' single-item' : ''}`}>
          <div className="thinking-content-scrollable fits">
            {labels.length === 1 && hasAnyThinkingContext ? (
              // Single item with thinking context: don't repeat the label, just show thinking
              <div className="activity-child-content activity-status-item">
                {durations[0] && <span className="activity-duration-badge">{durations[0]}</span>}
                {thinkingContexts?.[0] && <div className="activity-thinking-context">{thinkingContexts[0]}</div>}
              </div>
            ) : (
              labels.map((lbl, i) => {
                const tc = thinkingContexts?.[i]
                const hasTC = tc && tc.length > 0
                const isChildExpanded = expandedChildren.has(i)
                const toggleChild = () => setExpandedChildren(prev => {
                  const next = new Set(prev)
                  if (next.has(i)) next.delete(i)
                  else next.add(i)
                  return next
                })
                return (
                  <div key={i} className="activity-child">
                    <button
                      className="activity-child-header"
                      onClick={() => hasTC && toggleChild()}
                      disabled={!hasTC}
                    >
                      <span className="activity-child-label">{toPastTense(lbl)}</span>
                      {durations[i] && <span className="activity-duration-badge">{durations[i]}</span>}
                      {hasTC && (
                        <svg
                          className={`activity-caret ${isChildExpanded ? 'expanded' : ''}`}
                          viewBox="0 0 24 24" width="9" height="9"
                          fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round"
                        >
                          <polyline points="9 6 15 12 9 18" />
                        </svg>
                      )}
                    </button>
                    {hasTC && isChildExpanded && (
                      <div className="activity-thinking-context">{tc}</div>
                    )}
                  </div>
                )
              })
            )}
          </div>
        </div>
      )}
    </div>
  )
}

function StoredMessageActions({ content }: { content: string }) {
  const [menuOpen, setMenuOpen] = useState(false)
  const [copied, setCopied] = useState(false)
  const menuRef = useRef<HTMLDivElement>(null)

  const handleCopy = useCallback(() => {
    navigator.clipboard.writeText(content)
    setCopied(true)
    setTimeout(() => {
      setCopied(false)
      setMenuOpen(false)
    }, 1500)
  }, [content])

  useEffect(() => {
    if (!menuOpen) return
    const handleClickOutside = (e: MouseEvent) => {
      if (menuRef.current && !menuRef.current.contains(e.target as Node)) {
        setMenuOpen(false)
      }
    }
    const handleEscape = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setMenuOpen(false)
    }
    document.addEventListener('mousedown', handleClickOutside)
    document.addEventListener('keydown', handleEscape)
    return () => {
      document.removeEventListener('mousedown', handleClickOutside)
      document.removeEventListener('keydown', handleEscape)
    }
  }, [menuOpen])

  return (
    <div className="assistant-actions">
      <div className="assistant-menu-wrapper" ref={menuRef}>
        <button className="assistant-menu-btn" onClick={() => setMenuOpen(prev => !prev)}>
          <svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor">
            <circle cx="5" cy="12" r="2" />
            <circle cx="12" cy="12" r="2" />
            <circle cx="19" cy="12" r="2" />
          </svg>
        </button>
        {menuOpen && (
          <div className="assistant-menu-dropdown">
            <button className="assistant-menu-item" onClick={handleCopy}>
              {copied ? 'Copied!' : 'Copy Message'}
            </button>
          </div>
        )}
      </div>
    </div>
  )
}

function ResearchCard({ title, content, isStreaming: streaming }: { title: string; content: string; isStreaming?: boolean }) {
  const [expanded, setExpanded] = useState(false)
  const previewLines = content.split('\n').filter(l => l.trim()).slice(0, 3).join('\n')
  const previewText = previewLines.length > 150 ? previewLines.slice(0, 150) + '...' : previewLines

  return (
    <div className={`research-card ${expanded ? 'expanded' : ''}`} onClick={() => setExpanded(!expanded)}>
      <div className="research-card-header">
        <span className="research-card-title">{title}</span>
        <svg className={`research-card-chevron ${expanded ? 'open' : ''}`} viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" strokeWidth="2">
          <polyline points="6 9 12 15 18 9" />
        </svg>
        {streaming && <span className="research-card-status-dot" />}
      </div>
      {expanded ? (
        <div className="research-card-content">
          <MessageBubble message={{ id: 'research', role: 'assistant', content, isStreaming: false }} postMessage={() => {}} showActions={false} />
        </div>
      ) : (
        <div className="research-card-preview">{previewText}</div>
      )}
    </div>
  )
}

function StoredSegmentsView({ message, postMessage }: {
  message: ChatMessage
  postMessage: (payload: BridgePayload) => void
}) {
  const segments = message.segments!
  return (
    <>
      {segments.map((seg, i) => {
        if (seg.type === 'activity') {
          return <StoredActivityLabel key={`stored-seg-${i}`} labels={seg.labels} timestamps={seg.timestamps} thinkingContexts={seg.thinkingContexts} durationMs={seg.durationMs} />
        }
        if (seg.type === 'thinking') {
          return <StoredThinkingToggle key={`stored-seg-${i}`} thinking={{ content: seg.content, durationMs: seg.durationMs, agentId: seg.agentId, subAgents: seg.subAgents }} />
        }
        if (seg.type === 'research') {
          return <ResearchCard key={`stored-seg-${i}`} title={seg.title} content={seg.content} />
        }
        const { planDocument: _pd, ...rest } = message
        const textMsg = { ...rest, content: seg.content }
        return <MessageBubble key={`stored-seg-${i}`} message={textMsg} postMessage={postMessage} showActions={false} />
      })}
      {message.planDocument && (
        <PlanDocumentWidget planDocument={message.planDocument} postMessage={postMessage} />
      )}
      {!message.isStreaming && <StoredMessageActions content={message.content} />}
    </>
  )
}

export default function MessageList({ messages, activityItems, isStreaming, postMessage, turnSegments, todos, activeTabId }: MessageListProps) {
  const containerRef = useRef<HTMLDivElement>(null)
  const bottomRef = useRef<HTMLDivElement>(null)
  const isNearBottomRef = useRef(true)
  const prevTabIdRef = useRef(activeTabId)
  const prevMessageCountRef = useRef(messages.length)
  const rafIdRef = useRef<number>(0)
  const [showScrollButton, setShowScrollButton] = useState(false)

  const hasActiveItems = activityItems.some(a => a.isActive)

  const checkNearBottom = () => {
    if (!containerRef.current) return true
    const { scrollTop, scrollHeight, clientHeight } = containerRef.current
    return scrollHeight - scrollTop - clientHeight < 100
  }

  const doScroll = () => {
    const el = containerRef.current
    if (el) el.scrollTop = el.scrollHeight
  }

  const scrollToBottomIfNeeded = () => {
    if (isNearBottomRef.current) {
      cancelAnimationFrame(rafIdRef.current)
      rafIdRef.current = requestAnimationFrame(doScroll)
    }
  }

  const handleScroll = () => {
    const nearBottom = checkNearBottom()
    isNearBottomRef.current = nearBottom
    setShowScrollButton(!nearBottom && (hasActiveItems || isStreaming))
  }

  // Tab switch: reset to bottom
  useEffect(() => {
    if (prevTabIdRef.current !== activeTabId) {
      isNearBottomRef.current = true
      setShowScrollButton(false)
      requestAnimationFrame(doScroll)
    }
    prevTabIdRef.current = activeTabId
  }, [activeTabId])

  // User sends a new message: auto-scroll to bottom
  useEffect(() => {
    const len = messages.length
    if (len > prevMessageCountRef.current) {
      const lastMsg = messages[len - 1]
      if (lastMsg?.role === 'user') {
        isNearBottomRef.current = true
        setShowScrollButton(false)
        requestAnimationFrame(doScroll)
      }
    }
    prevMessageCountRef.current = len
  }, [messages])

  // Content updates during streaming: scroll only if at bottom
  useEffect(() => {
    scrollToBottomIfNeeded()
  }, [messages, activityItems, turnSegments])

  useEffect(() => {
    setShowScrollButton(!isNearBottomRef.current && (hasActiveItems || isStreaming))
  }, [hasActiveItems, isStreaming])

  useEffect(() => {
    return () => cancelAnimationFrame(rafIdRef.current)
  }, [])

  const lastAssistantMsg = messages.length > 0 && messages[messages.length - 1]?.role === 'assistant'
    ? messages[messages.length - 1]
    : null
  const lastHasStoredSegments = lastAssistantMsg?.segments && lastAssistantMsg.segments.length > 0
  const useInterleaved = turnSegments.length > 0 && lastAssistantMsg !== null && !lastHasStoredSegments

  let lastUserIdx = -1
  if (isStreaming || hasActiveItems) {
    for (let i = messages.length - 1; i >= 0; i--) {
      if (messages[i].role === 'user') { lastUserIdx = i; break }
    }
  }

  return (
    <div 
      className="message-list" 
      ref={containerRef}
      onScroll={handleScroll}
    >
      {messages.length === 0 && !hasActiveItems && turnSegments.length === 0 && (
        <div className="empty-state">
          <div className="empty-state-icon">
            <img src={traceLogoDark} alt="Trace Logo" />
          </div>
          <div className="empty-state-text">Start designing with Trace</div>
          <div className="empty-state-hint">Describe a circuit to build, or open an existing KiCad schematic to keep working on it</div>
        </div>
      )}

      {messages.map((message, idx) => {
        const isInterleavedAssistant = useInterleaved && message.id === lastAssistantMsg!.id
        if (isInterleavedAssistant) return null

        const isLastUser = idx === lastUserIdx
        const showTodoDrawer = isLastUser && todos.length > 0

        return (
          <div key={message.id} className={isLastUser ? (showTodoDrawer ? 'todo-message-group' : 'sticky-user-wrapper') : undefined}>
            {message.role === 'assistant' && message.segments && message.segments.length > 0 ? (
              <StoredSegmentsView message={message} postMessage={postMessage} />
            ) : (
              <MessageBubble message={message} postMessage={postMessage} tabIsStreaming={isStreaming} activeTabId={activeTabId} />
            )}
            {showTodoDrawer && <TodoWidget todos={todos} />}
          </div>
        )
      })}

      {useInterleaved && (() => {
        const lastTextSegIdx = turnSegments.reduce((last, seg, i) => seg.type === 'text' ? i : last, -1)
        return (
          <div className="interleaved-turn">
            {turnSegments.map((seg, i) => {
              if (seg.type === 'activity') {
                const segIsLive = seg.items.some(a => a.isActive)
                return <ActivityFeed key={`seg-${i}`} items={seg.items} isLive={segIsLive} />
              }
              if (seg.type === 'research') {
                return <ResearchCard key={`seg-${i}`} title={seg.title} content={seg.content} isStreaming={isStreaming} />
              }
              return (
                <InterleavedTextSegment
                  key={`seg-${i}`}
                  content={seg.content}
                  message={lastAssistantMsg!}
                  postMessage={postMessage}
                  tabIsStreaming={isStreaming}
                  showActions={!isStreaming && i === lastTextSegIdx}
                />
              )
            })}
            {lastAssistantMsg!.planDocument && (
              <PlanDocumentWidget planDocument={lastAssistantMsg!.planDocument} postMessage={postMessage} activeTabId={activeTabId} />
            )}
          </div>
        )
      })()}

      {!useInterleaved && hasActiveItems && activityItems.length > 0 && (
        <ActivityFeed items={activityItems} isLive={hasActiveItems} />
      )}

      <div ref={bottomRef} />

      {showScrollButton && (
        <button
          className="scroll-to-bottom"
          onClick={() => {
            isNearBottomRef.current = true
            setShowScrollButton(false)
            doScroll()
          }}
          aria-label="Scroll to bottom"
        >
          <svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
            <path d="M4 6l4 4 4-4" />
          </svg>
        </button>
      )}
    </div>
  )
}
