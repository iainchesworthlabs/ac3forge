#!/usr/bin/env node
"use strict";

// Runs the site's measurement scripts against a local copy of the quality-history
// branch and prints what they render, because there is no browser test for them.
//
//   node tools/ci/render_measurement_tiles.js <history-dir>          the home page's strip
//   node tools/ci/render_measurement_tiles.js <history-dir> --page   performance-quality.md's cards
//   ...                                       --json                  the same, as JSON
//
// <history-dir> holds the *.jsonl files the scripts fetch from
// raw.githubusercontent.com/<repo>/quality-history/, for example a directory with
// main.jsonl, performance-main.jsonl, memory-main.jsonl, external-comparison-main.jsonl
// and ac4-quality-main.jsonl. The scripts run unchanged in a vm context with a stub
// `document` and a `fetch` that reads that directory; nothing is fetched from the network.
//
// test_write_measurement_badges.py calls this to hold the two scripts to the badges the
// same history produces (tools/ci/write_measurement_badges.py --history-dir).

const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const repoRoot = path.resolve(__dirname, "..", "..");

// The two surfaces and the class names each renders with.
const SURFACES = {
  strip: {
    file: path.join(repoRoot, "docs", "javascripts", "hero-stats.js"),
    mount: "ac3f-stats",
    ids: ["ac3f-stats"],
    classes: { card: "ac3f-stat-card", head: "ac3f-stat-head", sub: "ac3f-stat-sub", line: "ac3f-stat-line",
      codec: "ac3f-stat-codec", figure: "ac3f-stat-figure", what: "ac3f-stat-what", note: "ac3f-stat-note",
      chip: "ac3f-stat-badge", watch: "ac3f-stat-figure-watch" },
  },
  page: {
    file: path.join(repoRoot, "docs", "performance-quality.md"),
    mount: "pq-status",
    ids: ["pq-status", "pq-asof"],
    classes: { card: "pq-card", head: "pq-head", sub: "pq-sub", line: "pq-line", codec: "pq-codec",
      figure: "pq-figure", what: "pq-what", note: "pq-note", chip: "pq-badge", watch: "pq-figure-watch" },
  },
};

const ENTITIES = { amp: "&", lt: "<", gt: ">", quot: '"', "#39": "'", times: "×", mdash: "—", ndash: "–",
  hellip: "…", nbsp: " " };

// The text of a fragment of the scripts' own HTML: tags removed until none is left (one pass can
// leave a tag behind when it was split by another), entities decoded, whitespace collapsed.
function text(html) {
  let stripped = html;
  for (let before = ""; before !== stripped;) {
    before = stripped;
    stripped = stripped.replace(/<[^>]+>/g, "");
  }
  return stripped.replace(/&(#?\w+);/g, (m, name) => (name in ENTITIES ? ENTITIES[name] : m))
    .replace(/\s+/g, " ").trim();
}

function scriptOf(surface) {
  const source = fs.readFileSync(surface.file, "utf8");
  if (!surface.file.endsWith(".md")) return source;
  // The cards' script is inline in the page: the last <script> element.
  const scripts = [...source.matchAll(/<script\s*>([\s\S]*?)<\/script\s*>/gi)];
  if (scripts.length === 0) throw new Error(`${surface.file} has no <script> element`);
  return scripts[scripts.length - 1][1];
}

async function render(surface, historyDir) {
  const elements = {};
  for (const id of surface.ids) elements[id] = { innerHTML: "" };
  const sandbox = {
    document: {
      readyState: "complete",
      getElementById: (id) => elements[id] || null,
      addEventListener: () => {},
    },
    // The URLs the scripts build end in <file> after ".../quality-history/"; a file the
    // directory lacks is a 404, as it is for a *.recent.jsonl before the history outgrows it.
    fetch: async (url) => {
      const file = String(url).split("/quality-history/")[1];
      const local = file ? path.join(historyDir, file) : null;
      if (!local || !fs.existsSync(local)) return { ok: false, text: async () => "" };
      return { ok: true, text: async () => fs.readFileSync(local, "utf8") };
    },
    console,
  };
  // The point of this tool: run the two committed site scripts as they are, in a context with a stub
  // document and a fetch that reads a local directory. They are this repository's own files, and
  // nothing a caller supplies is evaluated. NOSONAR: javascript:S1523 flags every dynamic execution.
  vm.runInNewContext(scriptOf(surface), sandbox, { filename: surface.file }); // NOSONAR
  const mount = elements[surface.mount];
  for (let waited = 0; mount.innerHTML === "" && waited < 10000; waited += 10) {
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  if (mount.innerHTML === "") throw new Error(`${surface.file} rendered nothing into #${surface.mount}`);
  // The page fills its "as of" line after the cards; give it a moment.
  await new Promise((resolve) => setTimeout(resolve, 20));
  return { html: mount.innerHTML, asof: elements["pq-asof"] ? elements["pq-asof"].innerHTML : "" };
}

// One entry per card: title, chip, sub line, one entry per line, note.
function cardsOf(html, c) {
  const first = (chunk, cls) => {
    const m = new RegExp(`<(?:div|h3) class="${cls}">(.*?)</(?:div|h3)>`, "s").exec(chunk);
    return m ? m[1] : "";
  };
  return html.split(`<div class="${c.card}">`).slice(1).map((chunk) => {
    const head = first(chunk, c.head);
    const chip = new RegExp(`<span class="${c.chip} [^"]*">(.*?)</span>`).exec(head);
    const lines = [...chunk.matchAll(new RegExp(`<div class="${c.line}">(.*?)</div>`, "gs"))].map((m) => {
      const part = (cls) => {
        const p = new RegExp(`<span class="${cls}[^"]*">(.*?)</span>(?=<span|$)`, "s").exec(m[1]);
        return p ? text(p[1]) : "";
      };
      return { codec: part(c.codec), figure: part(c.figure), what: part(c.what), watch: m[1].includes(c.watch) };
    });
    return { title: text(head.replace(/<span class="[^"]*badge[^"]*">.*?<\/span>/, "")), chip: chip ? text(chip[1]) : null,
      sub: text(first(chunk, c.sub)), lines, note: text(first(chunk, c.note)) };
  });
}

async function main(argv) {
  const args = argv.slice(2);
  const historyDir = args.find((a) => !a.startsWith("--"));
  if (!historyDir) {
    process.stderr.write("usage: render_measurement_tiles.js <history-dir> [--page] [--json]\n");
    return 2;
  }
  const surface = args.includes("--page") ? SURFACES.page : SURFACES.strip;
  const rendered = await render(surface, path.resolve(historyDir));
  const cards = cardsOf(rendered.html, surface.classes);
  if (args.includes("--json")) {
    process.stdout.write(JSON.stringify({ cards, asof: text(rendered.asof) }, null, 2) + "\n");
    return 0;
  }
  for (const card of cards) {
    process.stdout.write(`== ${card.title}${card.chip ? ` [${card.chip}]` : ""}\n`);
    if (card.sub) process.stdout.write(`   ${card.sub}\n`);
    for (const l of card.lines) {
      process.stdout.write(`   ${l.codec.padEnd(7)} ${l.figure}${l.watch ? " [watch]" : ""}: ${l.what}\n`);
    }
    process.stdout.write(`   ${card.note}\n`);
  }
  if (rendered.asof) process.stdout.write(`-- ${text(rendered.asof)}\n`);
  return 0;
}

main(process.argv).then((code) => { process.exitCode = code; }, (err) => {
  process.stderr.write(`${err.stack || err}\n`);
  process.exitCode = 1;
});
