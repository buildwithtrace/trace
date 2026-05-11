import { useState, useRef, useEffect } from 'react'
import type { ChatMode, PhaseIndicatorState } from '../vite-env.d'

interface ModeDropdownProps {
  mode: ChatMode
  onModeChange: (mode: ChatMode) => void
  phaseIndicator?: PhaseIndicatorState | null
}

const MODE_LABELS: Record<ChatMode, string> = {
  agent: 'Agent',
  ask: 'Ask',
  plan: 'Plan',
}

// Mode icons - simple representations
const MODE_ICONS: Record<ChatMode, JSX.Element> = {
  agent: (
    <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
      <path d="M18 3a3 3 0 0 0-3 3v12a3 3 0 0 0 3 3 3 3 0 0 0 3-3 3 3 0 0 0-3-3H6a3 3 0 0 0-3 3 3 3 0 0 0 3 3 3 3 0 0 0 3-3V6a3 3 0 0 0-3-3 3 3 0 0 0-3 3 3 3 0 0 0 3 3h12a3 3 0 0 0 3-3 3 3 0 0 0-3-3z" />
    </svg>
  ),
  ask: (
    <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
      <circle cx="12" cy="12" r="10" />
      <path d="M9.09 9a3 3 0 0 1 5.83 1c0 2-3 3-3 3" />
      <line x1="12" y1="17" x2="12.01" y2="17" />
    </svg>
  ),
  plan: (
    <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
      <line x1="8" y1="6" x2="21" y2="6" />
      <line x1="8" y1="12" x2="21" y2="12" />
      <line x1="8" y1="18" x2="21" y2="18" />
      <line x1="3" y1="6" x2="3.01" y2="6" />
      <line x1="3" y1="12" x2="3.01" y2="12" />
      <line x1="3" y1="18" x2="3.01" y2="18" />
    </svg>
  ),
}

export default function ModeDropdown({ mode, onModeChange, phaseIndicator }: ModeDropdownProps) {
  const [isOpen, setIsOpen] = useState(false)
  const dropdownRef = useRef<HTMLDivElement>(null)

  const phaseClass = phaseIndicator && mode === 'plan'
    ? (() => {
        const label = phaseIndicator.label.toLowerCase()
        if (label.includes('research') || label.includes('clarif')) return 'mode-phase-researching'
        if (label.includes('plan') || label.includes('design')) return 'mode-phase-planning'
        if (label.includes('approv') || label.includes('review')) return 'mode-phase-approval'
        if (label.includes('execut')) return 'mode-phase-executing'
        return 'mode-phase-researching'
      })()
    : ''

  const displayLabel = phaseIndicator && mode === 'plan' && phaseClass
    ? `Plan`
    : MODE_LABELS[mode]

  const phaseSubLabel = phaseIndicator && mode === 'plan' && phaseClass
    ? phaseIndicator.label.replace(/^(Creating |Plan )*/i, '').trim()
    : null

  // Close dropdown when clicking outside
  useEffect(() => {
    const handleClickOutside = (event: MouseEvent) => {
      if (dropdownRef.current && !dropdownRef.current.contains(event.target as Node)) {
        setIsOpen(false)
      }
    }

    document.addEventListener('mousedown', handleClickOutside)
    return () => document.removeEventListener('mousedown', handleClickOutside)
  }, [])

  const handleSelect = (newMode: ChatMode) => {
    onModeChange(newMode)
    setIsOpen(false)
  }

  return (
    <div className="mode-dropdown" ref={dropdownRef}>
      <button 
        className={`mode-dropdown-button ${phaseClass}`}
        onClick={() => setIsOpen(!isOpen)}
      >
        <span className="mode-icon">{MODE_ICONS[mode]}</span>
        <span className="mode-label">{displayLabel}</span>
        {phaseSubLabel && <span className="mode-phase-label">{phaseSubLabel}</span>}
        <svg 
          className={`chevron ${isOpen ? 'open' : ''}`}
          viewBox="0 0 24 24" 
          width="12" 
          height="12" 
          fill="none"
          stroke="currentColor"
          strokeWidth="2"
          strokeLinecap="round"
          strokeLinejoin="round"
        >
          <polyline points="6 9 12 15 18 9" />
        </svg>
      </button>
      
      {isOpen && (
        <div className="mode-dropdown-menu">
          {(Object.keys(MODE_LABELS) as ChatMode[]).map((modeOption) => (
            <button
              key={modeOption}
              className={`mode-option ${mode === modeOption ? 'selected' : ''}`}
              onClick={() => handleSelect(modeOption)}
            >
              <span className="mode-icon">{MODE_ICONS[modeOption]}</span>
              <span className="mode-option-label">{MODE_LABELS[modeOption]}</span>
            </button>
          ))}
        </div>
      )}
    </div>
  )
}
