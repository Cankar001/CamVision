import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";

// ---- passwords: scrypt, stored as  scrypt$N$r$p$salt$hash  (the parts in Base64)

const SCRYPT = { N: 16384, r: 8, p: 1, keyLength: 64 };

function scrypt(password: string, salt: Buffer, N: number, r: number, p: number, keyLength: number): Promise<Buffer> {
  return new Promise((resolve, reject) => {
    crypto.scrypt(password, salt, keyLength, { N, r, p, maxmem: 128 * N * r * 2 }, (error, key) => (error ? reject(error) : resolve(key)));
  });
}

export async function hashPassword(password: string): Promise<string> {
  const salt = crypto.randomBytes(16);
  const key = await scrypt(password, salt, SCRYPT.N, SCRYPT.r, SCRYPT.p, SCRYPT.keyLength);
  return ["scrypt", SCRYPT.N, SCRYPT.r, SCRYPT.p, salt.toString("base64"), key.toString("base64")].join("$");
}

export async function verifyPassword(stored: string, password: string): Promise<boolean> {
  const parts = stored.split("$");
  if (parts.length !== 6 || parts[0] !== "scrypt") {
    return false;
  }

  const [, n, r, p, salt, hash] = parts as [string, string, string, string, string, string];
  const expected = Buffer.from(hash, "base64");
  if (expected.length === 0) {
    return false;
  }

  try {
    const key = await scrypt(password, Buffer.from(salt, "base64"), Number(n), Number(r), Number(p), expected.length);
    return crypto.timingSafeEqual(key, expected);
  } catch {
    return false;
  }
}

/** A hash, which no password matches: checked when the user name is wrong, so the answer takes as long as for a wrong password. */
const DUMMY_HASH = await hashPassword(crypto.randomBytes(16).toString("hex"));

export class LoginChecker {
  constructor(
    private readonly user: string,
    private readonly passwordHash: string,
  ) {}

  /** Whether the login is right, and (for the log, never for the visitor) whether at least the user name was. */
  async check(user: string, password: string): Promise<{ ok: boolean; userMatches: boolean }> {
    // The user name is not case sensitive (a keyboard may capitalize it), the password is.
    const userMatches = safeEqual(user.trim().toLowerCase(), this.user.toLowerCase());
    const passwordMatches = await verifyPassword(userMatches ? this.passwordHash : DUMMY_HASH, password);
    return { ok: userMatches && passwordMatches, userMatches };
  }
}

export function safeEqual(a: string, b: string): boolean {
  const ha = crypto.createHash("sha256").update(a).digest();
  const hb = crypto.createHash("sha256").update(b).digest();
  return crypto.timingSafeEqual(ha, hb);
}

// ---- signed tokens: for the login session (cookie) and the links of the live streams

/** The secret for the signatures: made on the first start and kept in the data folder (only the owner can read it). */
export function loadOrCreateSecret(dataDir: string): Buffer {
  const file = path.join(dataDir, "session-secret");
  try {
    const existing = Buffer.from(fs.readFileSync(file, "utf8").trim(), "hex");
    if (existing.length >= 32) {
      return existing;
    }
  } catch {
    // none yet
  }

  const secret = crypto.randomBytes(32);
  fs.mkdirSync(dataDir, { recursive: true });
  fs.writeFileSync(file, secret.toString("hex") + "\n", { mode: 0o600 });
  return secret;
}

export interface TokenPayload {
  /** What the token is for: "session" or "stream". A token of one kind is no good for the other. */
  k: string;
  /** Expires, in seconds since 1970. */
  exp: number;
  [key: string]: string | number;
}

export class TokenSigner {
  constructor(private readonly secret: Buffer) {}

  sign(payload: TokenPayload): string {
    const body = Buffer.from(JSON.stringify(payload)).toString("base64url");
    return `${body}.${this.signature(body)}`;
  }

  /** The payload, if the signature is right, the kind matches and it has not expired. */
  verify(token: string | undefined, kind: string): TokenPayload | null {
    if (!token) {
      return null;
    }

    const [body, signature, extra] = token.split(".");
    if (!body || !signature || extra !== undefined) {
      return null;
    }

    const expected = this.signature(body);
    if (signature.length !== expected.length || !crypto.timingSafeEqual(Buffer.from(signature), Buffer.from(expected))) {
      return null;
    }

    try {
      const payload = JSON.parse(Buffer.from(body, "base64url").toString("utf8")) as TokenPayload;
      if (payload.k !== kind || typeof payload.exp !== "number" || payload.exp < Date.now() / 1000) {
        return null;
      }

      return payload;
    } catch {
      return null;
    }
  }

  private signature(body: string): string {
    return crypto.createHmac("sha256", this.secret).update(body).digest("base64url");
  }
}
