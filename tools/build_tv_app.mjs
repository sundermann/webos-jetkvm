import fs from "node:fs/promises";
import path from "node:path";
const destination = path.resolve("build/package/app");
const pkg = JSON.parse(await fs.readFile("package.json", "utf8"));
const app = JSON.parse(await fs.readFile("appinfo.json", "utf8"));
if (app.id !== pkg.config.appId || app.version !== pkg.version) {
  throw new Error("App identity/version must match package.json");
}
await fs.rm(destination, { recursive: true, force: true });
await fs.mkdir(destination, { recursive: true });
await Promise.all([
  ...(await fs.readdir("tv-app")).map((file) =>
    fs.copyFile(path.join("tv-app", file), path.join(destination, file)),
  ),
  ...["icon.png", "largeIcon.png"].map((file) =>
    fs.copyFile(path.join("assets", file), path.join(destination, file)),
  ),
]);
await fs.copyFile("node_modules/webostvjs/webOSTV.js", path.join(destination, "webOSTV.js"));
await fs.writeFile(path.join(destination, "appinfo.json"), JSON.stringify(app, null, 2) + "\n");
console.log(`TV setup app staged: ${app.id}`);
