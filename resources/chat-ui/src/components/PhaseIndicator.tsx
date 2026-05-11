import type { PhaseIndicatorState } from '../vite-env.d'

interface PhaseIndicatorProps {
  phase: PhaseIndicatorState | null
}

export default function PhaseIndicator({ phase }: PhaseIndicatorProps) {
  if (!phase) return null

  return (
    <div className="phase-indicator" style={{ backgroundColor: phase.color }}>
      <span className="phase-indicator-label">{phase.label}</span>
    </div>
  )
}
