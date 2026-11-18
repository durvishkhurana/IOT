import { useEffect, useState } from 'react'
import { api, EVENT_LABELS, fmtDateTime, resultClass } from '../api'

// Full event log with filters (type, date range) and CSV export.
export default function LogTable({ version }) {
  const [rows, setRows] = useState([])
  const [type, setType] = useState('')
  const [from, setFrom] = useState('')
  const [to, setTo] = useState('')
  const [limit, setLimit] = useState(200)

  useEffect(() => {
    const q = new URLSearchParams({ limit })
    if (type) q.set('type', type)
    if (from) q.set('from', from)
    if (to) q.set('to', to)
    api(`/api/events?${q}`).then(setRows).catch(() => {})
  }, [type, from, to, limit, version])

  return (
    <section className="card">
      <div className="card-head wrap">
        <h2>Event log</h2>
        <div className="filters">
          <select value={type} onChange={(e) => setType(e.target.value)}>
            <option value="">All types</option>
            {Object.entries(EVENT_LABELS).map(([k, v]) => <option key={k} value={k}>{v}</option>)}
          </select>
          <label>From <input type="date" value={from} onChange={(e) => setFrom(e.target.value)} /></label>
          <label>To <input type="date" value={to} onChange={(e) => setTo(e.target.value)} /></label>
          <select value={limit} onChange={(e) => setLimit(Number(e.target.value))}>
            {[50, 200, 1000].map((n) => <option key={n} value={n}>Last {n}</option>)}
          </select>
          {(type || from || to) && (
            <button className="btn ghost" onClick={() => { setType(''); setFrom(''); setTo('') }}>Clear</button>
          )}
          <a className="btn primary" href="/api/events/export.csv">⬇ Export CSV</a>
        </div>
      </div>
      <div className="table-wrap">
        <table>
          <thead>
            <tr><th>#</th><th>Time</th><th>Event</th><th>Card UID</th><th>User</th><th>Result</th><th>Reason</th><th>Fails</th></tr>
          </thead>
          <tbody>
            {rows.length === 0 && <tr><td colSpan="8" className="muted">No events match.</td></tr>}
            {rows.map((e) => (
              <tr key={e.id}>
                <td className="muted">{e.id}</td>
                <td className="nowrap">{fmtDateTime(e.timestamp)}</td>
                <td>{EVENT_LABELS[e.event_type] || e.event_type}</td>
                <td><code>{e.card_uid || '—'}</code></td>
                <td>{e.user_name || '—'}</td>
                <td><span className={`pill ${resultClass(e.result)}`}>{e.result}</span></td>
                <td>{e.reason || ''}</td>
                <td>{e.failed_attempts}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  )
}
