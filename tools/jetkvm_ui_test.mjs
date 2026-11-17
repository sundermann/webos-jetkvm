import { tvTarget } from "./tv_target.mjs";
import { pollUntil } from "./async.mjs";
// Integration checks for the actual JetKVM components and webOS adapter.
import { firefox } from "playwright";
import assert from "node:assert/strict";
const target = tvTarget(process.argv[2]);
const base = target.url;
const browser = await firefox.launch({ headless: true });
const page = await browser.newPage({
  viewport: { width: 1440, height: 1000 },
  ...(target.password
    ? { httpCredentials: { username: "jetkvm", password: target.password } }
    : {}),
});
page.setDefaultTimeout(10000);
const errors = [];
let reloading = false;
let lastTransportError;
let expectedReloadAborts = 0;
// Firefox reports User-Initiated Abort on channels belonging to the document
// being unloaded. Record the actual RTC error; keep active-session failures fatal.
await page.addInitScript(() => {
  window.__jetkvmSockets = [];
  window.__jetkvmHandshakes = 0;
  const Socket = window.WebSocket;
  window.WebSocket = class extends Socket {
    constructor(...args) {
      super(...args);
      window.__jetkvmSockets.push(this);
    }
  };
  const create = RTCPeerConnection.prototype.createDataChannel;
  RTCPeerConnection.prototype.createDataChannel = function (...args) {
    const channel = create.apply(this, args);
    channel.addEventListener("message", (event) => {
      if (channel.label === "hidrpc" && event.data instanceof ArrayBuffer) {
        const bytes = new Uint8Array(event.data);
        if (bytes[0] === 1 && bytes[1] === 1) {
          window.__jetkvmHandshakes++;
        }
      }
    });
    channel.addEventListener("error", (event) =>
      console.debug(
        "JETKVM_RTC_ERROR " +
          JSON.stringify({
            state: channel.readyState,
            detail: event.error?.errorDetail,
            message: event.error?.message,
          }),
      ),
    );
    return channel;
  };
});
const macroName = "Test arrow " + Date.now();
page.on("pageerror", (error) => errors.push(error.message));
page.on("console", (message) => {
  const text = message.text();
  if (text.startsWith("JETKVM_RTC_ERROR ")) {
    lastTransportError = JSON.parse(text.slice("JETKVM_RTC_ERROR ".length));
  }
  if (message.type() !== "error") {
    return;
  }
  if (
    reloading &&
    /^Error on rpcHidChannel 'hidrpc':/.test(text) &&
    lastTransportError?.state === "closed" &&
    lastTransportError?.detail === "sctp-failure" &&
    lastTransportError?.message?.startsWith("User-Initiated Abort")
  ) {
    expectedReloadAborts++;
    return;
  }
  errors.push(text);
});
const statusHeaders = target.password
  ? {
      Authorization: "Basic " + Buffer.from("jetkvm:" + target.password).toString("base64"),
    }
  : {};
const status = async () => (await fetch(base + "/api/status", { headers: statusHeaders })).json();
async function inputArrived(before) {
  await pollUntil(async () => (await status()).inputEvents > before, {
    timeoutMs: 5000,
    message: "Remote input did not arrive",
  });
}

