// Stateful test steps share a single viewer and must finish before the next
// operation starts. A failed step rejects the sequence and prevents later work.
export async function forEachSerial(values, operation) {
  let pending = Promise.resolve();
  for (const value of values) {
    pending = pending.then(() => operation(value));
  }
  await pending;
}

// Check immediately, then poll within a bounded deadline. Predicates never
// overlap, and their failures propagate unchanged to the test's caller.
export async function pollUntil(
  condition,
  { timeoutMs = 5000, intervalMs = 100, message = "Condition did not become ready" } = {},
) {
  const deadline = Date.now() + timeoutMs;
  const attempt = async () => {
    if (await condition()) {
      return;
    }
    const remaining = deadline - Date.now();
    if (remaining <= 0) {
      throw new Error(message);
    }
    await new Promise((resolve) => setTimeout(resolve, Math.min(intervalMs, remaining)));
    if (Date.now() >= deadline) {
      throw new Error(message);
    }
    return attempt();
  };
  await attempt();
}
