# Stage 2 — Parts sourcing report (v9k_dma_rev2)

- **Profile:** type1 (hand-solder, THT + modules)
- **Quantity:** 5 boards
- **Tool:** `partbroker` (own stock + LCSC + Digi-Key + Mouser), run 2026-07-10
- **Vendor strategy:** Digi-Key is the primary/preferred single vendor. It covers
  18 of the 21 sourced lines exactly. Three exact target MPNs are **not on
  Digi-Key** in partbroker (only Mouser): IC1, RN2, C5/7/9. Rather than
  substitute proven parts, those three are ordered from **Mouser** (a tiny
  3-line, ~$4.62 second order). No electrical substitutions were required — every
  target MPN is in stock somewhere. An all-Digi-Key alternative for each of the
  three is noted inline if a single order is mandatory (it costs a package /
  footprint change on RN2, so the split is recommended).
- Own stock: none of the target MPNs matched the in-house inventory source.

## Per-part results

Prices are Digi-Key/Mouser unit at small (5–20 pc) quantity — deep price-break
tiers are cheaper but irrelevant at this volume. Stock shown is what partbroker
returned; all vastly exceed 5-board + 20% spare needs.

| refs | MPN | vendor | stock | unit $ | qty (5 bd) | line $ | notes |
|---|---|---|---|---|---|---|---|
| U3 | ESP32-S3-WROOM-1U-N16R8 | Digi-Key | 947 | 6.76 | 5 | 33.80 | Mouser 2538@6.90 also OK |
| U4 | LD1117V33 | Digi-Key | 9542 | 1.27 | 5 | 6.35 | LD1117V33C (newer die) 1.14 alt; Mouser 0.73 |
| IC1 | SN74LVC1G04DCKR | **Mouser** | 3334 | 0.12 | 5 | 0.60 | genuine TI; DK surfaces only an EVVO marketplace listing + the rev1 automotive SN74LVC1G04QDCKRQ1 (TI, 50572@0.30) as an all-DK option |
| D1 | 1N5822-TP | Digi-Key | 6408 | 0.61 | 5 | 3.05 | MCC, DO-201AD axial; Mouser 1N5822 (Diotec) 0.46 |
| D2,D3 | 1N4148 | Digi-Key | 1.0M+ | 0.06 | 10 | 0.60 | onsemi/Diotec, DO-35 |
| U1-SKT | PPTC152LFBN-RC | Digi-Key | 2317 | 1.62 | 10 | 16.20 | Sullins 2×15 socket; **Digi-Key only** |
| U1-SWD-SKT | PPTC041LFBN-RC | Digi-Key | 32037 | 0.42 | 5 | 2.10 | Sullins 1×4 socket; **Digi-Key only** |
| J3 | DM3AT-SF-PEJM5 | Digi-Key | 41298 | 2.85 | 5 | 14.25 | Hirose microSD; Mouser same price |
| RN1 | 4609X-101-473LF | Digi-Key | 15405 | 0.37 | 5 | 1.85 | Bourns 47k ×8 bussed SIP9; Mouser 314 stock |
| RN2 | 4604X-101-272LF | **Mouser** | 582 | 0.42 | 5 | 2.10 | Bourns 2.7k ×3 bussed SIP4; **Mouser only.** DK has no bussed SIP4 (only isolated 4604X-102, or bussed **SIP5** 4605X-101-272LF 302@0.28 — package change) |
| J4–J9 | PREC040SAAN-RC | Digi-Key | 31122 | 0.49 | 5 strips | 2.45 | Sullins 1×40 breakaway; **Digi-Key only**. Cut per board: J4=4, J5=5, J6=2, J7=8, J8=3, J9=4 = 26 pins |
| R1 | RNMF14FTC330R | Digi-Key | 35218 | 0.10 | 5 | 0.50 | Stackpole 330Ω 1/4W |
| R2,R3 | MFR-25FRF52-10K | Digi-Key | 405296 | 0.10 | 10 | 1.00 | YAGEO 10k 1/4W |
| R5,R6 | MFR-25FTE52-4K7 | Digi-Key | 57482 | 0.10 | 10 | 1.00 | YAGEO 4.7k 1/4W (DNP, ordered) |
| R8 | MFR-25FTE52-470R | Digi-Key | 20417 | 0.10 | 5 | 0.50 | YAGEO 470Ω 1/4W (selected: MFR-25 E52 family) |
| R7 | MFR-25FTE52-2K2 | Digi-Key | 32948 | 0.10 | 5 | 0.50 | YAGEO 2.2k 1/4W (selected: MFR-25 E52 family) |
| C1 | ESE227M025AG3AA | Digi-Key | 6081 | 0.33 | 5 | 1.65 | KEMET 220µF 25V electrolytic, 8×11.5mm, 3.5mm LS |
| C2,C3,C4,C8 | 50YXJ10M5X11 | Digi-Key | 27127 | 0.30 | 20 | 6.00 | Rubycon 10µF 50V electrolytic, 5×11mm, 2.0mm LS; **Digi-Key only** |
| C5,C7,C9 | RD20B104K500A5HAND | **Mouser** | 17076 | 0.128 | 15 | 1.92 | Walsin 100nF 50V MLCC radial; **Mouser only.** All-DK sub: KEMET C320C104K5R5TA (147k@0.32, same C_Disc 5.08mm footprint) |
| C6 | SR305E105ZARTR1 | Digi-Key | 1844 | 1.24 | 5 | 6.20 | KYOCERA AVX 1µF 50V MLCC radial; Mouser 0.88 |
| C10 | C315C103K5R5TA | Digi-Key | 36674 | 0.30 | 5 | 1.50 | KEMET 10nF 50V X7R radial MLCC (target was "any 10nF radial MLCC"; selected on Digi-Key) |

