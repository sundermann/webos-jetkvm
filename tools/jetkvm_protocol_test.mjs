import { tvTarget } from "./tv_target.mjs";
// Independent browser peer exercising the pinned JetKVM wire protocol.
import { firefox } from "playwright";
import assert from "node:assert/strict";
import fs from "node:fs/promises";
const target = tvTarget(process.argv[2]);
const base = target.url;
const browser = await firefox.launch({ headless: true });
try {
  const page = await browser.newPage(
    target.password ? { httpCredentials: { username: "jetkvm", password: target.password } } : {},
  );
  await page.goto(base);
  const result = await page.evaluate(async (serviceUrl) => {
    const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
    function check(value, message) {
      if (!value) {
        throw new Error(message);
      }
    }
    async function until(condition, message, timeout = 10000) {
      const end = Date.now() + timeout;
      const attempt = async () => {
        if (Date.now() >= end) {
          throw new Error("Timed out: " + message);
        }
        if (await condition()) {
          return;
        }
        await wait(25);
        return attempt();
      };
      await attempt();
    }
    async function makePeer() {
      const pc = new RTCPeerConnection({ iceServers: [] });
      pc.addTransceiver("video", { direction: "recvonly" });
      pc.addTransceiver("audio", { direction: "recvonly" });
      const rpc = pc.createDataChannel("rpc");
      const hid = pc.createDataChannel("hidrpc");
      hid.binaryType = "arraybuffer";
      const ordered = pc.createDataChannel("hidrpc-unreliable-ordered", {
        ordered: true,
        maxRetransmits: 0,
      });
      const unordered = pc.createDataChannel("hidrpc-unreliable-nonordered", {
        ordered: false,
        maxRetransmits: 0,
      });
      const ws = new WebSocket(serviceUrl.replace(/^http/, "ws") + "/webrtc/signaling/client");
      const replies = new Map(),
        binary = [],
        signals = [],
        candidates = [];
      let description = false,
        metadata = false,
        id = 0,
        failure;
      const video = document.createElement("video");
      video.muted = true;
      video.autoplay = true;
      document.body.append(video);
      pc.ontrack = (e) => {
        video.srcObject = e.streams[0] || new MediaStream([e.track]);
      };
      hid.onmessage = (e) => binary.push(Array.from(new Uint8Array(e.data)));
      rpc.onmessage = (e) => {
        const j = JSON.parse(e.data);
        if (j.id !== undefined) {
          replies.get(String(j.id))?.(j);
          replies.delete(String(j.id));
        }
      };
      pc.onicecandidate = ({ candidate }) => {
        if (candidate?.candidate && ws.readyState === 1) {
          ws.send(JSON.stringify({ type: "new-ice-candidate", data: candidate }));
        }
      };
      ws.onmessage = async (e) => {
        if (e.data === "pong") {
          return;
        }
        try {
          const m = JSON.parse(e.data);
          signals.push(m.type);
          if (m.type === "device-metadata") {
            metadata = !!m.data.deviceVersion;
            await pc.setLocalDescription(await pc.createOffer());
            ws.send(
              JSON.stringify({
                type: "offer",
                data: { sd: btoa(JSON.stringify(pc.localDescription)) },
              }),
            );
          } else if (m.type === "answer") {
            await pc.setRemoteDescription(JSON.parse(atob(m.data)));
            description = true;
            // RTCPeerConnection queues these operations in call order.
            await Promise.all(
              candidates.splice(0).map((candidate) => pc.addIceCandidate(candidate)),
            );
          } else if (m.type === "new-ice-candidate") {
            if (description) {
              await pc.addIceCandidate(m.data);
            } else {
              candidates.push(m.data);
            }
          }
        } catch (error) {
          failure = String(error);
        }
      };
      const call = (method, params = {}, requestId = ++id) =>
        new Promise((resolve, reject) => {
          const timer = setTimeout(() => {
            replies.delete(String(requestId));
            reject(new Error("RPC timeout: " + method));
          }, 5000);
          replies.set(String(requestId), (value) => {
            clearTimeout(timer);
            resolve(value);
          });
          rpc.send(JSON.stringify({ jsonrpc: "2.0", id: requestId, method, params }));
        });
      return {
        pc,
        rpc,
        hid,
        ordered,
        unordered,
        ws,
        binary,
        signals,
        call,
        video,
        async ready() {
          await until(() => {
            if (failure) {
              throw new Error(failure);
            }
            return rpc.readyState === "open" && hid.readyState === "open";
          }, "JetKVM channels");
          check(metadata, "device metadata");
        },
        async decoded() {
          await until(async () => {
            const stats = await pc.getStats();
            return [...stats.values()].some(
              (s) => s.type === "inbound-rtp" && s.kind === "video" && s.framesDecoded > 3,
            );
          }, "H.264 decoding");
        },
        close() {
          for (const c of [rpc, hid, ordered, unordered]) {
            c.close();
          }
          ws.close();
          pc.close();
          video.remove();
        },
      };
    }
    const first = await makePeer();
    try {
      await first.ready();
      await first.decoded();
      check(
        first.signals.includes("answer") && first.signals.includes("new-ice-candidate"),
        "answer and trickle ICE",
      );
      check((await first.call("ping", {}, "string-id")).result === "pong", "JSON-RPC string IDs");
      check(
        (await first.call("unsupportedMethod")).error.code === -32601,
        "unsupported method error",
      );
      first.hid.send(new Uint8Array([1, 1]));
      await until(() => first.binary.some((d) => d[0] === 1 && d[1] === 1), "HID v1 handshake");
      first.hid.send(new Uint8Array([5, 0x4f, 1]));
      const keepalive = setInterval(() => {
        if (first.hid.readyState === "open") {
          first.hid.send(new Uint8Array([9]));
        }
      }, 50);
      try {
        await until(
          async () => (await first.call("getKeyDownState")).result.keys.includes(0x4f),
          "binary keypress",
        );
        const before = (await (await fetch(serviceUrl + "/api/status")).json()).inputEvents;
        const rejected = await makePeer();
        await until(() => rejected.ws.readyState === WebSocket.CLOSED, "second viewer rejected");
        rejected.close();
        check(
          (await first.call("ping")).result === "pong",
          "busy connection preserves first viewer",
        );
        check(
          (await (await fetch(serviceUrl + "/api/status")).json()).inputEvents === before,
          "rejected viewer must not release first viewer input",
        );
      } finally {
        clearInterval(keepalive);
      }
      await wait(800);
      check(
        !(await first.call("getKeyDownState")).result.keys.includes(0x4f),
        "missing keepalive releases key",
      );
      // A for 30 ms followed by a full release, encoded by the stock HID v1 schema.
      const start = first.binary.length;
      first.hid.send(
        new Uint8Array([7, 0, 0, 0, 0, 2, 0, 4, 0, 0, 0, 0, 0, 0, 30, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
      );
      await until(
        () => first.binary.slice(start).some((d) => d[0] === 0x34 && d[1] === 1),
        "macro start",
      );
      await until(
        () => first.binary.slice(start).some((d) => d[0] === 0x34 && d[1] === 0),
        "macro completion",
      );
      first.hid.send(new Uint8Array([3, 0]));
      check((await first.call("ping")).result === "pong", "malformed HID cannot kill session");
      await first.call("wheelReport", { wheelY: -1, wheelX: 0 });
      check((await first.call("getAudioConfig")).result.enabled === false, "audio disabled");
      check(
        (await first.call("setVideoCodecPreference", { codec: "H265" })).error.code === -32601,
        "encoder controls stay in TV app",
      );
      first.ws.send("ping");
      await wait(17000);
      check(
        (await first.call("ping")).result === "pong",
        "idle session survives former 15 second custom heartbeat",
      );
    } finally {
      first.close();
    }
    await until(
      async () => !(await (await fetch(serviceUrl + "/api/status")).json()).streaming,
      "capture stops after disconnect",
    );
    const second = await makePeer();
    try {
      await second.ready();
      await second.decoded();
      check((await second.call("ping")).result === "pong", "reconnection");
    } finally {
      second.close();
    }
    await Promise.all(
      ["offer", "answer", "disconnect", "video"].map(async (endpoint) => {
        check(
          (
            await fetch(serviceUrl + "/api/" + endpoint, {
              method: "POST",
              headers: { "Content-Type": "application/json" },
              body: "{}",
            })
          ).status === 404,
          "removed custom endpoint " + endpoint,
        );
      }),
    );
    return {
      signaling: "jetkvm-websocket",
      browserOffer: true,
      trickleICE: true,
      videoDecoded: true,
      jsonRpc: true,
      hidV1: true,
      macroAcknowledgements: true,
      keyWatchdog: true,
      busyViewerPreserved: true,
      idleConnection: true,
      reconnection: true,
      customEndpointsRemoved: true,
    };
  }, base);
  assert.equal(result.videoDecoded, true);
  await fs.mkdir("build/validation", { recursive: true });
  await fs.writeFile(
    "build/validation/jetkvm-protocol-browser.json",
    JSON.stringify(result, null, 2) + "\n",
  );
  console.log(JSON.stringify(result));
} finally {
  await browser.close();
}
