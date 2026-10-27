const { requireAuth, dbPut } = require('./_lib');

module.exports = async (req, res) => {
  if (req.method !== 'POST') return res.status(405).json({ error: 'POST only' });
  if (!requireAuth(req, res)) return;
  const body = req.body || {};
  const v = Number(body.state) === 1 ? 1 : 0;
  try {
    // id changes on every click so the ESP8266 knows it is a new command
    await dbPut('light/cmd', { v, id: Date.now() % 2000000000 });
    res.status(200).json({ ok: true, state: v });
  } catch (e) {
    res.status(500).json({ error: 'database error' });
  }
};
