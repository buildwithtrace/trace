import { useState, useEffect, useRef } from 'react'
import type { ActivityItem as ActivityItemType } from '../vite-env.d'
import FileEditWidget from './FileEditWidget'

interface ActivityItemProps {
  item: ActivityItemType
}

function formatDuration(startTime: number, endTime?: number): string | null {
  const end = endTime ?? Date.now()
  const ms = end - startTime
  if (ms < 1000) return null
  const seconds = Math.round(ms / 1000)
  if (seconds < 60) return `${seconds}s`
  const minutes = Math.floor(seconds / 60)
  const remaining = seconds % 60
  return `${minutes}m ${remaining}s`
}

export default function ActivityItem({ item }: ActivityItemProps) {
  const isThinking = item.type === 'thinking'
  const hasThinkingContent = isThinking && item.content.length > 0
  const hasThinkingContext = !!item.thinkingContext
  const hasDiff = !!item.diffInfo
  const canExpand = hasThinkingContent || hasThinkingContext || hasDiff

  const [expanded, setExpanded] = useState(false)
  const contentRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (expanded && contentRef.current) {
      contentRef.current.scrollTop = contentRef.current.scrollHeight
    }
  }, [item.content, expanded])

  const duration = !item.isActive ? formatDuration(item.startTime, item.endTime) : null

  const thinkingDuration = isThinking && !item.isActive
    ? (duration ?? '<1s')
    : null

  const label = thinkingDuration
    ? `Thought for ${thinkingDuration}`
    : item.label

  return (
    <div className={`activity-child ${item.type} ${item.isActive ? 'active' : ''}`}>
      <button
        className="activity-child-header"
        onClick={() => canExpand && setExpanded(prev => !prev)}
        disabled={!canExpand}
      >
        <span className={`activity-child-label ${item.isActive ? 'shimmer' : ''}`}>
          {label}
        </span>
        {!item.isActive && duration && !isThinking && (
          <span className="activity-duration-badge">{duration}</span>
        )}
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
        <div className="activity-child-content" ref={contentRef}>
          {hasThinkingContent && item.content}
          {hasThinkingContext && (
            <div className="activity-thinking-context">{item.thinkingContext}</div>
          )}
          {hasDiff && item.diffInfo && (
            <FileEditWidget
              filename={item.diffInfo.filename}
              oldContent={item.diffInfo.oldContent}
              newContent={item.diffInfo.newContent}
              visible={true}
            />
          )}
        </div>
      )}
    </div>
  )
}
