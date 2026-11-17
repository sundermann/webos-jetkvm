import { tvTarget } from "./tv_target.mjs";
import { forEachSerial, pollUntil } from "./async.mjs";
// Hardware integration: presets, TV-only presets, live decoder resize, persistence and a read-only browser console.
import { firefox } from "playwright";
import assert from "node:assert/strict";
import fs from "node:fs/promises";
import http from "node:http";
import path from "node:path";
import { execFileSync } from "node:child_process";

const base = tvTarget(process.argv[2]).url;
const ssh = process.env.WEBOS_SMOKE_SSH;
if (!ssh) {
  throw new Error("WEBOS_SMOKE_SSH is required for hardware preset tests");
}
const configPath =
  "/media/developer/apps/usr/palm/services/org.webosbrew.jetkvm.service/config.json";
const readConfig = () =>
  JSON.parse(
    execFileSync("ssh", ["-o", "LogLevel=ERROR", ssh, `cat ${configPath}`], {
      encoding: "utf8",
      stdio: ["ignore", "pipe", "pipe"],
    }),
  );
const original = readConfig();
const headers = {
  Authorization: "Basic " + Buffer.from("jetkvm:" + original.password).toString("base64"),
};
const quote = (value) => "'" + value.replaceAll("'", "'\\''") + "'";
function luna(method, parameters = {}, requireSuccess = true) {
  const command = `luna-send -n 1 luna://org.webosbrew.jetkvm.service/${method} ${quote(JSON.stringify(parameters))}`;
  const reply = JSON.parse(
    execFileSync("ssh", ["-tt", "-o", "LogLevel=ERROR", ssh, command], {
      encoding: "utf8",
      stdio: ["ignore", "pipe", "pipe"],
    }).trim(),
  );
  if (requireSuccess) {
    assert.ok(reply.returnValue, reply.errorText || "Luna request failed");
  }
  return reply;
}
const status = async () => (await fetch(base + "/api/status", { headers })).json();
async function waitFor(check, label, timeout = 15000) {
  await pollUntil(check, { timeoutMs: timeout, message: "Timed out: " + label });
}
const browser = await firefox.launch({ headless: true });
const page = await browser.newPage({
  viewport: { width: 1440, height: 1080 },
  httpCredentials: { username: "jetkvm", password: original.password },
});
const errors = [],
  results = [];
