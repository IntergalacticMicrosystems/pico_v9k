# board_design skill — friction log from the v9k_dma_rev2 run

Running log of improvement opportunities for /root/sync/board_design/ observed
while executing the pipeline on a real project (started 2026-07-10). Each item:
what happened → proposed skill change.

## 1. No "brownfield / new revision" path
The pipeline assumes greenfield. This project is a rev 2 of an existing board
plus a sibling reference design, which changed almost every stage: profile was
implied by "use the same parts as <reference>", parts came pre-validated from a
proven BOM, and symbols/footprints had to be **extracted from the previous
rev's files** (embedded lib symbols in the .kicad_sch, footprints in the
.kicad_pcb) rather than sourced fresh.
→ Add a short "Revising an existing board" section: reuse the old rev's
embedded symbols/footprints (they are proven against each other), re-verify
sourcing only, and note that prior-rev firmware is the authority on which pins
are actually load-bearing (grep before re-wiring).

## 2. kiutils cannot parse boards written by newer KiCad; pcbnew can
`kiutils Board.from_file()` crashed (IndexError in Pad `(net ...)`) on the
rev-1 `.kicad_pcb` (KiCad 9/10 format). The same file loads fine with pcbnew.
The .kicad_sch parsed fine with kiutils.
→ In "Machine facts", note: for *reading existing boards*, kiutils is
unreliable — use pcbnew (`HOME=/root python3`); kiutils is for *authoring*
schematics.

## 3. Footprint-extraction recipe is nonobvious
Extracting a footprint from a board to a .pretty needs
`pcbnew.PCB_IO_MGR.FindPlugin(pcbnew.PCB_IO_MGR.KICAD_SEXP)` + `FootprintSave`
(KiCad 10 renamed `PluginFind`→`FindPlugin`), plus copy-constructor +
`SetParent(None)` to detach board nets.
→ Worth a 10-line snippet in the skill or an `edablock extract-fp
<board> <name>...` subcommand.

## 4. partbroker `lib` returns wrong package variants
Two cases in one run: ESP32-S3-WROOM-**1U** got the WROOM-**1** (PCB antenna)
footprint; 1N5822 got the P15.24mm long-lead land instead of the standard
P12.70mm. Stock/MPN was right; footprint variant was wrong.
→ parts-sourcing skill should say: `lib` footprints are a *suggestion* — always
cross-check the package variant (antenna type, lead pitch) against the
datasheet or a proven reference BOM before recording.

## 5. Stock symbol ↔ custom footprint pad mismatch is a silent gate gap
Stage 2 recorded stock symbol `Connector:Micro_SD_Card_Det_Hirose_DM3AT`
against the rev-1 footprint whose shield pads are named `P1–P4/SWA/SWB`. No
gate catches pin/pad-name mismatch until emit-pcb or layout. Caught only by
manually diffing pad names.
→ Candidate: a Stage 2/3 check (edablock lint or partbroker) that compares the
recorded symbol's pin numbers against the recorded footprint's pad names and
flags non-coverage.

## 6. PROJECT_TOML.md has no convention for own-stock / PCB-feature parts
The Stage 2 gate ("every non-block part has a partbroker-returned MPN") can't
literally hold for the WeAct module (own stock, no distributor) or gold
fingers (PCB edge feature, no part at all). Invented ad hoc: `note` field,
empty `symbol`, exempt-from-gate comment.
→ Spec these in PROJECT_TOML.md: optional `note`, `source = "stock" |
"pcb-feature" | "vendor"`, and gate wording "every *purchasable* part…".

## 7. Stage 0 "ask the human" vs autonomous runs
Stage 0 hard-gates on the human stating profile+quantity. Fine interactively
(AskUserQuestion worked), but the skill could allow profile to be *derived and
confirmed* when the user pins the design to a reference ("same parts/patterns
as X" ⇒ X's profile) so only quantity needs asking.

## 9. Independent netlist audit is cheap and catches what gates can't
`check-sch`/`lint-sch` validate structure, not *intent* — nothing verifies the
wiring matches the design doc. Auditing the **emitted PCB's pad→net table**
(pcbnew, ~50-line script diffing against the DESIGN.md pin map) checked all
231 pads in seconds and is orchestrator-independent of the authoring agent.
→ Recommend in Stage 3: after emit-pcb, dump pad→net and diff against the
design's pin-map table. Gotcha for the recipe: NC pads read back as singleton
`unconnected-(...)` net names, not empty strings.

## 10. Reusable authoring idioms discovered in Stage 3
(a) Edge connectors / module sockets with power pins: normalize ALL pins to
`passive` and drive rails via PWR_FLAGs — avoids ERC "two power outputs" and
"undriven input" errors on glorified-connector parts (same trick the reference
used for the WeAct module; equally needed for gold fingers).
(b) `make_module`-style name→number maps collapse duplicate pin names (two
"3V3" pins) — wire such pins by explicit number.
→ Both belong in the schematic-authoring skill's notes.

## 11. No concept of population variants
The design iterated to "some boards may not have the ESP32 populated" — a
DNP-variant requirement that reshaped the architecture (UI moved to an SPI
expander so ESP32-less boards stay functional). The pipeline/PROJECT_TOML have
no way to express build variants (per-variant DNP sets, BOM columns), and
Stage 6 order quantities differ per variant.
→ Consider a `[[variants]]` table (name + dnp-refs list) that check-bom /
order-bom can consume; at minimum, a skill note to ask about optional-module
variants during Stage 0 (it changes architecture, not just the BOM).

## 12. `emit-pcb` is idempotent for nets, not for footprint swaps
SKILL.md says "re-run after any schematic edit". When J4 changed from a 1×4 to
a 1×7 header, incremental emit-pcb kept the old 1×4 footprint (it updates pad
nets, doesn't replace footprints). The authoring agent had to delete the
.kicad_pcb and re-emit fresh — safe pre-4a, destructive after placement.
→ Document in Stage 3: footprint *changes* need a fresh emit (pre-anchor) or a
manual footprint swap in the PCB (post-anchor); ideally emit-pcb should detect
libId mismatch and warn or replace.

## 13. Stage 3 has no visual-review step
Stage 5 mandates human review of the routed render, but Stage 3 has none. The
plotted schematic PDF (kicad-cli sch export pdf) read back multimodally caught
layout/readability issues cheaply and confirmed title-block notes twice this
session. → Add to Stage 3: plot the PDF and review it (orchestrator or human)
before calling the stage done.

## 8. `search --qty` semantics vs boards×spares
The skill says `--qty <n>` but not what n should be for passives (boards ×
per-board + spares). The sourcing agent had to be told "add ~20% spares"
explicitly. → One sentence in parts-sourcing: qty = boards × per-board count,
+~20% spares for passives/diodes.

## 14. (validation, not friction) New Stage 3 checks held up on a real change
First incremental schematic change after the 2026-07-10 skill updates (added
JP1/JP2 ESP-flash jumpers): emit-pcb correctly reported added=2 with no
footprint_changed noise, the pad→net dump confirmed the crossover wiring in
seconds, and the PDF read-back caught nothing but proved cheap (~1 min).
No changes proposed — items 9/12/13 work as written.
