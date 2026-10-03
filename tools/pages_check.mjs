// pages_check.mjs — run upstream Schwung's OWN contract validator and page
// planner over module.json, off-device, once per engine.
//
// The library is imported from a Schwung checkout, because the rules that
// matter (what a hidden knob does to the page, what the validator refuses)
// are the host's, and a re-implementation here would be a second opinion.
//
//   node tools/pages_check.mjs
//   SCHWUNG_SRC=/path/to/schwung node tools/pages_check.mjs
//
// Side effect: writes dist/tests/stereoid-fixture.json, a one-module fixture
// in the shape upstream's preview tools read:
//   node "$SCHWUNG_SRC/tools/param-pages/preview.mjs" stereoid --all \
//        --layout movy --fixture dist/tests/stereoid-fixture.json --png dist/tests/pages
import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";

const SRC = process.env.SCHWUNG_SRC || path.resolve("..", "schwung-current", ".worktrees", "v1.5.0");
const lib = (f) => pathToFileURL(path.join(SRC, "src", "shared", "param_pages", f)).href;
if (!fs.existsSync(path.join(SRC, "src", "shared", "param_pages", "page_plan.mjs"))) {
    console.error(`pages_check: no Schwung checkout at ${SRC} (set SCHWUNG_SRC) — skipped`);
    process.exit(0);
}
const { validateContract } = await import(lib("validate_contract.mjs"));
const { planPages, PAGE_KNOBS } = await import(lib("page_plan.mjs"));

const mod = JSON.parse(fs.readFileSync("src/module.json", "utf8"));
const hierarchy = mod.capabilities.ui_hierarchy;
const chainParams = mod.capabilities.chain_params;

let fail = 0;
const bad = (m) => { fail++; console.error("  FAIL " + m); };

// ---- the two copies of each param agree. The grid reads the level's entry,
// the chain host's C side reads chain_params; a range that differs between
// them is a knob that shows one scale and modulates on another.
const strip = (p) => { const { visible_if, ...rest } = p; return JSON.stringify(rest); };
const levelParams = hierarchy.levels.root.params;
if (levelParams.length !== chainParams.length) bad(`root declares ${levelParams.length} params, chain_params ${chainParams.length}`);
for (const p of levelParams) {
    const c = chainParams.find((q) => q.key === p.key);
    if (!c) bad(`${p.key} is on the page and not in chain_params`);
    else if (strip(c) !== strip(p)) bad(`${p.key} differs between the level and chain_params`);
    if (c && c.visible_if) bad(`${p.key}: visible_if is a LEVEL field; in chain_params it hides nothing`);
}

// ---- contract validation, the host's rules
const { findings } = validateContract({ id: mod.id, hierarchy, chainParams, capabilities: mod.capabilities });
for (const f of findings) {
    const line = `[${f.level}] ${f.rule}: ${f.message}`;
    if (f.level === "error") bad(line); else console.log("  " + line);
}

// ---- the page, per engine: ONE page of knobs, and only the knobs that
// apply (Josh: "only knobs that apply to each engine should show").
const want = {
    Comb:     "mode wide freq time trim comp hicut",
    Haas:     "mode wide freq time trim comp hicut late",
    Disperse: "mode wide freq time trim comp hicut",
    "M/S":    "mode wide freq trim comp hicut",
};
for (const [mode, keys] of Object.entries(want)) {
    const visible = (cond) => {
        if (!cond || cond.param !== "mode") return true;
        if ("equals" in cond) return cond.equals === mode;
        if ("not_equals" in cond) return cond.not_equals !== mode;
        return true;
    };
    const { pages } = planPages({ hierarchy, chainParams, visible });
    const knobPages = pages.filter((p) => p.kind === PAGE_KNOBS);
    const got = knobPages.map((p) => (p.keys || []).join(" ")).join(" | ");
    console.log(`  ${mode.padEnd(8)} ${pages.length} page(s): ${got}`);
    if (pages.length !== 1 || knobPages.length !== 1) bad(`${mode}: ${pages.length} pages planned, want exactly one knob page`);
    else if (got !== keys) bad(`${mode}: the page holds "${got}", want "${keys}"`);
}

// ---- a fixture for upstream's preview tools
fs.mkdirSync("dist/tests", { recursive: true });
fs.writeFileSync("dist/tests/stereoid-fixture.json", JSON.stringify({
    _source: "schwung-stereoid tools/pages_check.mjs — module.json's hierarchy and chain_params",
    generated_at: new Date().toISOString(),
    module_count: 1,
    not_captured: [],
    modules: [{
        id: mod.id, category: "audio_fx", component_key: "fx1", status: "ok",
        name: mod.name, version: mod.version,
        ui_hierarchy: hierarchy, chain_params: chainParams, presets: null,
    }],
}, null, 1));

console.log(fail ? `pages_check: FAILED (${fail})` : "pages_check: OK");
process.exit(fail ? 1 : 0);
