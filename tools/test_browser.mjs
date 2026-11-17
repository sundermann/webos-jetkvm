// Run integration checks against an explicitly selected webOS TV.
import { forEachSerial } from "./async.mjs";
import { tvTarget, verifyTvTarget } from "./tv_target.mjs";
import { spawn } from "node:child_process";
import { once } from "node:events";
import fs from "node:fs/promises";
import { fileURLToPath } from "node:url";

process.chdir(fileURLToPath(new URL("../", import.meta.url)));
const availableSuites = new Map([
  ["ui", [process.execPath, "tools/jetkvm_ui_test.mjs"]],
  ["protocol", [process.execPath, "tools/jetkvm_protocol_test.mjs"]],
  ["smoke", [process.execPath, "tools/webrtc_smoke.mjs"]],
  ["http", [process.env.PYTHON || "python3", "native/tests/http_server_test.py"]],
]);
const suites = process.argv.length > 3 ? process.argv.slice(3) : ["ui", "protocol", "smoke"];
if (suites.some((suite) => !availableSuites.has(suite))) {
  throw new Error("Expected suite names: ui, protocol, smoke, http");
}
const target = tvTarget(process.argv[2]);
await verifyTvTarget(target);
await fs.mkdir("build/validation", { recursive: true });
const environment = {
  ...process.env,
  WEBOS_STREAM_PASSWORD: target.password,
  WEBOS_SMOKE_BROWSER: "firefox",
};
await forEachSerial(suites, async (suite) => {
  const [command, script] = availableSuites.get(suite);
  const child = spawn(command, [script, target.url], { env: environment, stdio: "inherit" });
  const [code, signal] = await once(child, "exit");
  if (code !== 0) {
    throw new Error(`${suite} failed (${signal || code})`);
  }
});
