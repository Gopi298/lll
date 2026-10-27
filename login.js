const { makeToken, LOGIN_USER, LOGIN_PASS } = require('./_lib');

module.exports = (req, res) => {
  if (req.method !== 'POST') return res.status(405).json({ ok: false });
  const { user, pass } = req.body || {};
  if (user === LOGIN_USER && pass === LOGIN_PASS) {
    res.setHeader('Set-Cookie', `sid=${makeToken()}; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=604800`);
    return res.status(200).json({ ok: true });
  }
  return res.status(401).json({ ok: false });
};
