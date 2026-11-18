// Small wrapper around fetch(). `credentials: 'include'` sends the login cookie.
export async function api(path, options = {}) {
  const res = await fetch(path, {
    credentials: 'include',
    headers: { 'Content-Type': 'application/json' },
    ...options,
  })
  if (!res.ok) {
    let msg = res.statusText
    try { msg = (await res.json()).detail || msg } catch { /* not JSON */ }
    const err = new Error(msg)
    err.status = res.status
    throw err
  }
  return res.json()
}

// Human-friendly labels for event types
export const EVENT_LABELS = {
  card_scanned: 'Card scanned',
  access_granted: 'Access granted',
  access_denied: 'Access denied',
  lockout_started: 'Lockout started',
  lockout_ended: 'Lockout ended',
  door_opened: 'Door opened',
  door_closed: 'Door closed',
  entry_detected: 'Entry detected',
  unauthorized_movement: 'Unauthorized movement',
  device_online: 'Device online',
  heartbeat: 'Heartbeat',
}

// colour class: green = granted, red = denied/alert, grey = info
export const resultClass = (result) =>
  result === 'granted' ? 'ok' : result === 'denied' || result === 'alert' ? 'bad' : 'info'

export const fmtTime = (iso) => (iso ? iso.slice(11, 19) : '—')
export const fmtDateTime = (iso) => (iso ? iso.replace('T', ' ') : '—')
