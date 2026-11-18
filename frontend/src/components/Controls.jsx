import { useState } from 'react'
import { api } from '../api'

// Remote Unlock (with confirm dialog) + Lockdown toggle.
// Both only queue a command; the ESP32 picks it up within ~3 s.
export default function Controls({ stats, onError }) {
  const [confirming, setConfirming] = useState(false)
  const [busy, setBusy] = useState(false)
  const [msg, setMsg] = useState('')

  async function send(command, text) {
    setBusy(true)
    try {
      await api('/api/commands', { method: 'POST', body: JSON.stringify({ command }) })
      setMsg(text)
      setTimeout(() => setMsg(''), 4000)
    } catch (e) {
      onError(e.message)
    } finally {
      setBusy(false)
    }
  }

  const lockdown = stats?.lockdown
  const online = stats?.device_online

  return (
    <section className="card controls">
      <h2>Controls</h2>
      {!online && <p className="warn-text small">Device is offline. Commands wait in the queue until it reconnects.</p>}

      <button className="btn primary big" disabled={busy || lockdown} onClick={() => setConfirming(true)}>
        🔓 Remote Unlock
      </button>
      {lockdown && <p className="muted small">Turn lockdown off to unlock.</p>}

      <div className="toggle-row">
        <div>
          <b>Lockdown</b>
          <div className="muted small">Reject every card until turned off</div>
        </div>
        <button
          className={`switch ${lockdown ? 'on' : ''}`}
          disabled={busy}
          aria-pressed={!!lockdown}
          onClick={() =>
            send(lockdown ? 'lockdown_off' : 'lockdown_on', lockdown ? 'Lockdown OFF sent' : 'Lockdown ON sent')
          }
        >
          <span />
        </button>
      </div>
      {stats?.pending_commands > 0 && (
        <p className="muted small">{stats.pending_commands} command(s) waiting for the device…</p>
      )}
      {msg && <p className="ok-text small">{msg}</p>}

      {confirming && (
        <div className="modal-bg" onClick={() => setConfirming(false)}>
          <div className="card modal" onClick={(e) => e.stopPropagation()}>
            <h2>Unlock the door?</h2>
            <p className="muted">The servo will open for 5 seconds. This is logged as “remote unlock”.</p>
            <div className="modal-actions">
              <button className="btn ghost" onClick={() => setConfirming(false)}>Cancel</button>
              <button
                className="btn primary"
                onClick={() => { setConfirming(false); send('unlock', 'Unlock sent, door opens within ~3 s') }}
              >
                Yes, unlock
              </button>
            </div>
          </div>
        </div>
      )}
    </section>
  )
}