try {
  await page.goto(base);
  await page.getByRole("button", { name: "Connect", exact: true }).click();
  await page.waitForFunction(() =>
    /decoded [1-9]/.test(document.querySelector("#stats").textContent),
  );
  // Upstream connection charts, backed by real WebRTC stats.
  await page.getByRole("button", { name: "Connection Stats", exact: true }).click();
  await page.getByText("Frames per second", { exact: true }).waitFor();
  await page.screenshot({ path: "build/jetkvm-stats.png", fullPage: true });
  await page.getByRole("button", { name: "Hide", exact: true }).click();
  // Original virtual keyboard, including its client-side macro path.
  await page.getByRole("button", { name: "Virtual Keyboard", exact: true }).click();
  const beforeKey = (await status()).inputEvents;
  await page.locator(".hg-button").filter({ hasText: /^a$/ }).first().click();
  await inputArrived(beforeKey);
  await page.screenshot({ path: "build/jetkvm-keyboard.png", fullPage: true });
  await page.getByRole("button", { name: "Virtual Keyboard", exact: true }).click();
  // Original paste popover: typing in the form must not leak key presses to the TV.
  await page.getByRole("button", { name: "Paste text", exact: true }).click();
  const beforeTyping = (await status()).inputEvents;
  await page.locator("textarea").fill("Ab12");
  assert.equal((await status()).inputEvents, beforeTyping, "Form keystrokes leaked to the TV");
  console.log("Paste dialog controls:", await page.getByRole("button").allTextContents());
  await page.getByRole("button", { name: "Confirm Paste", exact: true }).click();
  await inputArrived(beforeTyping);
  // Original appearance and keyboard settings inside the port's settings shell.
  await page.getByRole("button", { name: "Settings", exact: true }).click();
  await page.getByRole("link", { name: "Appearance", exact: true }).click();
  await page.getByRole("combobox").first().selectOption("light");
  assert.equal(await page.locator("html").getAttribute("class"), "light");
  await page.getByRole("combobox").first().selectOption("dark");
  await page.getByRole("link", { name: "Keyboard", exact: true }).click();
  assert.ok(await page.getByRole("combobox").count());
  await page.screenshot({ path: "build/jetkvm-settings.png", fullPage: true });
  await page.getByRole("link", { name: "Macros", exact: true }).click();
  await page.getByRole("button", { name: /Add New Macro/i }).click();
  await page.getByPlaceholder("Macro Name", { exact: true }).fill(macroName);
  await page.getByRole("combobox").first().fill("→");
  await page.getByRole("option", { name: "→", exact: true }).click();
  await page.getByRole("button", { name: "Save Macro", exact: true }).click();
  await page.waitForURL(/\/settings\/macros\/?$/);
  await page.getByRole("dialog").getByText(macroName, { exact: true }).first().waitFor();
  await page.screenshot({ path: "build/jetkvm-macros.png", fullPage: true });
  await page.getByRole("button", { name: "Close", exact: true }).click();
  const beforeMacro = (await status()).inputEvents;
  await page.getByRole("button", { name: macroName, exact: true }).click();
  await inputArrived(beforeMacro);
  reloading = true;
  await page.reload();
  reloading = false;
  await page.getByRole("button", { name: "Connect", exact: true }).click();
  await page.waitForFunction(() =>
    /decoded [1-9]/.test(document.querySelector("#stats").textContent),
  );
  await page.getByRole("button", { name: macroName, exact: true }).waitFor();
  // Modal close restores normal physical keyboard control.
  const beforePhysical = (await status()).inputEvents;
  await page.locator("#screen").focus();
  await page.keyboard.press("ArrowRight");
  await inputArrived(beforePhysical);
  // Force a signaling interruption: use the upstream automatic reconnect path.
  const handshakes = await page.evaluate(() => window.__jetkvmHandshakes);
  reloading = true;
  await page.evaluate(() => window.__jetkvmSockets.at(-1).close());
  await page.waitForFunction(
    (previous) =>
      window.__jetkvmHandshakes > previous && document.querySelector("#screen")?.readyState >= 2,
    handshakes,
    { timeout: 20000 },
  );
  reloading = false;
  const beforeRecoveryKey = (await status()).inputEvents;
  await page.locator("#screen").focus();
  await page.keyboard.press("ArrowLeft");
  await inputArrived(beforeRecoveryKey);
  await page.waitForFunction(() => {
    const video = document.getElementById("screen");
    const bounds = video.getBoundingClientRect();
    return (
      document.elementFromPoint(bounds.x + bounds.width / 2, bounds.y + bounds.height / 2) === video
    );
  });
  const bounds = await page.locator("#screen").boundingBox();
  const beforePointer = (await status()).inputEvents;
  await page.mouse.move(bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
  await page.mouse.down();
  await inputArrived(beforePointer);
  await page.waitForTimeout(150);
  const beforeExit = (await status()).inputEvents;
  await page.mouse.move(bounds.x + bounds.width / 2, 1);
  await page.waitForTimeout(150);
  assert.equal((await status()).inputEvents, beforeExit, "Captured drag moved above the video");
  await page.mouse.move(bounds.x + bounds.width / 2 + 100, 1);
  await page.waitForTimeout(150);
  assert.equal(
    (await status()).inputEvents,
    beforeExit,
    "Captured drag moved along the video edge",
  );
  await page.mouse.up();
  await page.waitForTimeout(150);
  assert.equal(
    (await status()).inputEvents,
    beforeExit + 2,
    "Outside release must only release the button",
  );
  // Firefox headless does not dispatch native mouse events outside its viewport.
  // Dispatch the captured coordinates explicitly to exercise the browser-edge guard.
  const beforeViewportExit = (await status()).inputEvents;
  await page.locator("#screen").dispatchEvent("mousemove", {
    clientX: -10,
    clientY: bounds.y + bounds.height / 2,
    buttons: 0,
  });
  await page.locator("#screen").dispatchEvent("mousemove", {
    clientX: -20,
    clientY: bounds.y + bounds.height / 2 + 100,
    buttons: 0,
  });
  await page.waitForTimeout(150);
  assert.equal(
    (await status()).inputEvents,
    beforeViewportExit,
    "Captured coordinates outside the viewport moved the TV pointer",
  );
  // The video element also contains object-contain bars. Those must not be
  // clamped into movement or clicks along the TV screen's edges.
  const bar = await page.locator("#screen").evaluate((video) => {
    const rect = video.getBoundingClientRect();
    const scale = Math.min(rect.width / video.videoWidth, rect.height / video.videoHeight);
    const gapX = (rect.width - video.videoWidth * scale) / 2;
    const gapY = (rect.height - video.videoHeight * scale) / 2;
    if (gapX > 2) {
      return { x: rect.x + gapX / 2, y: rect.y + rect.height / 2 };
    }
    if (gapY > 2) {
      return { x: rect.x + rect.width / 2, y: rect.y + gapY / 2 };
    }
    return null;
  });
  assert.ok(bar, "Desktop fixture must exercise video letterboxing");
  const beforeBar = (await status()).inputEvents;
  await page.mouse.move(bar.x, bar.y);
  await page.mouse.click(bar.x, bar.y);
  await page.mouse.wheel(0, 100);
  await page.waitForTimeout(150);
  assert.equal((await status()).inputEvents, beforeBar, "Video black bars forwarded mouse input");
  await page.mouse.move(bounds.x + bounds.width / 2, bounds.y + bounds.height / 2);
  const beforeWheel = (await status()).inputEvents;
  await page.mouse.wheel(0, 100);
  await inputArrived(beforeWheel);
  // Losing focus or leaving the browser must not replay coordinates and restart
  // webOS's pointer idle timeout. With no buttons held these are input no-ops.
  await page.waitForTimeout(150);
  const beforeLeaving = (await status()).inputEvents;
  await page.evaluate(() => {
    window.dispatchEvent(new Event("blur"));
    document.documentElement.dispatchEvent(new PointerEvent("pointerleave"));
    document.dispatchEvent(new Event("visibilitychange"));
  });
  await page.waitForTimeout(300);
  assert.equal((await status()).inputEvents, beforeLeaving, "Focus loss moved the TV pointer");
  // Fullscreen is the upstream video action.
  await page.getByRole("button", { name: "Fullscreen", exact: true }).click();
  await page.waitForFunction(() => !!document.fullscreenElement);
  await page.keyboard.press("Escape");
  // Responsive layout must keep the console and control buttons reachable.
  await page.setViewportSize({ width: 390, height: 844 });
  await page.screenshot({ path: "build/jetkvm-mobile.png", fullPage: true });
  assert.ok(await page.getByRole("button", { name: "TV remote", exact: true }).isVisible());
  assert.equal(
    await page.evaluate(() => document.documentElement.scrollWidth > window.innerWidth),
    false,
    "Mobile layout overflows",
  );
  await page.getByRole("button", { name: "Disconnect", exact: true }).click();
  await page.waitForFunction(
    () => document.querySelector("#status").textContent === "Disconnected",
  );
  assert.deepEqual(errors, []);
  console.log(
    "Expected Firefox notices during intentional transport teardown:",
    expectedReloadAborts,
  );
  console.log(
    "Passed: stats, virtual keyboard, paste, form input isolation, settings, macro creation/execution/persistence, fullscreen, physical keys, pointer/wheel, pointer idle on focus loss, automatic WebSocket recovery, mobile layout.",
  );
} catch (error) {
  console.error(await page.locator("body").innerText());
  console.error(errors);
  await page.screenshot({ path: "build/jetkvm-ui-failure.png", fullPage: true });
  throw error;
} finally {
  await browser.close();
}
