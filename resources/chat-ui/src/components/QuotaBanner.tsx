interface QuotaBannerProps {
  message: string
  showUpgrade: boolean
  onUpgrade: () => void
  onDismiss: () => void
}

function QuotaBanner({ message, showUpgrade, onUpgrade, onDismiss }: QuotaBannerProps) {
  return (
    <div className={`quota-banner ${showUpgrade ? 'quota-banner-warning' : 'quota-banner-info'}`}>
      <div className="quota-banner-content">
        <span className="quota-banner-message">{message}</span>
        {showUpgrade && (
          <button className="quota-banner-upgrade" onClick={onUpgrade}>
            Upgrade
          </button>
        )}
      </div>
      <button className="quota-banner-dismiss" onClick={onDismiss} aria-label="Dismiss">
        <svg width="14" height="14" viewBox="0 0 14 14" fill="none" xmlns="http://www.w3.org/2000/svg">
          <path d="M10.5 3.5L3.5 10.5M3.5 3.5L10.5 10.5" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
        </svg>
      </button>
    </div>
  )
}

export default QuotaBanner
