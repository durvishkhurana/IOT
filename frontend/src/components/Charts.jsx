import {
  Bar, BarChart, CartesianGrid, Cell, Legend, Pie, PieChart, ResponsiveContainer, Tooltip, XAxis, YAxis,
} from 'recharts'

const GREEN = '#22c55e'
const RED = '#ef4444'
const PIE_COLORS = ['#ef4444', '#f97316', '#eab308', '#a855f7', '#64748b']
const tooltipStyle = { background: '#111827', border: '1px solid #334155', borderRadius: 8, color: '#e5e7eb' }

export default function Charts({ daily }) {
  const days = (daily?.daily || []).map((d) => ({ ...d, label: d.date.slice(5) })) // MM-DD
  const reasons = daily?.denial_reasons || []
  return (
    <div className="grid-chart">
      <section className="card">
        <h2>Granted vs denied, last 7 days</h2>
        <div className="chart">
          <ResponsiveContainer width="100%" height="100%">
            <BarChart data={days}>
              <CartesianGrid stroke="#1f2937" vertical={false} />
              <XAxis dataKey="label" stroke="#94a3b8" fontSize={12} />
              <YAxis allowDecimals={false} stroke="#94a3b8" fontSize={12} />
              <Tooltip contentStyle={tooltipStyle} cursor={{ fill: '#1f2937' }} />
              <Legend />
              <Bar dataKey="granted" name="Granted" fill={GREEN} radius={[4, 4, 0, 0]} />
              <Bar dataKey="denied" name="Denied" fill={RED} radius={[4, 4, 0, 0]} />
            </BarChart>
          </ResponsiveContainer>
        </div>
      </section>
      <section className="card">
        <h2>Denial reasons</h2>
        <div className="chart">
          {reasons.length === 0 ? (
            <div className="center muted">No denials 🎉</div>
          ) : (
            <ResponsiveContainer width="100%" height="100%">
              <PieChart>
                <Pie data={reasons} dataKey="count" nameKey="reason" innerRadius="45%" outerRadius="75%" paddingAngle={2}>
                  {reasons.map((r, i) => <Cell key={r.reason} fill={PIE_COLORS[i % PIE_COLORS.length]} />)}
                </Pie>
                <Tooltip contentStyle={tooltipStyle} itemStyle={{ color: '#e5e7eb' }} />
                <Legend />
              </PieChart>
            </ResponsiveContainer>
          )}
        </div>
      </section>
    </div>
  )
}
