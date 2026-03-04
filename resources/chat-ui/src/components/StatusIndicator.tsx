interface StatusIndicatorProps {
  text: string
}

export default function StatusIndicator({ text }: StatusIndicatorProps) {
  return (
    <div className="status-indicator">
      <span className="status-text">{text}</span>
    </div>
  )
}
