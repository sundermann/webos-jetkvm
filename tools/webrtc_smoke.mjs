import { tvTarget } from "./tv_target.mjs";
import { forEachSerial, pollUntil } from "./async.mjs";
// Browser integration check against a webOS TV.
import { chromium, firefox } from "playwright";
import fs from "node:fs/promises";
const target = tvTarget(process.argv[2]);
const url = target.url;
const isFirefox = process.env.WEBOS_SMOKE_BROWSER === "firefox";
const browser = await (isFirefox
  ? firefox.launch({ headless: true })
  : chromium.launch({
      executablePath:
        process.env.CHROME_PATH || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
      headless: true,
    }));
const password = target.password;
const credentials = password ? { username: "jetkvm", password } : undefined;
const headers = password
  ? { Authorization: "Basic " + Buffer.from("jetkvm:" + password).toString("base64") }
  : {};
const getStatus = async () => (await fetch(new URL("/api/status", url), { headers })).json();
const page = await browser.newPage({
  viewport: { width: 1440, height: 1080 },
  httpCredentials: credentials,
});
const errors = [];
page.on("pageerror", (error) => errors.push(error.message));
page.on("console", (message) => {
  if (message.type() === "error") {
    errors.push(message.text());
  }
});
try {
  if (password) {
    const endpoint = new URL("/api/status", url);
    if ((await fetch(endpoint)).status !== 401) {
      throw new Error("Unauthenticated access was accepted");
    }
    if (
      (
        await fetch(endpoint, {
          headers: {
            Authorization: "Basic " + Buffer.from("jetkvm:wrong-password").toString("base64"),
          },
        })
      ).status !== 401
    ) {
      throw new Error("Wrong password was accepted");
    }
  }
  await page.goto(url);
  await page.getByRole("button", { name: "Connect", exact: true }).click();
  await page.waitForFunction(
    () => /decoded [1-9]/.test(document.getElementById("stats").textContent),
    null,
    { timeout: 30000 },
  );
  const before = await page.locator("#stats").innerText();
  await page.waitForFunction(
    (previousStats) => document.getElementById("stats").textContent !== previousStats,
    before,
  );
  const stats = await page.locator("#stats").innerText();
  if (process.env.WEBOS_SMOKE_HOME === "1") {
    if (await page.getByRole("button", { name: "TV remote", exact: true }).count()) {
      await page.getByRole("button", { name: "TV remote", exact: true }).click();
    }
    await page.getByRole("button", { name: "Home", exact: true }).click();
    if (await page.getByRole("button", { name: "TV remote", exact: true }).count()) {
      await page.getByRole("button", { name: "TV remote", exact: true }).click();
    }
  }
  const samples = [];
  const measurementStart = await getStatus();
  const sampleCount = Math.max(0, Math.ceil(Number(process.env.WEBOS_SMOKE_SECONDS || 0)));
  await forEachSerial(Array.from({ length: sampleCount }), async () => {
    const last = await page.locator("#stats").innerText();
    await page.waitForFunction(
      (previousStats) => document.getElementById("stats").textContent !== previousStats,
      last,
    );
    samples.push(await page.locator("#stats").innerText());
  });
  const statusBefore = await getStatus();
  const elapsed = (statusBefore.monotonicMs - measurementStart.monotonicMs) / 1000;
  const performance =
    elapsed >= 1
      ? {
          seconds: elapsed,
          captureFps: (statusBefore.captured - measurementStart.captured) / elapsed,
          cpuPercentOneCore:
            ((statusBefore.cpuSeconds - measurementStart.cpuSeconds) * 100) / elapsed,
        }
      : undefined;
  if (await page.getByRole("button", { name: "TV remote", exact: true }).count()) {
    await page.getByRole("button", { name: "TV remote", exact: true }).click();
  }
  await page.getByRole("button", { name: "←", exact: true }).click();
  if (await page.getByRole("button", { name: "TV remote", exact: true }).count()) {
    await page.getByRole("button", { name: "TV remote", exact: true }).click();
  }
  // Confirm the event arrived through the encrypted data channel.
  await page.waitForFunction(() => document.getElementById("stats").textContent.includes("fps"));
  let statusAfter;
  await pollUntil(
    async () => {
      statusAfter = await getStatus();
      return statusAfter.inputEvents > statusBefore.inputEvents;
    },
    { timeoutMs: 5000, message: "Remote input was not delivered" },
  );
  await page.screenshot({ path: "build/webrtc-smoke.png", fullPage: true });
  await page.getByRole("button", { name: "Disconnect", exact: true }).click();
  await page.waitForFunction(
    () => document.getElementById("status").textContent === "Disconnected",
  );
  let idle;
  await pollUntil(
    async () => {
      idle = await getStatus();
      return !idle.streaming;
    },
    { timeoutMs: 5000, message: "Encoder did not stop after disconnect" },
  );
  // Verify that reconnection gets a fresh IDR and works without restarting the server.
  await page.getByRole("button", { name: "Connect", exact: true }).click();
  await page.waitForFunction(
    () => /decoded [1-9]/.test(document.getElementById("stats").textContent),
    null,
    { timeout: 30000 },
  );
  await page.getByRole("button", { name: "Disconnect", exact: true }).click();
  const result = {
    url,
    browser: isFirefox ? "Firefox" : "Chrome",
    stats,
    samples,
    performance,
    statusBefore,
    statusAfter,
    idle,
    pageErrors: errors,
  };
  await fs.writeFile("build/webrtc-smoke.json", JSON.stringify(result, null, 2));
  console.log(JSON.stringify(result, null, 2));
  if (errors.length) {
    throw new Error("Browser errors: " + errors.join("; "));
  }
} catch (error) {
  await page.screenshot({ path: "build/webrtc-failure.png", fullPage: true }).catch(() => {});
  console.error(
    await page
      .locator("body")
      .innerText()
      .catch(() => "Page unavailable"),
  );
  console.error(await getStatus().catch(() => "Server unavailable"));
  console.error(errors);
  throw error;
} finally {
  await browser.close();
}
