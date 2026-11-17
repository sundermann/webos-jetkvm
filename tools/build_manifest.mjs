import fs from "node:fs/promises";
import { createHash } from "node:crypto";
const app = JSON.parse(await fs.readFile("appinfo.json", "utf8"));
const filename = `${app.id}_${app.version}_all.ipk`;
const bytes = await fs.readFile(`dist/${filename}`);
const manifest = {
  id: app.id,
  version: app.version,
  title: app.title,
  appDescription: app.appDescription,
  root: true,
  ipk: filename,
  sha256: createHash("sha256").update(bytes).digest("hex"),
  size: bytes.length,
};
await fs.writeFile(`dist/${app.id}.manifest.json`, JSON.stringify(manifest, null, 2) + "\n");
console.log(`Manifest: dist/${app.id}.manifest.json`);
