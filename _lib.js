// Shared helpers for all API routes (file starts with "_" so Vercel does not expose it as a route)
const crypto = require('crypto');

// Your Firebase project (from your web config). Can be overridden with Vercel environment variables.
const DB = (process.env.FIREBASE_DB_URL || 'https://app-registration-2f6cb-default-rtdb.firebaseio.com').replace(/\/$/, '');
const API_KEY = process.env.FIREBASE_API_KEY || 'AIzaSyC9VfyxSYHpOPDFx7IPxQoiT9DsshF3TfI';

const LOGIN_USER = process.env.LOGIN_USER || 'priya';
const LOGIN_PASS = process.env.LOGIN_PASS || 'priya@143';

// Used to sign the login cookie. Set SESSION_SECRET in Vercel for better security.
const SESSION_SECRET =
  process.env.SESSION_SECRET ||
  crypto.createHash('sha256').update('smart-light|' + LOGIN_PASS + '|' + API_KEY).digest('hex');

function sign(v) {
  return crypto.createHmac('sha256', SESSION_SECRET).update(v).digest('hex');
}

function makeToken() {
  const exp = String(Date.now() + 7 * 24 * 3600 * 1000); // 7 days
  return exp + '.' + sign(exp);
}

function verifyToken(token) {
  if (!token) return false;
  const parts = token.split('.');
  if (parts.length !== 2) return false;
  try {
    const good = sign(parts[0]);
    if (!crypto.timingSafeEqual(Buffer.from(parts[1]), Buffer.from(good))) return false;
  } catch (e) {
    return false;
  }
  return Number(parts[0]) > Date.now();
}

function getCookie(req, name) {
  const c = req.headers.cookie || '';
  const found = c.split(';').map((x) => x.trim()).find((x) => x.startsWith(name + '='));
  return found ? found.slice(name.length + 1) : null;
}

function requireAuth(req, res) {
  if (!verifyToken(getCookie(req, 'sid'))) {
    res.status(401).json({ error: 'login' });
    return false;
  }
  return true;
}

// ---- Firebase access: anonymous sign-in with the API key (no database secret needed) ----
let cached = { token: '', exp: 0 };

async function getToken(force) {
  if (!force && cached.token && Date.now() < cached.exp) return cached.token;
  const r = await fetch(`https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=${API_KEY}`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ returnSecureToken: true }),
  });
  if (!r.ok) throw new Error('firebase sign-in ' + r.status + ' (enable Anonymous sign-in in Firebase Authentication)');
  const j = await r.json();
  cached = { token: j.idToken, exp: Date.now() + (Number(j.expiresIn) || 3600) * 1000 - 5 * 60 * 1000 };
  return cached.token;
}

async function dbFetch(path, opts) {
  for (let attempt = 0; attempt < 2; attempt++) {
    const t = await getToken(attempt > 0);
    const r = await fetch(`${DB}/${path}.json?auth=${t}`, opts);
    if (r.status === 401 || r.status === 403) {
      if (attempt === 0) continue; // token may be old: sign in again and retry once
      throw new Error('database denied access (check the Rules)');
    }
    if (!r.ok) throw new Error('database ' + r.status);
    return r.json();
  }
}

function dbGet(path) {
  return dbFetch(path);
}

function dbPut(path, data) {
  return dbFetch(path, { method: 'PUT', body: JSON.stringify(data) });
}

module.exports = { makeToken, verifyToken, getCookie, requireAuth, dbGet, dbPut, LOGIN_USER, LOGIN_PASS };
