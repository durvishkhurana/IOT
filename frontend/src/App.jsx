import { useEffect, useState } from 'react'
import { api } from './api'
import Login from './components/Login'
import Dashboard from './components/Dashboard'

export default function App() {
  const [user, setUser] = useState(undefined) // undefined = still checking

  useEffect(() => {
    api('/api/me').then((r) => setUser(r.user)).catch(() => setUser(null))
  }, [])

  if (user === undefined) return <div className="center muted">Loading…</div>
  if (!user) return <Login onLogin={setUser} />
  return <Dashboard user={user} onLogout={() => setUser(null)} />
}
