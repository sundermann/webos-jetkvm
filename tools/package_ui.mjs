// Stage JetKVM's frontend source and third-party notices with the built UI.
import fs from "node:fs/promises";
import path from "node:path";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
const root = fileURLToPath(new URL("../", import.meta.url));
const prepared = path.join(root, "build/jetkvm");
const ui = path.join(prepared, "ui");
const assets = path.join(ui, "dist/assets");
await fs.mkdir(assets, { recursive: true });
await fs.copyFile(path.join(ui, "LICENSE"), path.join(assets, "jetkvm-LICENSE.txt"));
const upstream = await fs.readFile(path.join(ui, "UPSTREAM.md"), "utf8");
async function entries(directory) {
  try {
    return await fs.readdir(directory, { withFileTypes: true });
  } catch (error) {
    if (error.code === "ENOENT") {
      return [];
    }
    throw error;
  }
}
async function packageNotices(folder) {
  const pkg = JSON.parse(await fs.readFile(path.join(folder, "package.json"), "utf8"));
  const licenses = (await entries(folder))
    .filter(
      (entry) => entry.isFile() && /^(license|licence|copying|notice)([.-]|$)/i.test(entry.name),
    )
    .sort((left, right) => left.name.localeCompare(right.name));
  const notices = await Promise.all(
    licenses.map(
      async (file) =>
        `\n--- ${pkg.name} ${pkg.version} / ${file.name} ---\n` +
        (await fs.readFile(path.join(folder, file.name), "utf8")) +
        "\n",
    ),
  );
  return notices.join("") + (await collect(path.join(folder, "node_modules")));
}
async function collect(directory) {
  const packages = (await entries(directory))
    .filter((entry) => entry.isDirectory() && !entry.name.startsWith("."))
    .sort((left, right) => left.name.localeCompare(right.name));
  const notices = await Promise.all(
    packages.map((entry) => {
      const folder = path.join(directory, entry.name);
      return entry.name.startsWith("@") ? collect(folder) : packageNotices(folder);
    }),
  );
  return notices.join("");
}
const notices = upstream + "\n\n" + (await collect(path.join(ui, "node_modules")));
await fs.writeFile(path.join(assets, "jetkvm-third-party.txt"), notices);
execFileSync(
  "tar",
  [
    "--exclude=ui/node_modules",
    "--exclude=ui/dist",
    "--exclude=ui/localization/paraglide",
    "--exclude=ui/localization/jetKVM.UI.inlang/cache",
    "-czf",
    path.join(assets, "jetkvm-source.tar.gz"),
    "-C",
    prepared,
    "ui",
    "internal/logging/sse.html",
    "UPSTREAM-REVISION",
    "-C",
    root,
    "patches/jetkvm",
    "native/JETKVM_PROTOCOL.md",
  ],
  { env: { ...process.env, COPYFILE_DISABLE: "1" } },
);
console.log("JetKVM assets, GPL source archive, and third-party notices prepared.");
