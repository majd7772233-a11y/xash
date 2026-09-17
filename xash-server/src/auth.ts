export type MagdRole = 'host' | 'client';

export interface MagdTokenPayload {
  sub: string;
  exp: number;
  role: MagdRole;
  room?: string;
}

const TOKEN_PREFIX = 'magd_token_';

function base64UrlEncodeBytes(bytes: Uint8Array): string {
  let binary = '';
  const CHUNK_SIZE = 0x8000;

  for (let offset = 0; offset < bytes.length; offset += CHUNK_SIZE) {
    const chunk = bytes.subarray(
      offset,
      Math.min(offset + CHUNK_SIZE, bytes.length),
    );

    binary += String.fromCharCode(...chunk);
  }

  return btoa(binary)
    .replace(/=/g, '')
    .replace(/\+/g, '-')
    .replace(/\//g, '_');
}

function base64UrlEncodeText(text: string): string {
  return base64UrlEncodeBytes(new TextEncoder().encode(text));
}

function base64UrlDecodeBytes(value: string): Uint8Array {
  if (!/^[A-Za-z0-9_-]*$/.test(value)) {
    throw new Error('Invalid base64url value');
  }

  const padded =
    value.replace(/-/g, '+').replace(/_/g, '/') +
    '='.repeat((4 - (value.length % 4)) % 4);

  const binary = atob(padded);
  const bytes = new Uint8Array(binary.length);

  for (let i = 0; i < binary.length; i++) {
    bytes[i] = binary.charCodeAt(i);
  }

  return bytes;
}

function base64UrlDecodeText(value: string): string {
  return new TextDecoder().decode(base64UrlDecodeBytes(value));
}

function isValidPayload(payload: unknown): payload is MagdTokenPayload {
  if (!payload || typeof payload !== 'object') {
    return false;
  }

  const p = payload as Partial<MagdTokenPayload>;

  if (
    typeof p.sub !== 'string' ||
    p.sub.length === 0 ||
    p.sub.length > 128
  ) {
    return false;
  }

  if (
    typeof p.exp !== 'number' ||
    !Number.isFinite(p.exp) ||
    p.exp <= Date.now()
  ) {
    return false;
  }

  if (p.role !== 'host' && p.role !== 'client') {
    return false;
  }

  if (
    p.room !== undefined &&
    (
      typeof p.room !== 'string' ||
      p.room.length === 0 ||
      p.room.length > 64
    )
  ) {
    return false;
  }

  return true;
}

export async function hashPassword(password: string): Promise<string> {
  const hash = await crypto.subtle.digest(
    'SHA-256',
    new TextEncoder().encode(password),
  );

  return base64UrlEncodeBytes(new Uint8Array(hash));
}

export async function generateToken(
  payload: MagdTokenPayload,
  secret: string,
): Promise<string> {
  if (!secret) {
    throw new Error('MAGD_JWT_SECRET is not configured');
  }

  if (!isValidPayload(payload)) {
    throw new Error('Invalid token payload');
  }

  const header = base64UrlEncodeText(
    JSON.stringify({
      alg: 'HS256',
      typ: 'JWT',
    }),
  );

  const body = base64UrlEncodeText(
    JSON.stringify(payload),
  );

  const signingInput = `${header}.${body}`;

  const key = await crypto.subtle.importKey(
    'raw',
    new TextEncoder().encode(secret),
    {
      name: 'HMAC',
      hash: 'SHA-256',
    },
    false,
    ['sign'],
  );

  const signature = await crypto.subtle.sign(
    'HMAC',
    key,
    new TextEncoder().encode(signingInput),
  );

  return (
    `${TOKEN_PREFIX}` +
    `${signingInput}.` +
    `${base64UrlEncodeBytes(new Uint8Array(signature))}`
  );
}

export async function verifyToken(
  token: string,
  secret: string,
): Promise<MagdTokenPayload | null> {
  if (
    !secret ||
    typeof token !== 'string' ||
    !token.startsWith(TOKEN_PREFIX)
  ) {
    return null;
  }

  /*
   * IMPORTANT:
   * TOKEN_PREFIX = "magd_token_"
   *
   * Never hard-code its length.
   */
  const rawToken = token.slice(TOKEN_PREFIX.length);

  const parts = rawToken.split('.');

  if (parts.length !== 3) {
    return null;
  }

  try {
    const [headerPart, bodyPart, signaturePart] = parts;

    const header = JSON.parse(
      base64UrlDecodeText(headerPart),
    ) as Record<string, unknown>;

    if (
      header.alg !== 'HS256' ||
      header.typ !== 'JWT'
    ) {
      return null;
    }

    const signature =
      base64UrlDecodeBytes(signaturePart);

    /*
     * HS256 produces exactly 32 bytes.
     */
    if (signature.length !== 32) {
      return null;
    }

    const key = await crypto.subtle.importKey(
      'raw',
      new TextEncoder().encode(secret),
      {
        name: 'HMAC',
        hash: 'SHA-256',
      },
      false,
      ['verify'],
    );

    const valid = await crypto.subtle.verify(
      'HMAC',
      key,
      signature,
      new TextEncoder().encode(
        `${headerPart}.${bodyPart}`,
      ),
    );

    if (!valid) {
      return null;
    }

    const payload = JSON.parse(
      base64UrlDecodeText(bodyPart),
    ) as unknown;

    if (!isValidPayload(payload)) {
      return null;
    }

    return payload;
  } catch {
    return null;
  }
}