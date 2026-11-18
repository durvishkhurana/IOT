import { useState } from 'react'
import { api } from '../api'

export default function Login({ onLogin }) {
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [error, setError] = useState('')

  async function submit(e) {
    e.preventDefault()
    setError('')
    try {
      const r = await api('/api/login', { method: 'POST', body: JSON.stringify({ username, password }) })
      onLogin(r.user)
    } catch (err) {
      setError(err.message)
    }
  }

  return (
    <div className="center">
      <form className="card login" onSubmit={submit}>
        <h1>🔒 Smart Door Lock</h1>
        <p className="muted">Sign in to the dashboard</p>
        <input placeholder="Username" value={username} onChange={(e) => setUsername(e.target.value)} autoFocus />
        <input placeholder="Password" type="password" value={password} onChange={(e) => setPassword(e.target.value)} />
        {error && <div className="error">{error}</div>}
        <button className="btn primary" type="submit">Sign in</button>
      </form>
    </div>
  )
}
