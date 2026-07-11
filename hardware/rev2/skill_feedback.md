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

## 15. Footprints extracted from boards can smuggle board-frame keepout zones
Both rev-1-harvested footprints (Victor_Gold_Fingers_3, HRS_DM3AT-SF-PEJM5)
contained `(zone ...)` keepout rule areas whose polygon points were still in
the *donor board's absolute coordinates* (X≈264–330, Y≈195–230). Pads and
graphics are stored footprint-local, but zones captured into a footprint keep
whatever coordinates they had — so on the new board they rendered as
disconnected clumps ~300 mm off the page. Harmless to DRC where they landed,
but confusing, and if the anchor had been elsewhere they could have silently
imposed keepouts on real copper. Fix was deleting the zones from the .pretty
files and the embedded PCB copies.
→ Add to the footprint-extraction recipe (item 3): after FootprintSave, check
for `(zone` blocks in the .kicad_mod and either rebase their polygons to the
footprint origin or delete them; a one-line sanity check (max |coord| within
the footprint should be ≲ courtyard size) catches this class of bug.

## 17. place.py default search_radius_mm=15 fails on long parts
First placeroute run died with `PLACE FAIL: no legal position for RN1` (24.6 mm
SIP9) / J5 at every margin — the ring search can't reach open space within
15 mm of a crowded target, and larger courtyard margins make it worse. Fix was
a project-local autoroute.toml with `search_radius_mm = 40.0` (documented knob,
CLI > config). → SKILL Stage 4b should mention this first-line remedy; better,
place.py could auto-widen the radius (or placeroute sweep it) before declaring
PLACE FAIL.

## 18. `autoroute.py --skip-freerouting --close-gaps` double-adds pours on an
already-poured board (tool bug). Rerunning the post-route half on the
placeroute output duplicated both GND zones (4 zones total → 2×
`zones_intersect` hard violations) and its BD0 repair via landed co-located
with GF1 pad 25 (2× `holes_co_located`). Net effect: same 2 unconnected,
+4 new hard violations; had to roll back to the tool's own pre-route backup.
→ close-gaps/post-route should delete-or-skip existing pours (idempotent
pours) and collision-check repair vias against existing holes.

## 19. DRC unconnected-item distances mislead gap-repair decisions
DRC reported the BD7 break as pad↔track "1.15 mm", which motivated the
close-gaps attempt; close-gaps then measured the true connectivity gap as
52.2 mm (only the grid router could attempt it). → Skill note for Stage 5
triage: the DRC description's distance is pad-to-nearest-same-net-copper, not
the routable gap; check the closer's own SKIP/measure lines before choosing a
remedy.

## 20. Board-setup constraints vs anchor footprints should be preflighted
Two whole classes of Stage-5 hard DRC were knowable before any routing:
U3's stock WROOM footprint has 0.2 mm thermal-via drills vs the board setup's
0.3 mm min hole (×12), and J3's edge-mount microSD shield pads sit at 0.0 mm
from Edge.Cuts vs the 0.5 mm edge clearance (×2). Both are design decisions
(relax constraint / exclusion / footprint edit), and finding them after a
40-minute route sweep is the most expensive possible time. → Add a Stage 4a
gate check: scan locked footprints' pad drills and edge-adjacent pads against
the board setup constraints; surface conflicts to the human before Stage 4b.

## 21. autoroute's per-roll DRC ignores the project `.kicad_dru` — false alarms
A `.kicad_dru` rule (`edge_clearance min 0.3mm` for J3, KiCad 10) DOES relax
the board-setup edge clearance: kicad-cli DRC run on the project honors it
(0 violations, verified with/without the file). But the tool's per-roll DRC
summaries kept reporting the 2 J3 edge violations — the roll DRC evidently
runs without the project rules context — while the same run's FINAL DRC showed
0. Mid-run I mis-diagnosed this as "setup constraints are absolute floors"
(they are not, at least for edge_clearance). → llm_autoroute: make roll DRC
load the sibling .kicad_dru; skill note: judge rule exemptions only by
kicad-cli DRC on the real project, not by roll summaries.

