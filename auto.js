const { requireAuth, dbGet, dbPut } = require('./_lib');

const TIME_RE = /^([01]\d|2[0-3]):[0-5]\d$/;
const DATE_RE = /^\d{4}-(0[1-9]|1[0-2])-(0[1-9]|[12]\d|3[01])$/;

function defaults() {
  const slots = [];
  for (let i = 0; i < 3; i++) slots.push({ e: 0, on: '18:00', off: '23:00', d: 127, date: '' });
  return { auto: 0, ver: 0, slots };
}

module.exports = async (req, res) => {
  if (!requireAuth(req, res)) return;
  try {
    if (req.method === 'GET') {
      const s = await dbGet('light/schedule');
      res.setHeader('Cache-Control', 'no-store');
      if (!s || !Array.isArray(s.slots)) return res.status(200).json(defaults());
      // fill missing values so the page always gets 3 complete slots
      const base = defaults();
      const slots = base.slots.map((b, i) => Object.assign({}, b, s.slots[i] || {}));
      slots.forEach((x) => { if (x.date === undefined || x.date === null) x.date = ''; });
      return res.status(200).json({ auto: Number(s.auto) === 1 ? 1 : 0, ver: s.ver || 0, slots });
    }

    if (req.method === 'POST') {
      const b = req.body || {};
      if (!Array.isArray(b.slots) || b.slots.length !== 3) {
        return res.status(400).json({ error: 'need 3 slots' });
      }
      const slots = [];
      for (const s of b.slots) {
        const on = String(s.on || '');
        const off = String(s.off || '');
        const date = String(s.date || '');
        const d = Number(s.d);
        if (!TIME_RE.test(on) || !TIME_RE.test(off)) return res.status(400).json({ error: 'bad time' });
        if (date && !DATE_RE.test(date)) return res.status(400).json({ error: 'bad date' });
        if (!Number.isInteger(d) || d < 0 || d > 127) return res.status(400).json({ error: 'bad days' });
        slots.push({ e: Number(s.e) === 1 ? 1 : 0, on, off, d, date });
      }
      const data = { auto: Number(b.auto) === 1 ? 1 : 0, ver: Math.floor(Date.now() / 1000), slots };
      await dbPut('light/schedule', data);
      return res.status(200).json({ ok: true });
    }

    res.status(405).json({ error: 'method' });
  } catch (e) {
    res.status(500).json({ error: 'database error' });
  }
};
