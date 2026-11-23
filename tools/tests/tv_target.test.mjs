import assert from "node:assert/strict";
import test from "node:test";
import { tvTarget, verifyTvTarget } from "../tv_target.mjs";

test("a TV address and credentials are explicit", () => {
  assert.throws(() => tvTarget(undefined, {}), /TV URL/);
  assert.throws(() => tvTarget("file:///tmp/console", {}), /HTTP/);
  assert.throws(() => tvTarget("http://user:secret@tv:47990", {}), /credentials/);
  assert.throws(() => tvTarget("http://tv:47990", {}), /WEBOS_STREAM_PASSWORD/);
  assert.deepEqual(tvTarget("http://tv:47990/", { WEBOS_STREAM_PASSWORD: "test" }), {
    url: "http://tv:47990",
    password: "test",
  });
});

test("SSH credentials stay in memory and parse errors do not expose them", () => {
  const target = tvTarget("http://tv:47990", { WEBOS_SMOKE_SSH: "root@tv" }, (command, args) => {
    assert.equal(command, "ssh");
    assert.ok(args.includes("root@tv"));
    return '{"password":"private-test"}';
  });
  assert.equal(target.password, "private-test");
  assert.throws(
    () => tvTarget("http://tv", { WEBOS_SMOKE_SSH: "root@tv" }, () => '{"password":"private-test'),
    (error) => /credentials/.test(error.message) && !error.message.includes("private-test"),
  );
});

test("integration tests require a hardware server without an existing viewer", async () => {
  const target = { url: "http://tv:47990", password: "test" };
  const response = (value) => async (url, options) => {
    assert.equal(url, target.url + "/api/status");
    assert.equal(
      options.headers.Authorization,
      "Basic " + Buffer.from("jetkvm:test").toString("base64"),
    );
    return { ok: true, json: async () => value };
  };
  await verifyTvTarget(
    target,
    response({
      application: "org.webosbrew.jetkvm",
      source: "Nyx hardware H.264",
      streaming: false,
    }),
  );
  await verifyTvTarget(
    target,
    response({
      application: "org.webosbrew.jetkvm",
      source: "LG hardware H.264",
      streaming: false,
    }),
  );
  await assert.rejects(verifyTvTarget(target, response({ application: "other" })), /webOS/);
  await assert.rejects(
    verifyTvTarget(
      target,
      response({ application: "org.webosbrew.jetkvm", source: "file replay" }),
    ),
    /hardware/,
  );
  await assert.rejects(
    verifyTvTarget(
      target,
      response({
        application: "org.webosbrew.jetkvm",
        source: "Nyx hardware H.264",
        streaming: true,
      }),
    ),
    /viewer/,
  );
});