## 22. Freerouting emits a redundant via co-located with a PTH pad
In two independent runs freerouting changed layers exactly at GF1's PTH pad 22
by emitting a discrete via at the pad's position → `holes_co_located` ×2. The
via is electrically redundant (a PTH pad already joins the layers); deleting
it clears the violations with connectivity intact (verify by DRC after). The
close-gaps stage did the same thing once ("via-joined" on top of the pad).
→ autoroute post-route could sweep for same-net vias co-located with PTH pads
and delete them automatically; skill note for Stage 5 triage meanwhile.

## 23. Card-edge footprints with paired SMD finger + companion PTH pads need a
routability preflight. GF1 (rev-1 harvest) has, per finger number, the SMD
finger pad at the edge AND a companion PTH pad 12.5 mm inboard, same pad
number ⇒ same net ⇒ the router must bridge every pair. Two consequences hit
Stage 5 repeatedly: (a) NC fingers form 2-pad nets literally named
`unconnected-(GF1-IRQ-Pad16)` that freerouting must still route (and once
plateau-failed on); (b) the pairs sit in the densest corridor, and freerouting
plateau-left exactly one bus net (BD0/BD6/BD7 in different runs) unrouted
GF1→U1 on the 2-layer board. The DRC "length 1.15 mm" text on the unconnected
item is the nearest stub, NOT the real gap (~52 mm) — reinforces item 19.
→ Stage 3/4a check: enumerate same-number multi-pad groups in big connector
footprints, confirm intent, and flag NC-net pad pairs (route, netclass, or
renumber) before routing.

## 24. (validation) Third anchor layout routed clean on the first roll
After two failed layouts (items 17-23), the human's third arrangement (U1
directly above GF1, shortening the bus corridor; U5 demoted from anchor to
placer-managed) probed fully routable at margin 1 in 10 passes and the final
route was perfect on roll 1 (122/122, 0 hard). Placement quality dominates
router effort by a wide margin — rolls/grid/close-gaps never rescued a bad
corridor, while a good corridor needed none of them. → Skill note for Stage 5
failures: after ~2 failed reroutes, the highest-value move is re-visiting
Stage 4a anchors (especially the corridor between the two densest connectors),
not more router knobs. Also validated: the fill-stage starved-thermal
remediation self-healed (1 hard after fill -> 0), and the placeroute early-stop
on a clean probe saved the rest of the sweep.

## 25. Stage 2 sourcing decisions never reach the schematic — Stage 6 fails on it
First check-bom run: 21 errors, 19 of them `no_identifier` for refs whose MPNs
ARE recorded in project.toml [[parts]] — but check-bom (type1) resolves by the
schematic's MPN field, and Stage 3 authoring never copied the [[parts]] MPNs
into the symbols. The other 2 errors were the own-stock module (U1) and the
PCB feature (GF1), which per parts-sourcing SKILL should not be BOM lines at
all (exclude-from-BOM + manual-offset note), yet were exported. The pipeline
has a silent seam: PROJECT_TOML.md says record MPNs in [[parts]], partbroker
SKILL says record MPNs in schematic fields, and nothing connects them.
→ Fix in schematic-authoring skill (Stage 3): "set each symbol's MPN property
from project.toml [[parts]] and in_bom=no for source=stock/pcb-feature parts"
— or better, gensch/SchBuilder grows a set_part_fields(project_toml) helper;
plus a Stage 3 lint that cross-checks symbol MPN fields against [[parts]].

## 16. Sub-stage gates (4a/4b) don't fit the integer `stage` field
PROJECT_TOML.md defines `stage` as an integer ("highest stage number whose
gate passed", advances by exactly one), but SKILL.md splits Stage 4 into two
gates (4a anchors/outline, 4b placement). After 4a passed there was no legal
way to record it — worked around with a comment, bumping to 4 only after 4b's
PLACE OK. → Either spec this convention in PROJECT_TOML.md ("stage = 4 means
both 4a and 4b passed; record 4a in a comment") or allow `stage = "4a"`.