**Digi-Key subtotal ≈ $99.50 · Mouser subtotal ≈ $4.62**
**Total electronic parts (5 boards) ≈ $104.12 (~$20.82/board)**, excluding the
own-stock WeAct module and the bare PCB.

## Not sourced (own stock / PCB feature)

| refs | item | disposition |
|---|---|---|
| U1 | WeAct RP2350B Core Board (16MB flash + 8MB PSRAM) | Own stock, qty 5 (WeAct store / AliExpress). Not carried by DK/Mouser. Footprint `v9k_dma_rev2:WeAct_RP2350B_Core_Socket` |
| GF1 | Victor gold fingers | PCB edge feature, footprint `v9k_dma_rev2:Victor_Gold_Fingers_3`. No vendor part |
| PWR_FLAGs | Power flags | Schematic-only ERC symbols, no physical part / footprint |

## Symbol / footprint notes and flags

Per instruction, the following use the caller-supplied **known-good** values;
partbroker's own suggestion differed and is flagged here rather than silently
used:

- **U3 (ESP32):** using `RF_Module:ESP32-S3-WROOM-1` / **`RF_Module:ESP32-S3-WROOM-1U`**.
  partbroker `lib` returned footprint `RF_Module:ESP32-S3-WROOM-1` (PCB-antenna
  variant); the `-1U` (external/u.FL antenna) footprint is correct for the
  WROOM-1U module and is used. **Flagged.**
- **D1 (1N5822):** using **`Device:D_Schottky`** / **`Diode_THT:D_DO-201AD_P12.70mm_Horizontal`**.
  partbroker `lib` returned `Diode:1N5822` / `Diode_THT:D_DO-201AD_P15.24mm_Horizontal`
  (symbol and a wider 15.24 mm land vs the 12.70 mm land in use). Caller value
  used. **Flagged** — confirm the 12.70 mm lead pitch matches the placed part in
  layout.
- **U4 (LD1117V33):** caller value `Regulator_Linear:LD1117V33` /
  `Package_TO_SOT_THT:TO-220-3_Vertical` matches partbroker `lib` exactly. No conflict.
- **Project-local footprints** (from rev 1 `.pretty`, verified present on disk):
  IC1 `v9k_dma_rev2:SOT65P210X110-5N`, J3 `v9k_dma_rev2:HRS_DM3AT-SF-PEJM5`,
  RN1 `v9k_dma_rev2:4609X101473LF`, U1 `v9k_dma_rev2:WeAct_RP2350B_Core_Socket`,
  GF1 `v9k_dma_rev2:Victor_Gold_Fingers_3`.
- **IC1 symbol** `74xGxx:74LVC1G04` (single inverter, SC70-5) — confirmed in the
  installed KiCad `74xGxx` library.
- **J3 symbol** `Connector:Micro_SD_Card_Det_Hirose_DM3AT` — KiCad ships a symbol
  specifically for this Hirose DM3AT part.
- **RN1 symbol** `Device:R_Network08` (8 resistors + common bus, SIP9).
  **RN2 symbol** `Device:R_Network03` (3 resistors + common bus, SIP4), footprint
  `Resistor_THT:R_Array_SIP4` (generic; no project-local footprint for RN2).
- **U1-SKT / U1-SWD-SKT** use generic KiCad connector symbols/footprints
  (`Conn_02x15_Odd_Even` + `PinSocket_2x15`, `Conn_01x04` + `PinSocket_1x04`).
  Note these physical sockets populate the U1 module footprint
  `WeAct_RP2350B_Core_Socket` on the board; the generic symbol/footprint entries
  document the ordered parts.
- **Headers J4–J9** use the caller placeholder `Connector_Generic:Conn_01x0N` /
  `Connector_PinHeader_2.54mm:PinHeader_1x0N_P2.54mm_Vertical`; per-ref pin
  counts (4/5/2/8/3/4) are cut from PREC040SAAN-RC strips in Stage 3.
- **Electrolytics** C1 and C2/3/4/8 use `Device:C_Polarized` (this KiCad build has
  no `Device:CP`). **Film/ceramic radials** C5–C10 use `Device:C`. Axial
  resistors use `Device:R` with `Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal`.

## Gate verdict: **PASS**

All 21 sourced lines have a verified in-stock MPN (Digi-Key or Mouser) plus a
symbol and a footprint. No target MPN was out of stock; no electrical
substitution was needed. The only deviations from a single-vendor order are the
three Mouser-only lines (IC1, RN2, C5/7/9), each with a documented all-Digi-Key
fallback. Own-stock/PCB-feature items (U1 WeAct module, GF1, PWR_FLAGs) are
recorded but exempt from the sourcing gate.
