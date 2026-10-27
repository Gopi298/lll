module.exports = (req, res) => {
  res.setHeader('Set-Cookie', 'sid=; Path=/; HttpOnly; Secure; SameSite=Lax; Max-Age=0');
  res.status(200).json({ ok: true });
};
