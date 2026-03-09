interface AuthHeaderProps {
  onSignIn: () => void
}

function AuthHeader({ onSignIn }: AuthHeaderProps) {
  return (
    <div className="auth-header">
      <span className="auth-header-title">Trace</span>
      <button className="auth-header-sign-in" onClick={onSignIn}>
        Sign In
      </button>
    </div>
  )
}

export default AuthHeader
