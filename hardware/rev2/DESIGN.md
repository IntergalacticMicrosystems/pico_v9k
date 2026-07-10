# v9k_dma_rev2 — Victor 9000 DMA card, WeAct RP2350B revision

Rev 2 of the Victor 9000 DMA/hard-disk replacement card. Replaces the Pimoroni
PGA2350 with a socketed **WeAct RP2350B Core** module, adds an
**ESP32-S3-WROOM-1U-N16R8** WiFi coprocessor (SPI-linked, population optional)
and an **MCP23S17 SPI GPIO expander** that carries all UI so boards without
the ESP32 keep full local functionality. Parts and patterns follow the proven
`v9k_flop` emulator design
(`/root/sync/Victor9000-Development-Private/v9k_flop/hardware/emulator/`).

Reference for the previous revision: `../Victor9K_pga2350.kicad_sch` +
firmware pin map `../../software/pico_victor/dma.h`.

## Architecture (2026-07-10 rework — expander revision)

- **Every Victor bus signal and the SD card stay on their exact rev-1 GPIOs.**
  No bus-side firmware changes at all.
- One shared SPI1 bus (GP42/43/44), four CS-gated devices, per-CS baud rates:
  microSD (CS=GP41, 25 MHz), ESP32-S3 FSPI slave (CS=GP45, ≤26 MHz),
  MCP23S17 (CS=GP47, ≤10 MHz), SPI OLED (CS from expander, ≤10 MHz).
  Single-owner arbitration: one transaction at a time; poll the expander
  between SD blocks, never mid-burst.
- ESP32 link is **SPI-only** (no UART): DRDY, EN, IO0 all route through the
  expander. ESP32 firmware loads via its USB pins on J7 (RP2350 can hold
  EN/IO0 through the expander), WiFi OTA, or — with JP1/JP2 shunts installed —
  from the RP2350 itself (esp-serial-flasher over UART0, image from SD; EN/IO0
  strapped via the expander). Shunts open by default; open them before using a
  USB-serial adapter on J8 (its TX would fight ESP TXD0 on the GP33 net).
- UI (SPI OLED + rotary encoder + activity LED) hangs off the MCP23S17 on
  `+3V3_PERI` — fully functional with U3 unpopulated.

## RP2350B GPIO map (rev 2)

| GPIO | use | | GPIO | use |
|---|---|---|---|---|
| GP0 | **NC — module PSRAM CS1** (QMI XIP_CS1n) | | GP31 | CLK15B (unchanged) |
| GP1–8 | BD0–BD7 (unchanged) | | GP32 | IR4 / DMA IRQ out (unchanged) |
| GP9–20 | A8–A19 (unchanged) | | GP33 | UART0 RX, debug J8 (unchanged, F2) |
| GP21 | RD (unchanged) | | GP34 | IR5 (unchanged) |
| GP22 | WR (unchanged) | | GP35 | IO_M (unchanged) |
| GP23 | DT_R (unchanged; remove module user-key 0R) | | GP36 | SSO (unchanged) |
| GP24 | ALE + pulldown network (unchanged) | | GP37 | DLATCH (unchanged) |
| GP25 | HOLD (unchanged; remove module user-LED 0R) | | GP38 | CSEN (unchanged) |
| GP26 | XACK (unchanged) | | GP39 | PHASE2 (unchanged) |
| GP27 | EXTIO (unchanged) | | GP40 | DEN (unchanged) |
| GP28 | READY (unchanged) | | GP41–44 | SD_CS / SPI1_SCK / SPI1_MOSI / SPI1_MISO (unchanged; SPI1 now shared) |
| GP29 | HLDA (unchanged) | | GP45 | **ESP_CS** |
| GP30 | CLK5 (unchanged) | | GP46 | **UART0 TX (debug, F11 UART_AUX)** |
| | | | GP47 | **MCP_CS (MCP23S17)** |

Module non-GPIO pins: 5V=`+5V_RAIL`, VBUS=NC, 3V3×2=`3V3_WEACT` (IC1 only),
RUN ← RESET inverter, EN=NC, SWD pads 61–64 → J9.

Only three GPIOs differ from rev 1: GP45 (was breakout) → ESP_CS, GP46 (was
breakout) → UART0 TX (forced off GP0 by the module PSRAM), GP47 (was unused)
→ MCP_CS. Gold fingers NC: only 15 (NMI), 16 (IRQ), 34 (+12V) — same as rev 1.

## MCP23S17 expander (U5, DIP-28, on +3V3_PERI)

CS=GP47, SCK/SI/SO on SPI1, A0–A2=GND, RESET pulled up 10k (R4) to
+3V3_PERI, INTA/INTB NC. 100nF decoupling (C11).

| GPA | use | GPB | use |
|---|---|---|---|
| GPA0 | ENC_A → J5-1 | GPB0 | OLED_CS → J4-7 |
| GPA1 | ENC_B → J5-2 | GPB1 | OLED_DC → J4-6 |
| GPA2 | ENC_SW → J5-3 | GPB2 | OLED_RST → J4-5 |
| GPA3 | LED_DRV → R1 330Ω → J6-1 | GPB3–7 | spare → J10 |
| GPA4 | ESP_DRDY (input ← U3 IO9) | | |
| GPA5 | ESP_EN (out; R2 10k pullup keeps ESP32 booting if expander unconfigured) | | |
| GPA6 | ESP_IO0 (out; R3 10k pullup) | | |
| GPA7 | spare → J10 | | |

Encoder is polled quadrature (~1 kHz expander reads; poll between SD blocks).
OLED is an **SPI module**: pixel data streams on SPI1 at full speed while the
expander latches CS/DC/RST.

