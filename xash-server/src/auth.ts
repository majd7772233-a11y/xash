export type MagdRole = 'host' | 'client';

export interface MagdTokenPayload {
  sub: string;
  exp: number;
  role: MagdRole;
  room: string;
}

const TOKEN_PREFIX = 'magd_token_';

function base64UrlEncodeBytes(bytes: Uint8Array): string {
  let binary = '';
  const CHUNK_SIZE = 0x8000;

  for (let offset = 0; offset < bytes.length; offset += CHUNK_SIZE) {
    binary += String.fromCharCode(
      ...bytes.subarray(
        offset,
        Math.min(offset + CHUNK_SIZE, bytes.length),
      ),
    );
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
    throw new Error('Invalid base64url');
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

/*
 * TypeScript 7 + newer Node type definitions can represent Uint8Array
 * as being backed by ArrayBufferLike instead of a guaranteed ArrayBuffer.
 *
 * Web Crypto accepts an ArrayBuffer safely, so make an explicit copy.
 */
function toArrayBuffer(bytes: Uint8Array): ArrayBuffer {
  const copy = new Uint8Array(bytes.byteLength);
  copy.set(bytes);
  return copy.buffer;
}

function base64UrlDecodeText(value: string): string {
  return new TextDecoder().decode(base64UrlDecodeBytes(value));
}

function validPayload(value: unknown): value is MagdTokenPayload {
  if (!value || typeof value !== 'object') return false;

  const p = value as Partial<MagdTokenPayload>;

  return (
    typeof p.sub === 'string' &&
    p.sub.length > 0 &&
    p.sub.length <= 128 &&
    typeof p.exp === 'number' &&
    Number.isFinite(p.exp) &&
    p.exp > Date.now() &&
    (p.role === 'host' || p.role === 'client') &&
    typeof p.room === 'string' &&
    /^[A-Z0-9_-]{3,64}$/.test(p.room)
  );
}

export async function hashPassword(password: string): Promise<string> {
  const digest = await crypto.subtle.digest(
    'SHA-256',
    new TextEncoder().encode(password),
  );

  return base64UrlEncodeBytes(new Uint8Array(digest));
}

export async function generateToken(
  payload: MagdTokenPayload,
  secret: string,
): Promise<string> {
  if (!secret) {
    throw new Error('MAGD_JWT_SECRET is not configured');
  }

  if (!validPayload(payload)) {
    throw new Error('Invalid token payload');
  }

  const header = base64UrlEncodeText(
    JSON.stringify({
      alg: 'HS256',
      typ: 'JWT',
    }),
  );

  const body = base64UrlEncodeText(JSON.stringify(payload));
  const input = `${header}.${body}`;

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
    new TextEncoder().encode(input),
  );

  return (
    `${TOKEN_PREFIX}${input}.` +
    base64UrlEncodeBytes(new Uint8Array(signature))
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

  const parts = token.slice(TOKEN_PREFIX.length).split('.');

  if (parts.length !== 3) {
    return null;
  }

  try {
    const [headerPart, bodyPart, signaturePart] = parts;

    const header = JSON.parse(
      base64UrlDecodeText(headerPart),
    ) as Record<string, unknown>;

    if (header.alg !== 'HS256' || header.typ !== 'JWT') {
      return null;
    }

    const signature = base64UrlDecodeBytes(signaturePart);

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
      toArrayBuffer(signature),
      new TextEncoder().encode(`${headerPart}.${bodyPart}`),
    );

    if (!valid) {
      return null;
    }

    const payload = JSON.parse(
      base64UrlDecodeText(bodyPart),
    ) as unknown;

    return validPayload(payload) ? payload : null;
  } catch {
    return null;
  }
}
