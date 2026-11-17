import assert from "node:assert/strict";
import test from "node:test";
import { forEachSerial, pollUntil } from "../async.mjs";

test("stateful operations run in order without overlap", async () => {
  const events = [];
  let release;
  const gate = new Promise((resolve) => {
    release = resolve;
  });
  const result = forEachSerial([1, 2], async (value) => {
    events.push(`start ${value}`);
    if (value === 1) {
      await gate;
    }
    events.push(`end ${value}`);
  });
  await Promise.resolve();
  assert.deepEqual(events, ["start 1"]);
  release();
  await result;
  assert.deepEqual(events, ["start 1", "end 1", "start 2", "end 2"]);
});

test("a failed operation prevents subsequent state changes", async () => {
  const visited = [];
  await assert.rejects(
    forEachSerial([1, 2, 3], (value) => {
      visited.push(value);
      if (value === 2) {
        throw new Error("operation failed");
      }
    }),
    /operation failed/,
  );
  assert.deepEqual(visited, [1, 2]);
  await forEachSerial([], () => {
    assert.fail("Empty sequence invoked an operation");
  });
});

test("polling awaits each condition and stops at success", async () => {
  let active = 0;
  let calls = 0;
  await pollUntil(
    async () => {
      assert.equal(++active, 1);
      await new Promise((resolve) => setTimeout(resolve, 1));
      --active;
      return ++calls === 3;
    },
    { timeoutMs: 1000, intervalMs: 1 },
  );
  assert.equal(calls, 3);
});

test("polling preserves failures and reports its deadline", async () => {
  const failure = new Error("condition failed");
  await assert.rejects(
    pollUntil(() => {
      throw failure;
    }),
    (error) => error === failure,
  );
  await assert.rejects(
    pollUntil(() => false, { timeoutMs: 5, intervalMs: 1, message: "not ready" }),
    /not ready/,
  );
  await pollUntil(() => true, { timeoutMs: 0 });
});
