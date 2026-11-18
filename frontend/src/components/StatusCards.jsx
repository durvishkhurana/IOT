import { fmtDateTime } from '../api'

function Card({ label, value, tone = 'info', sub }) {
  return (
    <div className={`card stat ${tone}`}>
      <div className="stat-label">{label}</div>
      <div className="stat-value">{value}</div>
      {sub && <div className="stat-sub">{sub}</div>}
    </div>
  )
}

export default function StatusCards({ stats }) {
  if (!stats) return <div className="cards muted">Loading status…</div>
  return (
    <section className="cards">
      <Card
        label="Device"
        value={stats.device_online ? 'Online' : 'Offline'}
        tone={stats.device_online ? 'ok' : 'bad'}
        sub={stats.last_seen ? `last heartbeat ${stats.last_seen.slice(11)}` : 'never seen'}
      />
      <Card label="Door" value={stats.door === 'open' ? 'Open' : 'Closed'} tone={stats.door === 'open' ? 'warn' : 'ok'} />
      <Card
        label="Lockdown"
        value={stats.lockdown ? 'ON' : 'Off'}
        tone={stats.lockdown ? 'bad' : 'ok'}
        sub={stats.lockdown_pending ? 'waiting for device…' : null}
      />
      <Card label="Entries today" value={stats.entries_today} tone="ok" />
      <Card
        label="Denied today"
        value={stats.denied_today}
        tone={stats.denied_today ? 'bad' : 'info'}
        sub={`${stats.lockouts_today} lockout${stats.lockouts_today === 1 ? '' : 's'}`}
      />
      <Card
        label="Last entry"
        value={stats.last_entry ? stats.last_entry.slice(11, 16) : '—'}
        sub={stats.last_entry ? `${stats.last_entry_user || 'remote'} · ${fmtDateTime(stats.last_entry).slice(0, 10)}` : null}
      />
    </section>
  )
}
