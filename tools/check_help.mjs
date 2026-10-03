#!/usr/bin/env node
/*
 * check_help.mjs — every src/help.json line must FIT ON THE SCREEN.
 *
 * ⚠⚠ A help line is DRAWN, never wrapped and never truncated. The host's
 * print() walks the string one glyph at a time and pixels past x=127 are
 * dropped by set_pixel with no error anywhere — so an over-long line loses its
 * tail SILENTLY. No ellipsis, nothing in the log. A 2026-08 sweep of the
 * catalog found 27 modules shipping lines that run off the screen, the worst by
 * 100px, which is most of a second line's worth of text nobody can read.
 *
 * ⚠ THE BUDGET IS PIXELS, NOT CHARACTERS. load_font trims every glyph to its
 * own inked extent, so the atlas is fixed-pitch and the screen is
 * PROPORTIONAL: "." advances 3px and "W" advances 6px. Counting characters
 * against 20 both rejects lines that render perfectly and passes lines that do
 * not — the host's own test says it was written that way first, and that is why
 * this measures instead.
 *
 * The glyph widths come from the HOST's scripts/generate_font.py, which
 * schwung_host.c names as the single source of truth for the atlas. Point
 * SCHWUNG_SRC at a checkout of the host Stereoid targets; without one this check
 * SKIPS rather than guessing, because a made-up width table is worse than no
 * check at all.
 *
 * Mirrors tests/host/test_help_content_width.sh upstream. Kept separate rather
 * than shared because it must run against a module's help, not the host's.
 */
import fs from "node:fs";
import path from "node:path";

const HELP = process.argv[2] || "src/help.json";
const SRC = process.env.SCHWUNG_SRC || path.resolve("..", "schwung-current", ".worktrees", "v1.5.0");
const GEN = path.join(SRC, "scripts", "generate_font.py");

if (!fs.existsSync(HELP)) {
    console.log(`${HELP}: absent — no on-device help (that is allowed; the module simply gets no Module Help row)`);
    process.exit(0);
}
if (!fs.existsSync(GEN)) {
    console.log(`${HELP}: SKIPPED — no font table at ${GEN}. Set SCHWUNG_SRC to a host checkout.`);
    process.exit(0);
}

const genSrc = fs.readFileSync(GEN, "utf8");
const cellM = genSrc.match(/CHAR_W,\s*CHAR_H\s*=\s*(\d+),\s*(\d+)/);
if (!cellM) { console.error(`could not read CHAR_W/CHAR_H from ${GEN}`); process.exit(1); }
const CELL_W = Number(cellM[1]), CELL_H = Number(cellM[2]);

/* Drawing constants, read from the host that draws rather than restated. */
const scrollSrc = fs.readFileSync(path.join(SRC, "src/shared/scrollable_text.mjs"), "utf8");
const hostSrc = fs.readFileSync(path.join(SRC, "src/schwung_host.c"), "utf8");
const SCREEN_WIDTH = Number((scrollSrc.match(/const SCREEN_WIDTH = (\d+);/) || [])[1]);
const ORIGIN_X = Number((scrollSrc.match(/print\(\s*(\d+)\s*,\s*y\s*,\s*lines\[i\]/) || [])[1]);
const CHAR_SPACING = Number((hostSrc.match(/load_font\("font\.png",\s*(\d+)\)/) || [])[1]);
if (![SCREEN_WIDTH, ORIGIN_X, CHAR_SPACING].every(Number.isFinite)) {
    console.error("could not read the drawing constants from the host");
    process.exit(1);
}

/* Per-glyph advance, trimmed the way load_font trims. */
const widths = new Map();
const entryRe = /(?:'((?:\\.|[^'\\])*)'|"((?:\\.|[^"\\])*)")\s*:\s*\[([^\]]*)\]/g;
let m;
while ((m = entryRe.exec(genSrc)) !== null) {
    const key = (m[1] !== undefined ? m[1] : m[2])
        .replace(/\\'/g, "'").replace(/\\"/g, '"').replace(/\\\\/g, "\\");
    if (key.length !== 1) continue;
    const rows = (m[3].match(/'([^']*)'/g) || []).map((s) => s.slice(1, -1));
    if (rows.length !== CELL_H || !rows.every((r) => r.length === CELL_W)) continue;
    let first = -1, last = -1;
    for (let x = 0; x < CELL_W; x++)
        for (let y = 0; y < CELL_H; y++)
            if (rows[y][x] !== ".") { if (first === -1) first = x; last = x; break; }
    widths.set(key, first === -1 ? CELL_W : last - first + 1);
}
if (widths.size < 90) { console.error(`only parsed ${widths.size} glyphs from ${GEN}`); process.exit(1); }

const rightEdge = (line) => {
    let x = ORIGIN_X, last = ORIGIN_X - 1;
    for (const ch of line) {
        const w = widths.get(ch);
        if (w === undefined) { x += CHAR_SPACING; continue; }   /* glyph() miss */
        last = x + w - 1;
        x += w + CHAR_SPACING;
    }
    return last;
};

const errors = [];
let lines = 0, titles = 0, widest = 0, deepest = "";
const walk = (node, trail) => {
    const here = `${trail}/${node.title || "?"}`;
    /* ⚠ A TITLE IS DRAWN TOO. It is the row you pick in the help list, printed
     * by the same print() with the same silent clipping past x=127 — so a long
     * section name loses its tail exactly the way a long line does, and this
     * checker measured only `lines` and would have said OK. Found while adding
     * "Buses & sends": the ampersand turned out to be in the atlas, but nothing
     * here would have caught it if it had not been. */
    if (node.title) {
        const edge = rightEdge(node.title);
        if (edge > SCREEN_WIDTH - 1)
            errors.push(`${here}: TITLE runs to x=${edge}, screen ends at ${SCREEN_WIDTH - 1}`);
        for (const ch of node.title)
            if (!widths.has(ch))
                errors.push(`${here}: TITLE character ${JSON.stringify(ch)} has no glyph and draws as a ${CHAR_SPACING}px gap`);
        titles++;
    }
    for (const line of node.lines || []) {
        lines++;
        const edge = rightEdge(line);
        if (edge > widest) { widest = edge; deepest = line; }
        if (edge > SCREEN_WIDTH - 1)
            errors.push(`${here}: runs to x=${edge}, screen ends at ${SCREEN_WIDTH - 1} — ${JSON.stringify(line)}`);
        for (const ch of line)
            if (!widths.has(ch))
                errors.push(`${here}: ${JSON.stringify(ch)} has no glyph and draws as a ${CHAR_SPACING}px gap — ${JSON.stringify(line)}`);
    }
    if (!(node.lines || []).length && !(node.children || []).length)
        errors.push(`${here}: leaf has neither lines nor children`);
    for (const kid of node.children || []) walk(kid, here);
};

const help = JSON.parse(fs.readFileSync(HELP, "utf8"));
/* A module's help is one tree with children; the host's own file uses
 * `sections`. Accept the module shape and say so if it is neither. */
const roots = Array.isArray(help.children) ? help.children
            : Array.isArray(help.sections) ? help.sections : null;
if (!roots || !roots.length) {
    console.error(`${HELP}: no children — the host shows a Module Help row only for a non-empty tree`);
    process.exit(1);
}
for (const r of roots) walk(r, "");

if (errors.length) {
    console.error(`${HELP}: FAILED`);
    for (const e of errors) console.error("  - " + e);
    process.exit(1);
}
console.log(`${HELP}: OK — ${lines} lines + ${titles} titles, widest right edge x=${widest} of ${SCREEN_WIDTH - 1} (${JSON.stringify(deepest)})`);
