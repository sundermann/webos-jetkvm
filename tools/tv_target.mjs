import { execFileSync } from "node:child_process";

export function tvTarget(value, environment = process.env, run = execFileSync) {
  if (!value) {
    throw new Error("An explicit TV URL is required, for example http://tv:47990");
  }
  const url = new URL(value);
  if (url.protocol !== "http:" || url.pathname !== "/" || url.search || url.hash) {
    throw new Error("Use the TV's HTTP root URL");
  }
  if (url.username || url.password) {
    throw new Error("Pass credentials through the environment, not the URL");
  }
  let password = environment.WEBOS_STREAM_PASSWORD;
  if (!password && environment.WEBOS_SMOKE_SSH) {
    try {
      const config = JSON.parse(
        run(
          "ssh",
          [
            "-o",
            "BatchMode=yes",
            "-o",
            "ConnectTimeout=5",
            "-o",
            "LogLevel=ERROR",
            environment.WEBOS_SMOKE_SSH,
            "cat /media/developer/apps/usr/palm/services/org.webosbrew.jetkvm.service/config.json",
          ],
          { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] },
        ),
      );
      password = config.password;
    } catch {
      throw new Error("Cannot read TV credentials over SSH");
    }
  }
  if (typeof password !== "string" || !password) {
    throw new Error("Set WEBOS_STREAM_PASSWORD or WEBOS_SMOKE_SSH for TV credentials");
  }
  return { url: url.origin, password };
}

export async function verifyTvTarget(target, request = fetch) {
  const response = await request(target.url + "/api/status", {
    headers: {
      Authorization: "Basic " + Buffer.from("jetkvm:" + target.password).toString("base64"),
    },
    signal: AbortSignal.timeout(5000),
  });
  if (!response.ok) {
    throw new Error("The TV did not accept the configured credentials");
  }
  const status = await response.json();
  const hardwareSources = ["Nyx hardware H.264", "LG hardware H.264"];
  if (status.application !== "org.webosbrew.jetkvm" || !hardwareSources.includes(status.source)) {
    throw new Error("Integration tests require the webOS hardware backend");
  }
  if (status.streaming) {
    throw new Error("The TV already has an active viewer; disconnect it before integration tests");
  }
}