let tvServer;
let tv;
page.on("pageerror", (error) => errors.push(error.message));
page.setDefaultTimeout(15000);
async function decoded(preset) {
  await page.waitForFunction(({ width, height }) => {
    const video = document.querySelector("#screen");
    return video?.videoWidth === width && video?.videoHeight === height && video.readyState >= 2;
  }, preset);
  await waitFor(async () => {
    const value = await status();
    return (
      value.streaming &&
      value.width === preset.width &&
      value.height === preset.height &&
      !value.error
    );
  }, preset.id + " capture");
  assert.equal(await page.locator("#status").innerText(), "Connected");
}
async function restore() {
  luna("configure", { resolution: original.resolution || "1080p" });
  const saved = readConfig();
  assert.ok(saved.password === original.password, "Password changed during preset tests");
  assert.ok(saved.autostart === original.autostart, "Autostart changed during preset tests");
}
async function applyPreset(preset) {
  await tv.locator("#resolution").selectOption(preset.id);
  await tv.getByRole("button", { name: "Save settings", exact: true }).click();
  await waitFor(() => readConfig().resolution === preset.id, "TV preset saved");
  await waitFor(
    async () => await tv.getByRole("button", { name: "Save settings", exact: true }).isEnabled(),
    "TV settings request completed",
  );
}
try {
  const settings = luna("status");
  assert.deepEqual(
    settings.videoPresets.map((p) => p.id),
    ["360p", "540p", "720p", "1080p"],
  );
  await page.goto(base);
  const postHeaders = {
    ...headers,
    Origin: new URL(base).origin,
    "Content-Type": "application/json",
  };
  assert.equal((await fetch(base + "/api/video", { headers })).status, 404);
  assert.equal(
    (
      await fetch(base + "/api/video", {
        method: "POST",
        headers: postHeaders,
        body: JSON.stringify({ resolution: "720p" }),
      })
    ).status,
    404,
  );
  assert.equal(
    readConfig().resolution,
    original.resolution,
    "Removed browser API changed the preset",
  );
  for (const invalid of [
    { resolution: "2160p" },
    { resolution: 720 },
    { width: 1920, height: 1080 },
  ]) {
    assert.equal(luna("configure", invalid, false).returnValue, false);
    assert.equal(
      readConfig().resolution,
      original.resolution,
      "Invalid preset changed the configuration",
    );
  }
  await page.getByRole("button", { name: "Connect", exact: true }).click();
  await page.waitForFunction(() =>
    /decoded [1-9]/.test(document.querySelector("#stats").textContent),
  );
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  assert.equal(await page.getByRole("link", { name: "Video", exact: true }).count(), 0);
  assert.equal(await page.getByRole("combobox", { name: "Video resolution" }).count(), 0);
  assert.equal(
    await page.getByRole("button", { name: "Apply resolution", exact: true }).count(),
    0,
  );
  await page.waitForFunction(() => !document.querySelector("[data-enter],[data-leave]"));
  await page.screenshot({
    path: "build/validation/tv-only-browser-settings.png",
    fullPage: true,
    animations: "disabled",
  });
  await page.getByRole("button", { name: "Close", exact: true }).click();

  // Exercise packaged TV HTML/JS against the actual Luna service.
  tvServer = http.createServer(async (req, res) => {
    try {
      const name = new URL(req.url, "http://localhost").pathname;
      const allowed = ["/index.html", "/app.js", "/style.css", "/webOSTV.js", "/icon.png"];
      if (!allowed.includes(name)) {
        res.writeHead(404).end();
        return;
      }
      const types = {
        ".html": "text/html",
        ".js": "application/javascript",
        ".css": "text/css",
        ".png": "image/png",
      };
      res.setHeader("Content-Type", types[path.extname(name)]);
      res.end(await fs.readFile("build/package/app" + name));
    } catch {
      res.writeHead(500).end();
    }
  });
  await new Promise((resolve) => tvServer.listen(0, "127.0.0.1", resolve));
  tv = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
  tv.on("pageerror", (error) => errors.push(error.message));
  await tv.exposeFunction("tvLuna", (method, args) => luna(method, args));
  await tv.route("**/webOSTV.js", (route) =>
    route.fulfill({
      contentType: "application/javascript",
      body: "window.webOS={service:{request:function(uri,o){window.tvLuna(o.method,o.parameters||{}).then(o.onSuccess).catch(function(e){o.onFailure({errorText:e.message});});}}};",
    }),
  );
  await tv.goto(`http://127.0.0.1:${tvServer.address().port}/index.html`);
  await waitFor(
    async () => (await tv.locator("#resolution option").count()) === 4,
    "TV preset choices",
  );
  await forEachSerial(settings.videoPresets, async (preset) => {
    await applyPreset(preset);
    await decoded(preset);
    const before = await status();
    await new Promise((resolve) => setTimeout(resolve, 2200));
    const after = await status();
    const fps =
      ((after.captured - before.captured) * 1000) / (after.monotonicMs - before.monotonicMs);
    assert.ok(
      fps > preset.fps * 0.9 && fps < preset.fps * 1.1,
      `${preset.id}: unexpected frame rate ${fps}`,
    );
    assert.ok(readConfig().password === original.password, "Password changed");
    assert.ok(readConfig().autostart === original.autostart, "Autostart changed");
    results.push({
      preset: preset.id,
      width: after.width,
      height: after.height,
      fps,
      stats: await page.locator("#stats").innerText(),
    });
    console.log(JSON.stringify(results.at(-1)));
  });
  await page.getByRole("button", { name: "Disconnect", exact: true }).click();
  await waitFor(async () => !(await status()).streaming, "disconnect");
  await applyPreset(settings.videoPresets.find((p) => p.id === "720p"));
  await waitFor(
    async () => (await status()).resolution === "720p",
    "idle server reads TV settings",
  );
  await page.reload();
  await page.getByRole("button", { name: "Connect", exact: true }).click();
  await decoded(settings.videoPresets.find((p) => p.id === "720p"));
  await page.getByRole("button", { name: "Disconnect", exact: true }).click();
  await waitFor(async () => !(await status()).streaming, "disconnect before restart");
  luna("stop");
  luna("start");
  await page.reload();
  await page.getByRole("button", { name: "Connect", exact: true }).click();
  await decoded(settings.videoPresets.find((p) => p.id === "720p"));
  await tv.reload();
  await waitFor(
    async () => (await tv.locator("#resolution").inputValue()) === "720p",
    "TV preset after restart",
  );
  await tv.locator("#resolution").focus();
  await tv.keyboard.press("ArrowLeft");
  assert.equal(await tv.locator("#resolution").inputValue(), "540p");
  await tv.keyboard.press("ArrowDown");
  assert.equal(await tv.evaluate(() => document.activeElement.id), "autostart");
  await tv.keyboard.press("ArrowDown");
  assert.equal(await tv.evaluate(() => document.activeElement.id), "save");
  await tv.keyboard.press("Enter");
  await decoded(settings.videoPresets.find((p) => p.id === "540p"));
  await tv.screenshot({ path: "build/validation/tv-only-resolution-picker.png", fullPage: true });
  assert.ok(
    await tv.evaluate(() => document.documentElement.scrollHeight <= innerHeight),
    "TV setup exceeds the TV viewport",
  );
  await page.setViewportSize({ width: 390, height: 844 });
  await page.screenshot({
    path: "build/validation/tv-only-mobile-console.png",
    fullPage: true,
    animations: "disabled",
  });
  assert.equal(
    await page.evaluate(() => document.documentElement.scrollWidth > innerWidth),
    false,
    "Mobile console overflows",
  );
  assert.deepEqual(errors, []);
  await fs.writeFile(
    "build/validation/tv-only-resolution-results.json",
    JSON.stringify(
      {
        presets: results,
        reload: true,
        restart: true,
        tvRemoteNavigation: true,
        browserControlsAbsent: true,
        browserSettingsApiAbsent: true,
        errors,
      },
      null,
      2,
    ),
  );
  console.log(
    "Passed: TV-only configuration, all presets, live resize, persistence, credentials and original browser settings.",
  );
} catch (error) {
  await page
    .screenshot({ path: "build/validation/tv-only-resolution-failure.png", fullPage: true })
    .catch(() => {});
  console.error(
    await page
      .locator("body")
      .innerText()
      .catch(() => ""),
  );
  throw error;
} finally {
  try {
    await restore();
  } finally {
    await browser.close();
    if (tvServer) {
      await new Promise((resolve) => tvServer.close(resolve));
    }
  }
}
