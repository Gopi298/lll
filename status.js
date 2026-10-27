const { requireAuth, dbGet } = require('./_lib');

module.exports = async (req, res) => {
  if (!requireAuth(req, res)) return;
  try {
    const d = (await dbGet('light')) || {};
    const now = Math.floor(Date.now() / 1000);
    const seen = Number(d.seen) || 0;
    res.setHeader('Cache-Control', 'no-store');
    res.status(200).json({
      on: Number(d.state) === 1,
      auto: !!(d.schedule && Number(d.schedule.auto) === 1),
      online: seen > 0 && now - seen < 45,
      seen,
    });
  } catch (e) {
    res.status(500).json({ error: 'database error' });
  }
};
