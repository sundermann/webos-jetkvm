// Prepare a patched build tree without modifying the upstream submodule.
import fs from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";

const root = fileURLToPath(new URL("../", import.meta.url));
const upstream = path.join(root, "third_party/jetkvm");
const patches = path.join(root, "patches/jetkvm");
const output = path.join(root, "build/jetkvm");
const preparationVersion = 2;
const stateFile = path.join(output, ".git/webos-prepared.json");
const exists = (file) =>
  fs.access(file).then(
    () => true,
    () => false,
  );
const git = (cwd, ...args) =>
  execFileSync("git", ["-C", cwd, ...args], { maxBuffer: 64 * 1024 * 1024 });
const hash = (value) => createHash("sha256").update(value).digest("hex");

async function inputs() {
  if (!(await exists(path.join(upstream, ".git")))) {
    git(root, "submodule", "update", "--init", "--", "third_party/jetkvm");
  }
  if (git(upstream, "rev-parse", "--show-toplevel").toString().trim() !== upstream) {
    throw new Error("third_party/jetkvm must be an initialized Git submodule.");
  }
  if (git(upstream, "status", "--porcelain").length) {
    throw new Error(
      "The upstream submodule has local changes. Keep edits in the patch series instead.",
    );
  }
  const revision = git(upstream, "rev-parse", "HEAD").toString().trim();
  const names = (await fs.readFile(path.join(patches, "series"), "utf8"))
    .split("\n")
    .map((line) => line.trim())
    .filter((line) => line && !line.startsWith("#"));
  if (!names.length || names.some((name) => !/^[\w.-]+\.patch$/.test(name))) {
    throw new Error("patches/jetkvm/series must list patch filenames in application order.");
  }
  const contents = await Promise.all(
    names.map((name) => fs.readFile(path.join(patches, name), "utf8")),
  );
  const fingerprint = hash(JSON.stringify([revision, names, contents, preparationVersion]));
  return { revision, names, contents, fingerprint };
}

function sourceState(directory) {
  // New files are registered with intent-to-add before this check; any other
  // untracked source also prevents replacing a developer's working tree.
  return hash(
    Buffer.concat([
      git(
        directory,
        "diff",
        "--binary",
        "--full-index",
        "HEAD",
        "--",
        "ui",
        "internal/logging/sse.html",
      ),
      git(
        directory,
        "ls-files",
        "--others",
        "--exclude-standard",
        "-z",
        "--",
        "ui",
        "internal/logging/sse.html",
      ),
    ]),
  );
}

function commit(directory, message) {
  git(directory, "add", "-A", "--", "ui", "internal/logging/sse.html");
  git(
    directory,
    "-c",
    "user.name=webOS JetKVM build",
    "-c",
    "user.email=build@localhost",
    "-c",
    "commit.gpgsign=false",
    "commit",
    "-qm",
    message,
  );
}

async function prepare() {
  const input = await inputs();
  if (await exists(stateFile)) {
    const state = JSON.parse(await fs.readFile(stateFile, "utf8"));
    if (state.fingerprint === input.fingerprint) {
      console.log("Patched JetKVM source is ready in build/jetkvm/ui.");
      return;
    }
    if (sourceState(output) !== state.sourceState) {
      throw new Error(
        "Build inputs changed while build/jetkvm/ui has edits. Preserve those edits before regenerating this directory.",
      );
    }
  } else if (await exists(output)) {
    throw new Error(
      "build/jetkvm already exists without preparation metadata; preserve or remove it before preparing the frontend.",
    );
  }
  const temporary = `${output}.tmp-${process.pid}`;
  await fs.mkdir(temporary, { recursive: true });
  try {
    execFileSync("tar", ["-x", "-C", temporary], {
      input: git(upstream, "archive", "HEAD", "ui", "internal/logging/sse.html"),
    });
    git(temporary, "init", "-q");
    await fs.appendFile(
      path.join(temporary, ".git/info/exclude"),
      "\n/ui/localization/paraglide/\n/ui/localization/jetKVM.UI.inlang/cache/\n",
    );
    commit(temporary, `JetKVM UI at ${input.revision}`);
    for (let i = 0; i < input.names.length; i++) {
      git(temporary, "apply", "--check", path.join(patches, input.names[i]));
      git(temporary, "apply", path.join(patches, input.names[i]));
      // Leave the final (webOS) patch editable against the preceding patches.
      if (i < input.names.length - 1) {
        commit(temporary, input.names[i]);
      }
    }
    git(temporary, "add", "-N", "--", "ui");
    await fs.writeFile(path.join(temporary, "UPSTREAM-REVISION"), `${input.revision}\n`);
    await fs.writeFile(
      path.join(temporary, ".git/webos-prepared.json"),
      JSON.stringify(
        {
          fingerprint: input.fingerprint,
          revision: input.revision,
          sourceState: sourceState(temporary),
        },
        null,
        2,
      ) + "\n",
    );
    const modules = path.join(output, "ui/node_modules");
    if (await exists(modules)) {
      await fs.rename(modules, path.join(temporary, "ui/node_modules"));
    }
    await fs.rm(output, { recursive: true, force: true });
    await fs.rename(temporary, output);
  } finally {
    await fs.rm(temporary, { recursive: true, force: true });
  }
  console.log(
    `Prepared JetKVM ${input.revision} with ${input.names.length} patches in build/jetkvm/ui.`,
  );
}

async function refresh() {
  const input = await inputs();
  if (!(await exists(stateFile))) {
    throw new Error("Run npm run prepare:ui before editing the generated source.");
  }
  const state = JSON.parse(await fs.readFile(stateFile, "utf8"));
  if (state.fingerprint !== input.fingerprint) {
    throw new Error(
      "Patch inputs changed since preparation. Preserve edits and prepare the updated source before refreshing.",
    );
  }
  git(output, "add", "-N", "--", "ui");
  const patch = git(output, "diff", "--binary", "--full-index", "HEAD", "--", "ui");
  if (!patch.length) {
    throw new Error("The webOS patch is empty; refusing to overwrite it.");
  }
  const name = input.names.at(-1);
  await fs.writeFile(path.join(patches, name), patch);
  state.fingerprint = (await inputs()).fingerprint;
  state.sourceState = sourceState(output);
  await fs.writeFile(stateFile, JSON.stringify(state, null, 2) + "\n");
  console.log(`Refreshed patches/jetkvm/${name}. Review the patch diff before committing.`);
}

try {
  const action = process.argv[2];
  if (action === "--refresh") {
    await refresh();
  } else if (action === undefined) {
    await prepare();
  } else {
    throw new Error(`Unknown option: ${action}`);
  }
} catch (error) {
  console.error(error.message);
  process.exitCode = 1;
}
