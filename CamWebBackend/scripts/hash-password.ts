import { hashPassword } from "../src/auth.ts";

// Usage: npm run hash-password -- "the password"     (without the password on the command line, it is asked for)
let password = process.argv[2];

if (!password) {
  const { createInterface } = await import("node:readline/promises");
  const rl = createInterface({ input: process.stdin, output: process.stdout });
  password = await rl.question("Password: ");
  rl.close();
}

if (!password || password.length < 8) {
  console.error("Use a password with at least 8 characters.");
  process.exit(1);
}

console.log("\nPut this line into .env:\n");
console.log(`ADMIN_PASSWORD_HASH=${await hashPassword(password)}`);
