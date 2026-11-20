import { statSync } from "node:fs";
import { spawnSync } from "node:child_process";

const serialPort = process.env.M5_SERIAL_PORT || "/dev/cu.usbmodem1101";

function command(name, args = ["--version"], required = true) {
  const result = spawnSync(name, args, { encoding: "utf8", timeout: 5_000 });
  const output = `${result.stdout || ""}${result.stderr || ""}`.trim().split("\n")[0];
  return {
    name,
    ok: result.status === 0,
    required,
    detail:
      result.status === 0
        ? output
        : result.error?.code === "ETIMEDOUT"
          ? "timed out while resolving the command"
          : "not found on PATH",
  };
}

function executable(name, required = true) {
  const result = spawnSync("which", [name], { encoding: "utf8", timeout: 5_000 });
  return {
    name,
    ok: result.status === 0,
    required,
    detail: result.status === 0 ? result.stdout.trim() : "not found on PATH",
  };
}

function file(name, path, required = true) {
  try {
    const kind = statSync(path).isCharacterDevice() ? "character device" : "path";
    return { name, ok: true, required, detail: `${path} (${kind})` };
  } catch {
    return { name, ok: false, required, detail: `${path} is unavailable` };
  }
}

const checks = [
  command("node", ["--version"]),
  executable("pnpm"),
  command("agora", ["version"]),
  command("python3", ["--version"], false),
  command("idf.py", ["--version"]),
  file("M5StickS3 serial", serialPort),
];

for (const check of checks) {
  const marker = check.ok ? "ok" : check.required ? "missing" : "optional";
  console.log(`[${marker}] ${check.name}: ${check.detail}`);
}

if (checks.some((check) => check.required && !check.ok)) {
  process.exitCode = 1;
}
