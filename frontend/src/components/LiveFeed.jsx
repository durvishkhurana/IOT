import { EVENT_LABELS, fmtTime, resultClass } from '../api'

// Newest first. Heartbeats are hidden here (they arrive every 30 s and would flood the feed).
export default function LiveFeed({ events }) {
  const list = events.filter((e) => e.event_type !== 'heartbeat')
  return (
    <section className="card">
      <div className="card-head">
        <h2>Live event feed</h2>
        <span className="muted small">heartbeats hidden</span>
      </div>
      <ul className="feed">
        {list.length === 0 && <li className="muted">No events yet.</li>}
        {list.map((e) => (
          <li key={e.id} className={`feed-item ${resultClass(e.result)}`}>
            <span className="feed-time">{fmtTime(e.timestamp)}</span>
            <span className="feed-main">
              <b>{EVENT_LABELS[e.event_type] || e.event_type}</b>
              {e.user_name && <> · {e.user_name}</>}
              {e.card_uid && <code className="uid">{e.card_uid}</code>}
              {e.reason && <span className="muted"> — {e.reason}</span>}
            </span>
            {e.event_type === 'access_denied' && e.failed_attempts > 0 && (
              <span className="pill bad small">fail {e.failed_attempts}/3</span>
            )}
          </li>
        ))}
      </ul>
    </section>
  )
}
