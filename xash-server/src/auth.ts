export async function hashPassword(password: string): Promise<string> {
  const encoder = new TextEncoder();
  const data = encoder.encode(password);
  const hashBuffer = await crypto.subtle.digest('SHA-256', data);
  const hashArray = Array.from(new Uint8Array(hashBuffer));
  return hashArray.map(b => b.toString(16).padStart(2, '0')).join('');
}

export async function generateToken(payload: { sub: string; exp: number }, secretStr = process?.env?.JWT_SECRET || 'magd_default_secret'): Promise<string> {
  const encoder = new TextEncoder();
  const keyData = encoder.encode(secretStr);
  const key = await crypto.subtle.importKey('raw', keyData, { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);

  const header = JSON.stringify({ alg: 'HS256', typ: 'JWT' });
  const body = JSON.stringify(payload);

  const b64Header = btoa(header).replace(/=/g, '').replace(/\+/g, '-').replace(/\//g, '_');
  const b64Body = btoa(body).replace(/=/g, '').replace(/\+/g, '-').replace(/\//g, '_');

  const dataToSign = encoder.encode(`${b64Header}.${b64Body}`);
  const signature = await crypto.subtle.sign('HMAC', key, dataToSign);
  const b64Signature = btoa(String.fromCharCode(...new Uint8Array(signature)))
    .replace(/=/g, '')
    .replace(/\+/g, '-')
    .replace(/\//g, '_');

  return `magd_token_${b64Header}.${b64Body}.${b64Signature}`;
}

export async function verifyToken(token: string, secretStr = process?.env?.JWT_SECRET || 'magd_default_secret'): Promise<{ sub: string; exp: number } | null> {
  if (!token || !token.startsWith('magd_token_')) return null;

  const rawToken = token.replace('magd_token_', '');
  const parts = rawToken.split('.');
  if (parts.length !== 3) return null;

  const [b64Header, b64Body, b64Signature] = parts;
  const encoder = new TextEncoder();
  const keyData = encoder.encode(secretStr);
  const key = await crypto.subtle.importKey('raw', keyData, { name: 'HMAC', hash: 'SHA-256' }, false, ['verify']);

  const dataToVerify = encoder.encode(`${b64Header}.${b64Body}`);

  try {
    const binarySignature = new Uint8Array(
      atob(b64Signature.replace(/-/g, '+').replace(/_/g, '/'))
        .split('')
        .map(c => c.charCodeAt(0))
    );

    const isValid = await crypto.subtle.verify('HMAC', key, binarySignature, dataToVerify);
    if (!isValid) return null;

    const payload = JSON.parse(atob(b64Body.replace(/-/g, '+').replace(/_/g, '/')));
    if (payload.exp && Date.now() > payload.exp) return null;

    return payload;
  } catch {
    return null;
  }
}