## ESP32-S3 (U3) — optional population

Power pin 2 on `+3V3_PERI` (C4 10µF + C5 100nF). FSPI slave on IOMUX pins:
IO10(18)=ESP_CS←GP45, IO11(19)=SPI1_MOSI, IO12(20)=SPI1_SCK,
IO13(21)=SPI1_MISO, IO9(17)=ESP_DRDY→expander GPA4. EN(3)=ESP_EN
(+C6 1µF delay, J7-3); IO0(27)=ESP_IO0 (J7-4). TXD0(37)/RXD0(36) go to J7-5/6
(external flashing/console) and to JP2-2/JP1-2 — shunts cross them over to
UART0_RX(GP33)/UART0_TX(GP46) so the RP2350 can drive the ROM bootloader.
USB D-(13)/D+(14) → J7-7/8.
IO35–37 (28–30) NC (octal PSRAM). IO4–7, IO14 now NC (UI moved to expander).
GND pins 1, 40, 41. Unpopulated-U3 boards: R2/R3/C6/J7 may still be fitted;
nothing else changes, WiFi is simply absent.

### Why the leftover bus signals stayed connected this time

The 2026-07-10 rework dropped the RP2350↔ESP32 UART and moved UI/control to
the expander specifically so CLK15B, IR5, DLATCH, CSEN and PHASE2 could stay
wired as in rev 1 (they are unused by today's validated firmware but preserved
for future bus work). CLK5 (`dma_master.pio` waits), DEN/SSO/IO_M (driven
outputs) and IR4 (IRQ out) were always load-bearing.

## Firmware migration notes (rev 2 board)

1. `UART_TX_PIN` 0 → 46 with `gpio_set_function(46, GPIO_FUNC_UART_AUX)`
   (F11); RX on GP33 stays F2. Same UART0. All other rev-1 defines unchanged.
2. GP0 must never be driven (module PSRAM CS). PSRAM usable via QMI CS1
   (see v9k_flop `firmware/psram.c`).
3. New: MCP23S17 driver on SPI1 (mode 0, ≤10 MHz, CS=GP47): UI polling loop
   (encoder/DRDY/LED), OLED CS/DC/RST latching. ESP32 SPI transactions
   (mode 0, ≤26 MHz, CS=GP45). Per-CS `spi_set_baudrate` before each owner
   change; never interleave inside an SD block transfer.
4. OLED driver = SSD1306/SH1106 over SPI (was I2C in the pre-rework plan).
5. WeAct module prep: remove the GP23 user-key and GP25 user-LED 0R links.

## Power

- `VICTOR_5V` (fingers 36/37) → **D1 1N5822** → `+5V_RAIL` (C1 220µF, C2 10µF).
- `+5V_RAIL` → WeAct 5V pin (module regulator → `3V3_WEACT`) and
  **U4 LD1117V33** (C3 10µF out) → `+3V3_PERI` (ESP32, SD, expander, OLED,
  encoder, pullups).
- WeAct VBUS pin NC. USB bench power enters through the module's internal
  VBUS diode (verified from the WeAct schematic PDF); D1 blocks backfeed into
  the Victor PSU; the module diode blocks backfeed into a USB host.
- `-12V` (finger 32): ALE network only — R7 2.2k ALE→-12V, C10 10nF filter,
  clamp node R8 470Ω from +5V_RAIL with D2 (K=ALE) / D3 (K=GND) 1N4148s.
- RN2 (2.7k bussed) pullups on RD/WR/DT_R to `+5V_RAIL`; RN1 (47k bussed)
  pullups on all SD lines to `+3V3_PERI`.
- RESET (finger 13) → IC1 SN74LVC1G04 (VCC=`3V3_WEACT`, C9 100nF) → module RUN.

## Headers

| ref | pins | pinout |
|---|---|---|
| J4 OLED (SPI) | 1×7 | 1=GND 2=+3V3_PERI 3=SPI1_SCK 4=SPI1_MOSI 5=OLED_RST 6=OLED_DC 7=OLED_CS |
| J5 encoder | 1×5 | 1=ENC_A 2=ENC_B 3=ENC_SW 4=+3V3_PERI 5=GND |
| J6 activity LED | 1×2 | 1=ACT_LED (330Ω from GPA3) 2=GND |
| J7 ESP32 breakout | 1×8 | 1=GND 2=+3V3_PERI 3=ESP_EN 4=ESP_IO0 5=ESP_TXD0 6=ESP_RXD0 7=ESP_USB_D- 8=ESP_USB_D+ |
| J8 debug UART | 1×3 | 1=GND 2=UART0_TX(GP46) 3=UART0_RX(GP33), 3.3V |
| JP1 ESP flash TX | 1×2 | 1=UART0_TX(GP46) 2=ESP_RXD0 — shunt = RP2350→ESP32 flash link |
| JP2 ESP flash RX | 1×2 | 1=UART0_RX(GP33) 2=ESP_TXD0 — shunt = RP2350→ESP32 flash link |
| J9 SWD | 1×4 | 1=GND 2=SWCLK 3=SWDIO 4=3V3_WEACT (module H4 pads 61–64) |
| J10 expander spares | 1×8 | 1=GND 2=+3V3_PERI 3=XGPIO_A7 4=XGPIO_B3 5=XGPIO_B4 6=XGPIO_B5 7=XGPIO_B6 8=XGPIO_B7 |

Header pin total 41/board (JP1/JP2 added) — two 40-pin breakaway strips per
board; the leftover covers spares. Plus 2 jumper shunts/board for JP1/JP2.
R5/R6 (I2C pullups) deleted — no I2C anywhere on the board.
