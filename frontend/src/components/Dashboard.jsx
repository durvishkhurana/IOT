import { useCallback, useEffect, useRef, useState } from 'react'
import { api } from '../api'
import StatusCards from './StatusCards'
import LiveFeed from './LiveFeed'
import Charts from './Charts'
import LogTable from './LogTable'
import Controls from './Controls'

export default function Dashboard({ user, onLogout }) {
  const [stats, setStats] = useState(null)
  const [feed, setFeed] = useState([])        // newest first
  const [daily, setDaily] = useState(null)
  const [wsState, setWsState] = useState('connecting')
  const [logVersion, setLogVersion] = useState(0) // bump to tell the log table new data exists
  const wsRef = useRef(null)

  const loadDaily = useCallback(() => api('/api/stats/daily?days=7').then(setDaily).catch(() => {}), [])

  // initial data
  useEffect(() => {
    api('/api/stats').then(setStats).catch(() => {})
    api('/api/events?limit=50').then(setFeed).catch(() => {})
    loadDaily()
  }, [loadDaily])

  // live updates over WebSocket (auto-reconnects every 3 s if the server restarts)
  useEffect(() => {
    let closedByUs = false
    let retry
    function connect() {
      const proto = location.protocol === 'https:' ? 'wss' : 'ws'
      const ws = new WebSocket(`${proto}://${location.host}/ws`)
      wsRef.current = ws
      setWsState('connecting')
      ws.onopen = () => setWsState('live')
      ws.onmessage = (msg) => {
        const data = JSON.parse(msg.data)
        if (data.stats) setStats(data.stats)
        if (data.type === 'event') {
          setFeed((f) => [data.event, ...f].slice(0, 100))
          setLogVersion((v) => v + 1)
          if (['access_granted', 'access_denied'].includes(data.event.event_type)) loadDaily()
        }
      }
      ws.onclose = (e) => {
        setWsState('offline')
        if (e.code === 4401) return onLogout() // session expired
        if (!closedByUs) retry = setTimeout(connect, 3000)
      }
    }
    connect()
    return () => { closedByUs = true; clearTimeout(retry); wsRef.current?.close() }
  }, [loadDaily, onLogout])

  async function logout() {
    await api('/api/logout', { method: 'POST' }).catch(() => {})
    onLogout()
  }

  return (
    <div className="page">
      <header className="topbar">
        <div>
          <h1>🔒 Smart Door Lock</h1>
          <span className="muted small">ESP32 · RFID + PIN · UEC640 IoT project</span>
        </div>
        <div className="topbar-right">
          <span className={`pill ${wsState === 'live' ? 'ok' : 'bad'}`}>
            {wsState === 'live' ? '● Live' : wsState === 'connecting' ? '… Connecting' : '○ Disconnected'}
          </span>
          <span className="muted small">{user}</span>
          <button className="btn ghost" onClick={logout}>Log out</button>
        </div>
      </header>

      <StatusCards stats={stats} />

      <div className="grid-2">
        <Controls stats={stats} onError={(m) => alert(m)} />
        <LiveFeed events={feed} />
      </div>

      <Charts daily={daily} />
      <LogTable version={logVersion} />
    </div>
  )
}
