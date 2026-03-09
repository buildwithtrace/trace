import { useState, useRef } from 'react'
import type { TodoItem } from '../vite-env.d'

interface TodoWidgetProps {
  todos: TodoItem[]
}

function StatusIcon({ status }: { status: TodoItem['status'] }) {
  if (status === 'completed') {
    return (
      <svg viewBox="0 0 24 24" width="16" height="16" fill="none">
        <circle cx="12" cy="12" r="10" fill="currentColor" opacity="0.15" />
        <circle cx="12" cy="12" r="10" stroke="currentColor" strokeWidth="1.5" />
        <polyline points="8 12.5 11 15.5 16.5 9.5" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" fill="none" />
      </svg>
    )
  }
  if (status === 'in_progress') {
    return (
      <svg viewBox="0 0 24 24" width="16" height="16" fill="none" className="todo-icon-spinning">
        <circle cx="12" cy="12" r="10" stroke="currentColor" strokeWidth="1.5" opacity="0.3" />
        <path d="M12 2 A10 10 0 0 1 22 12" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
      </svg>
    )
  }
  return (
    <svg viewBox="0 0 24 24" width="16" height="16" fill="none">
      <circle cx="12" cy="12" r="10" stroke="currentColor" strokeWidth="1.5" />
    </svg>
  )
}

export default function TodoWidget({ todos }: TodoWidgetProps) {
  const [expanded, setExpanded] = useState(false)
  const [visible, setVisible] = useState(false)
  const listRef = useRef<HTMLDivElement>(null)

  const activeTodos = todos.filter(t => t.status !== 'cancelled')
  const completedCount = activeTodos.filter(t => t.status === 'completed').length
  const totalCount = activeTodos.length
  const inProgressCount = activeTodos.filter(t => t.status === 'in_progress').length

  const handleToggle = () => {
    if (expanded) {
      setExpanded(false)
      const el = listRef.current
      if (el) {
        el.addEventListener('animationend', () => setVisible(false), { once: true })
      } else {
        setVisible(false)
      }
    } else {
      setVisible(true)
      setExpanded(true)
    }
  }

  if (activeTodos.length === 0) return null

  const currentItem = activeTodos.find(t => t.status === 'in_progress')

  return (
    <div className={`todo-widget-inline ${expanded ? 'todo-expanded' : ''}`}>
      <button
        className="todo-header-bar"
        onClick={handleToggle}
      >
        <span className="todo-indicator-wrap">
          <svg
            className="todo-dot"
            viewBox="0 0 24 24"
            width="12"
            height="12"
          >
            <circle cx="12" cy="12" r="9" fill="none" stroke="currentColor" strokeWidth="2.5" opacity="0.2" />
            <circle
              cx="12" cy="12" r="9"
              fill="none"
              stroke="currentColor"
              strokeWidth="2.5"
              strokeLinecap="round"
              strokeDasharray={`${2 * Math.PI * 9}`}
              strokeDashoffset={`${2 * Math.PI * 9 * (1 - completedCount / totalCount)}`}
              transform="rotate(-90 12 12)"
              style={{ transition: 'stroke-dashoffset 0.4s ease' }}
            />
          </svg>
          <svg
            className={`todo-chevron ${expanded ? 'expanded' : ''}`}
            viewBox="0 0 24 24"
            width="12"
            height="12"
            fill="none"
            stroke="currentColor"
            strokeWidth="2.5"
            strokeLinecap="round"
            strokeLinejoin="round"
          >
            <polyline points="9 6 15 12 9 18" />
          </svg>
        </span>
        <span className="todo-header-label">
          {currentItem ? currentItem.content : `${completedCount}/${totalCount} todos`}
        </span>
        <span className="todo-header-counter">
          {completedCount}/{totalCount}
          {inProgressCount > 0 && ` \u00b7 ${inProgressCount} running`}
        </span>
      </button>
      {visible && (
        <div ref={listRef} className={`todo-items-list ${expanded ? 'todo-slide-down' : 'todo-slide-up'}`}>
          {activeTodos.map(todo => (
            <div key={todo.id} className={`todo-item todo-status-${todo.status}`}>
              <span className="todo-item-icon">
                <StatusIcon status={todo.status} />
              </span>
              <span className="todo-item-content">{todo.content}</span>
            </div>
          ))}
        </div>
      )}
    </div>
  )
}
