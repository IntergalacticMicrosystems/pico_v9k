#!/usr/bin/env python3
"""Generate the v9k_dma_rev2 schematic (Victor 9000 DMA card, WeAct RP2350B rev).

Source of truth for v9k_dma_rev2.kicad_sch + v9k_dma_rev2.kicad_pro.

Run with the kiutils venv:
    /root/sync/eda_blocks/.venv/bin/python generate_schematic.py
Validate (from this dir):
    edablock check-sch  v9k_dma_rev2.kicad_sch     # ERC + structural gate
    edablock lint-sch   v9k_dma_rev2.kicad_sch     # logical-wiring lints
    edablock emit-pcb   v9k_dma_rev2.kicad_sch     # -> v9k_dma_rev2.kicad_pcb
    kicad-cli sch export pdf -o v9k_dma_rev2.pdf v9k_dma_rev2.kicad_sch

Pattern copied verbatim from v9k_flop/hardware/emulator/generate_schematic.py
(newuuid/load_lib/resolve/pin_coords/make_module/add_symbol/net/auto-NoConnect,
plus the WEACT_H1/H2/H4 tables + _wn/U1n helpers). The four rev1 project-local
symbols (GF1 Victor 9K Expansion Port, J3 DM3AT, IC1 SN74LVC1G04, RN1 47k SIP9)
are deep-copied out of the rev1 schematic and re-embedded under nickname
v9k_dma_rev2 with project.toml footprints. GF1's pins are normalised to passive
(edge connector = glorified connector, like the module); the four PWR_FLAGs
supply the rails for ERC. See DESIGN.md for the full change spec.

2026-07-10 rework (expander revision): every Victor bus signal back on its
rev-1 GPIO; U5 MCP23S17 (CS=GP47) carries all UI (SPI OLED / encoder / LED)
plus the ESP32 EN/IO0/DRDY control lines, so the board is fully functional
with U3 unpopulated; ESP32 link is SPI-only (TXD0/RXD0 go to J7 and, via
JP1/JP2 shunt jumpers, cross over to UART0 so the RP2350 can flash U3);
J4 is a 1x7 SPI-OLED header; J10 breaks out the spare expander GPIOs;
UART0 TX moved to GP46; R5/R6 (I2C pullups) deleted — no I2C on the board.
"""
import copy, hashlib, json, os
from kiutils.schematic import Schematic
from kiutils.symbol import SymbolLib, Symbol, SymbolPin
from kiutils.items.schitems import (SchematicSymbol, GlobalLabel, Connection,
    SymbolProjectInstance, SymbolProjectPath, HierarchicalSheetInstance,
    Junction, NoConnect)
from kiutils.items.common import (Property, Position, Effects, Font, Justify,
    TitleBlock, Stroke)
from kiutils.items.syitems import SyRect

SYMDIR = "/usr/share/kicad/symbols"
PROJECT = "v9k_dma_rev2"
NICK = "v9k_dma_rev2"
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, f"{PROJECT}.kicad_sch")
PRO_OUT = os.path.join(HERE, f"{PROJECT}.kicad_pro")
REV1_SCH = "/root/sync/pico_v9k/hardware/Victor9K_pga2350.kicad_sch"
EMU_PRO = ("/root/sync/Victor9000-Development-Private/v9k_flop/hardware/"
           "emulator/v9k_flop_emulator.kicad_pro")

_ctr = [0]
def newuuid(tag=""):
    _ctr[0] += 1
    h = hashlib.md5(f"v9kdmarev2-{tag}-{_ctr[0]}".encode()).hexdigest()
    return f"{h[0:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:32]}"

ROOT_UUID = newuuid("rootsheet")

# ---- stock symbol loading + extends resolution ---------------------------
_libcache = {}
def load_lib(fn):
    if fn not in _libcache:
        path = fn if os.path.isabs(fn) else f"{SYMDIR}/{fn}"
        _libcache[fn] = SymbolLib().from_file(path)
    return _libcache[fn]

def find_sym(fn, name):
    for s in load_lib(fn).symbols:
        if s.entryName == name:
            return s
    raise KeyError(name)

def resolve(fn, name):
    s = find_sym(fn, name)
    if s.extends:
        base = copy.deepcopy(resolve(fn, s.extends))
        base.entryName = name
        base.libraryNickname = s.libraryNickname
        for u in base.units:
            u.entryName = name
        childprops = {p.key: p for p in s.properties}
        for i, p in enumerate(base.properties):
            if p.key in childprops:
                base.properties[i] = copy.deepcopy(childprops[p.key])
        base.extends = None
        return base
    return copy.deepcopy(s)

def pin_coords(sym):
    d = {}
    def collect(s):
        for p in s.pins:
            d[p.number] = (p.position.X, p.position.Y, p.position.angle or 0)
        for u in s.units:
            collect(u)
    collect(sym)
    return d

# ---- rev1 project-local symbols (deep-copied out of the rev1 schematic) ---
_rev1 = Schematic().from_file(REV1_SCH)
def rev1_sym(entryName):
    for s in _rev1.libSymbols:
        if s.entryName == entryName:
            return copy.deepcopy(s)
    raise KeyError(entryName)

GF1_SYM = rev1_sym("Victor 9K Expansion Port")
J3_SYM  = rev1_sym("DM3AT-SF-PEJM5")
IC1_SYM = rev1_sym("SN74LVC1G04QDCKRQ1")
RN1_SYM = rev1_sym("4609X-101-473LF")

# ---- custom module symbol builder ----------------------------------------
def eff(size=1.27, justify=None, hide=False):
    e = Effects(font=Font(width=size, height=size))
    if justify:
        e.justify = Justify(horizontally=justify)
    e.hide = hide
    return e

def make_module(entryName, left, right, halfw=17.78):
    """Box symbol: left/right = [(number,name)] top->bottom, None = spacer row.
    All pins passive (module = glorified connector); PWR_FLAGs handle ERC."""
    rows = max(len(left), len(right))
    body_top = ((rows + 1) // 2) * 2.54 + 2.54
    body_bot = body_top - (rows + 1) * 2.54
    sym = Symbol()
    sym.entryName = entryName
    sym.inBom = True; sym.onBoard = True
    sym.pinNamesOffset = 1.016
    sym.properties = [
        Property(key="Reference", value="U", id=0,
                 position=Position(0, body_top + 2.54, 0), effects=eff()),
        Property(key="Value", value=entryName, id=1,
                 position=Position(0, body_bot - 2.54, 0), effects=eff()),
        Property(key="Footprint", value="", id=2,
                 position=Position(0, 0, 0), effects=eff(hide=True)),
        Property(key="Datasheet", value="~", id=3,
                 position=Position(0, 0, 0), effects=eff(hide=True)),
    ]
    unit = Symbol()
    unit.entryName = entryName
    unit.unitId = 1; unit.styleId = 1
    rect = SyRect()
    rect.start = Position(-halfw, body_top)
    rect.end = Position(halfw, body_bot)
    rect.stroke = Stroke(width=0.254, type="default")
    rect.fill.type = "background"
    unit.graphicItems = [rect]
    pins = []
    for i, ent in enumerate(left):
        if ent is None: continue
        num, name = ent
        p = SymbolPin(electricalType="passive", graphicalStyle="line",
                      position=Position(-(halfw + 5.08), body_top - (i + 1) * 2.54, 0),
                      length=5.08, name=name, number=str(num),
                      nameEffects=eff(), numberEffects=eff())
        pins.append(p)
    for i, ent in enumerate(right):
        if ent is None: continue
        num, name = ent
        p = SymbolPin(electricalType="passive", graphicalStyle="line",
                      position=Position(halfw + 5.08, body_top - (i + 1) * 2.54, 180),
                      length=5.08, name=name, number=str(num),
                      nameEffects=eff(), numberEffects=eff())
        pins.append(p)
    unit.pins = pins
    sym.units = [unit]
    return sym

# ---- WeAct RP2350B Core: canonical pin numbering --------------------------
# H1 (board right column, top->bottom, outer/inner) = pins 1..30
# H2 (board left column) = pins 31..60 ; H4 SWD = 61..64.
WEACT_H1 = [(1,"GP24"),(2,"GP23"),(3,"GP22"),(4,"GP21"),(5,"GP20"),(6,"GP19"),
            (7,"GP18"),(8,"GP17"),(9,"GP16"),(10,"GP15"),(11,"GP14"),(12,"GP13"),
            (13,"GP12"),(14,"GP11"),(15,"GP10"),(16,"GP9"),(17,"GP8"),(18,"GP7"),
            (19,"GP6"),(20,"GP5"),(21,"GP4"),(22,"GP3"),(23,"GP2"),(24,"GP1"),
            (25,"GP0"),(26,"VREF"),(27,"GND"),(28,"GND"),(29,"5V"),(30,"VBUS")]
WEACT_H2 = [(31,"GP26"),(32,"GP25"),(33,"GP28"),(34,"GP27"),(35,"GP30"),
            (36,"GP29"),(37,"GP32"),(38,"GP31"),(39,"GP34"),(40,"GP33"),
            (41,"GP36"),(42,"GP35"),(43,"GP38"),(44,"GP37"),(45,"GP40"),
            (46,"GP39"),(47,"GP42"),(48,"GP41"),(49,"GP44"),(50,"GP43"),
            (51,"GP46"),(52,"GP45"),(53,"RUN"),(54,"GP47"),(55,"3V3"),
            (56,"3V3"),(57,"GND"),(58,"EN"),(59,"GND"),(60,"GND")]
WEACT_H4 = [(61,"SWD_GND"),(62,"SWCLK"),(63,"SWDIO"),(64,"SWD_3V3")]

_wn = {name: num for num, name in WEACT_H1 + WEACT_H2 + WEACT_H4}
weact_left = [(_wn[f"GP{i}"], f"GP{i}") for i in range(25)] + [
    None, (26,"VREF"), (30,"VBUS"), (29,"5V"), (27,"GND"), (28,"GND")]
weact_right = ([(_wn[f"GP{i}"], f"GP{i}") for i in range(25, 48)] +
    [None, (53,"RUN"), (58,"EN"), (55,"3V3"), (56,"3V3"), (57,"GND"), (59,"GND"),
     (60,"GND"), None, (62,"SWCLK"), (63,"SWDIO"), (64,"SWD_3V3"), (61,"SWD_GND")])
WEACT_SYM = make_module("WeAct_RP2350B_Core", weact_left, weact_right)

# ---- schematic assembly ----------------------------------------------------
sc = Schematic().create_new()
sc.version = 20211123   # kiutils-supported; KiCad 10 reads & upgrades
sc.uuid = ROOT_UUID
sc.paper.paperSize = "A2"

embedded = {}
def embed(libId, sym):
    if libId not in embedded:
        sym = copy.deepcopy(sym)
        sym.libraryNickname, sym.entryName = libId.split(":")
        for u in sym.units:
            u.entryName = libId.split(":")[1]
        embedded[libId] = sym
    return embedded[libId]

placed = []; labels = []; wires = []; noconns = []
sym_pins = {}; sym_used = {}

def snap(v, grid=2.54):
    return round(round(v / grid) * grid, 2)
def r2(v):
    return round(float(v), 2)

def add_symbol(libId, src, ref, value, x, y, footprint="", rot=0, mirror=None,
               fields=None, pin_type=None, inbom=True, dnp=False):
    """src = (libfile, symname) for stock, or a Symbol object for custom/rev1."""
    x = snap(x); y = snap(y)
    base = src if isinstance(src, Symbol) else resolve(*src)
    sym = embed(libId, base)
    if pin_type:
        def _setpins(s):
            for p in s.pins: p.electricalType = pin_type
            for u in s.units: _setpins(u)
        _setpins(sym)
    coords = pin_coords(sym)
    inst = SchematicSymbol()
    inst.libraryNickname, inst.entryName = libId.split(":")
    inst.position = Position(x, y, rot)
    inst.unit = 1
    inst.inBom = inbom; inst.onBoard = inbom; inst.dnp = dnp
    inst.uuid = newuuid(f"sym-{ref}")
    inst.mirror = mirror
    ys = [y - py for _, (px, py, ang) in coords.items()] or [y]
    ref_y = r2(min(min(ys), y) - 5.08)
    val_y = r2(max(max(ys), y) + 5.08)
    props = [
        Property(key="Reference", value=ref, id=0,
                 position=Position(x, ref_y, 0), effects=eff()),
        Property(key="Value", value=value, id=1,
                 position=Position(x, val_y, 0), effects=eff()),
        Property(key="Footprint", value=footprint, id=2,
                 position=Position(x, y, 0), effects=eff(hide=True)),
        Property(key="Datasheet", value="~", id=3,
                 position=Position(x, y, 0), effects=eff(hide=True)),
    ]
    if fields:
        idn = 4
        for k, v in fields.items():
            p = Property(key=k, value=v, id=idn, position=Position(x, y, 0),
                         effects=eff(hide=True))
            props.append(p); idn += 1
    inst.properties = props
    inst.instances = [SymbolProjectInstance(name=PROJECT,
        paths=[SymbolProjectPath(sheetInstancePath="/" + ROOT_UUID,
                                 reference=ref, unit=1)])]
    placed.append(inst)
    pmap = {str(n): (r2(x + px), r2(y - py), ang) for n, (px, py, ang) in coords.items()}
    sym_pins[ref] = pmap; sym_used[ref] = set()
    def pinpos(num):
        sym_used[ref].add(str(num))
        return pmap[str(num)]
    pinpos.ref = ref
    return pinpos

STUB = 5.08
_OUT = {0: ((-1, 0), 180), 180: ((1, 0), 0), 270: ((0, -1), 90), 90: ((0, 1), 270)}

def net(name, *pins):
    for (x, y, ang) in pins:
        (dx, dy), lang = _OUT[int(ang)]
        ex, ey = r2(x + dx * STUB), r2(y + dy * STUB)
        w = Connection(type="wire", points=[Position(x, y), Position(ex, ey)])
        w.uuid = newuuid(f"w-{name}")
        wires.append(w)
        gl = GlobalLabel(text=name, shape="bidirectional",
                         position=Position(ex, ey, lang), effects=eff())
        gl.uuid = newuuid(f"gl-{name}")
        labels.append(gl)

# === footprint strings (project.toml) ======================================
FP_R   = "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P7.62mm_Horizontal"
FP_C1  = "Capacitor_THT:CP_Radial_D8.0mm_P3.50mm"       # 220uF
FP_CP  = "Capacitor_THT:CP_Radial_D5.0mm_P2.00mm"       # 10uF electrolytic
FP_CD5 = "Capacitor_THT:C_Disc_D5.1mm_W3.2mm_P5.00mm"   # 100nF / 1uF
FP_C10 = "Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P2.50mm"   # 10nF
FP_HDR = "Connector_PinHeader_2.54mm:PinHeader_1x%02d_P2.54mm_Vertical"

# === place components =======================================================
# --- Victor gold-finger edge connector (left) ---
GF1 = add_symbol(f"{NICK}:Victor 9K Expansion Port", GF1_SYM,
    "GF1", "Victor 9K Expansion Port", 70, 180,
    footprint=f"{NICK}:Victor_Gold_Fingers_3", pin_type="passive",
    fields={"Note": "PCB edge connector; fingers 15 (NMI), 16 (IRQ), 34 (+12V) NC - same as rev 1"})

# --- WeAct RP2350B Core module (center) ---
U1 = add_symbol(f"{NICK}:WeAct_RP2350B_Core", WEACT_SYM,
    "U1", "WeAct RP2350B Core", 175, 165,
    footprint=f"{NICK}:WeAct_RP2350B_Core_Socket",
    fields={"MPN": "WeAct RP2350B Core Board (16MB flash + 8MB PSRAM)",
            "Note": "socketed 2x(2x15)+1x4 SWD, project-canonical pin numbering; "
                    "GP0 pad = on-module PSRAM CS (QMI XIP_CS1n) - MUST stay NC; "
                    "VBUS(30) NC; module prep: remove GP23/GP25 0R user-key/LED links"})
def U1n(name):
    return U1(_wn[name])

# --- ESP32-S3-WROOM-1U (right) ---
U3 = add_symbol("RF_Module:ESP32-S3-WROOM-1",
    ("RF_Module.kicad_sym", "ESP32-S3-WROOM-1"),
    "U3", "ESP32-S3-WROOM-1U-N16R8", 300, 165,
    footprint="RF_Module:ESP32-S3-WROOM-1U",
    fields={"MPN": "ESP32-S3-WROOM-1U-N16R8",
            "Note": "population optional (UI lives on U5 expander); octal PSRAM: "
                    "IO35/36/37 (pins 28/29/30) reserved, left NC; FSPI slave link "
                    "to RP2350B; TXD0/RXD0 to J7 (external flash/console) and "
                    "JP1/JP2 (shunts = RP2350 UART0 flash link)"})

# --- U5 MCP23S17 SPI GPIO expander (UI + ESP control; works w/o U3) ---
U5 = add_symbol("Interface_Expansion:MCP23S17x-x-SP",
    ("Interface_Expansion.kicad_sym", "MCP23S17x-x-SP"),
    "U5", "MCP23S17", 520, 160,
    footprint="Package_DIP:DIP-28_W7.62mm",
    fields={"MPN": "MCP23S17-E/SP",
            "Note": "SPI1 CS=GP47 (<=10MHz); GPA=encoder/LED/ESP ctrl, "
                    "GPB=OLED CS/DC/RST + J10 spares; A0-A2=GND, INTA/INTB NC"})
R4 = add_symbol("Device:R", ("Device.kicad_sym", "R"), "R4", "10k", 490, 60,
    footprint=FP_R, fields={"MPN": "MFR-25FRF52-10K", "Note": "MCP23S17 RESET pullup"})
C11 = add_symbol("Device:C", ("Device.kicad_sym", "C"), "C11", "100nF", 560, 60,
    footprint=FP_CD5, fields={"Note": "U5 VDD decoupling"})

# --- RESET -> inverter -> RUN ---
IC1 = add_symbol(f"{NICK}:SN74LVC1G04QDCKRQ1", IC1_SYM,
    "IC1", "SN74LVC1G04", 120, 90,
    footprint=f"{NICK}:SOT65P210X110-5N",
    fields={"MPN": "SN74LVC1G04DCKR", "Note": "RESET (GF1-13) inverted -> module RUN"})
C9 = add_symbol("Device:C", ("Device.kicad_sym", "C"), "C9", "100nF", 150, 90,
    footprint=FP_CD5, fields={"Note": "IC1 VCC decoupling (3V3_WEACT)"})

# --- power in: Victor +5V -> D1 -> +5V_RAIL -> U4 LD1117V33 -> +3V3_PERI ---
D1 = add_symbol("Device:D_Schottky", ("Device.kicad_sym", "D_Schottky"),
    "D1", "1N5822", 70, 330,
    footprint="Diode_THT:D_DO-201AD_P12.70mm_Horizontal",
    fields={"MPN": "1N5822-TP", "Note": "VICTOR_5V -> +5V_RAIL (blocks backfeed)"})
C1 = add_symbol("Device:C_Polarized", ("Device.kicad_sym", "C_Polarized"),
    "C1", "220uF", 100, 340, footprint=FP_C1,
    fields={"MPN": "ESE227M025AG3AA", "Note": "+5V_RAIL bulk"})
C2 = add_symbol("Device:C_Polarized", ("Device.kicad_sym", "C_Polarized"),
    "C2", "10uF", 120, 340, footprint=FP_CP)
U4 = add_symbol("Regulator_Linear:LD1117V33",
    ("Regulator_Linear.kicad_sym", "LD1117V33"),
    "U4", "LD1117V33", 160, 330,
    footprint="Package_TO_SOT_THT:TO-220-3_Vertical",
    fields={"MPN": "LD1117V33", "Note": "+3V3_PERI: ESP32 + SD + OLED + encoder"})
C3 = add_symbol("Device:C_Polarized", ("Device.kicad_sym", "C_Polarized"),
    "C3", "10uF", 195, 340, footprint=FP_CP,
    fields={"Note": "LD1117 VOUT stability"})

# --- ALE constant-current pulldown network (rev1 topology, rev2 refs) ---
R8 = add_symbol("Device:R", ("Device.kicad_sym", "R"), "R8", "470", 40, 90,
    footprint=FP_R, fields={"MPN": "MFR-25FTE52-470R", "Note": "+5V_RAIL -> clamp node"})
R7 = add_symbol("Device:R", ("Device.kicad_sym", "R"), "R7", "2.2k", 40, 130,
    footprint=FP_R, fields={"MPN": "MFR-25FTE52-2K2", "Note": "ALE constant-current pulldown to -12V"})
D2 = add_symbol("Diode:1N4148", ("Diode.kicad_sym", "1N4148"), "D2", "1N4148", 70, 90,
    footprint="Diode_THT:D_DO-35_SOD27_P7.62mm_Horizontal",
    fields={"Note": "clamp node -> ALE (K=ALE)"})
D3 = add_symbol("Diode:1N4148", ("Diode.kicad_sym", "1N4148"), "D3", "1N4148", 70, 130,
    footprint="Diode_THT:D_DO-35_SOD27_P7.62mm_Horizontal",
    fields={"Note": "clamp node -> GND (K=GND)"})
C10 = add_symbol("Device:C", ("Device.kicad_sym", "C"), "C10", "10nF", 40, 160,
    footprint=FP_C10, fields={"MPN": "C315C103K5R5TA", "Note": "-12V filter"})

# --- RN2 2.7k bussed pullups on RD/WR/DT_R to +5V_RAIL ---
RN2 = add_symbol("Device:R_Network03", ("Device.kicad_sym", "R_Network03"),
    "RN2", "2.7k", 130, 240, footprint="Resistor_THT:R_Array_SIP4",
    fields={"MPN": "4604X-101-272LF", "Note": "common=+5V_RAIL; RD/WR/DT_R pullups"})

# --- microSD push-pull + RN1 47k bussed pullups to +3V3_PERI ---
J3 = add_symbol(f"{NICK}:DM3AT-SF-PEJM5", J3_SYM,
    "J3", "microSD", 300, 330,
    footprint=f"{NICK}:HRS_DM3AT-SF-PEJM5",
    fields={"MPN": "DM3AT-SF-PEJM5", "Note": "SPI1 (shared with ESP32); SWA/SWB NC"})
RN1 = add_symbol(f"{NICK}:4609X-101-473LF", RN1_SYM,
    "RN1", "47k", 240, 330, footprint=f"{NICK}:4609X101473LF",
    fields={"MPN": "4609X-101-473LF", "Note": "pin1=common=+3V3_PERI; SD-line pullups"})
C7 = add_symbol("Device:C", ("Device.kicad_sym", "C"), "C7", "100nF", 345, 340,
    footprint=FP_CD5, fields={"Note": "SD VDD decoupling"})
C8 = add_symbol("Device:C_Polarized", ("Device.kicad_sym", "C_Polarized"),
    "C8", "10uF", 365, 340, footprint=FP_CP, fields={"Note": "SD VDD bulk"})

# --- ESP32 support ---
R2 = add_symbol("Device:R", ("Device.kicad_sym", "R"), "R2", "10k", 260, 70,
    footprint=FP_R, fields={"MPN": "MFR-25FRF52-10K", "Note": "ESP_EN pullup"})
R3 = add_symbol("Device:R", ("Device.kicad_sym", "R"), "R3", "10k", 290, 70,
    footprint=FP_R, fields={"MPN": "MFR-25FRF52-10K", "Note": "ESP_IO0 pullup"})
C6 = add_symbol("Device:C", ("Device.kicad_sym", "C"), "C6", "1uF", 260, 110,
    footprint=FP_CD5, fields={"MPN": "SR305E105ZARTR1", "Note": "ESP_EN reset delay"})
C4 = add_symbol("Device:C_Polarized", ("Device.kicad_sym", "C_Polarized"),
    "C4", "10uF", 340, 90, footprint=FP_CP, fields={"Note": "+3V3_PERI bulk @ ESP32"})
C5 = add_symbol("Device:C", ("Device.kicad_sym", "C"), "C5", "100nF", 360, 90,
    footprint=FP_CD5, fields={"Note": "+3V3_PERI decoupling @ ESP32"})

# --- headers ---
J4 = add_symbol("Connector_Generic:Conn_01x07",
    ("Connector_Generic.kicad_sym", "Conn_01x07"),
    "J4", "OLED SPI", 430, 90, footprint=FP_HDR % 7,
    fields={"Note": "1=GND 2=+3V3_PERI 3=SPI1_SCK 4=SPI1_MOSI "
                    "5=OLED_RST 6=OLED_DC 7=OLED_CS"})
J5 = add_symbol("Connector_Generic:Conn_01x05",
    ("Connector_Generic.kicad_sym", "Conn_01x05"),
    "J5", "ENCODER", 430, 160, footprint=FP_HDR % 5,
    fields={"Note": "1=A 2=B 3=SW 4=+3V3_PERI 5=GND"})
J6 = add_symbol("Connector_Generic:Conn_01x02",
    ("Connector_Generic.kicad_sym", "Conn_01x02"),
    "J6", "ACT LED", 430, 230, footprint=FP_HDR % 2,
    fields={"Note": "1=330R<-U5 GPA3 2=GND"})
R1 = add_symbol("Device:R", ("Device.kicad_sym", "R"), "R1", "330", 400, 230,
    footprint=FP_R, fields={"MPN": "RNMF14FTC330R", "Note": "activity LED series R"})
J7 = add_symbol("Connector_Generic:Conn_01x08",
    ("Connector_Generic.kicad_sym", "Conn_01x08"),
    "J7", "ESP32 BREAKOUT", 430, 300, footprint=FP_HDR % 8,
    fields={"Note": "1=GND 2=+3V3_PERI 3=EN 4=IO0 5=TXD0 6=RXD0 7=USB_D- 8=USB_D+"})
J8 = add_symbol("Connector_Generic:Conn_01x03",
    ("Connector_Generic.kicad_sym", "Conn_01x03"),
    "J8", "DEBUG UART", 200, 90, footprint=FP_HDR % 3,
    fields={"Note": "1=GND 2=TX(GP46) 3=RX(GP33); 3.3V"})
J9 = add_symbol("Connector_Generic:Conn_01x04",
    ("Connector_Generic.kicad_sym", "Conn_01x04"),
    "J9", "SWD", 240, 90, footprint=FP_HDR % 4,
    fields={"Note": "1=GND 2=SWCLK 3=SWDIO 4=3V3_WEACT (module H4 pads 61-64)"})
JP1 = add_symbol("Connector_Generic:Conn_01x02",
    ("Connector_Generic.kicad_sym", "Conn_01x02"),
    "JP1", "ESP FLASH TX", 275, 90, footprint=FP_HDR % 2,
    fields={"Note": "1=UART0_TX(GP46) 2=ESP_RXD0; shunt = RP2350 flashes U3. "
                    "Open shunts before using a USB-serial adapter on J8."})
JP2 = add_symbol("Connector_Generic:Conn_01x02",
    ("Connector_Generic.kicad_sym", "Conn_01x02"),
    "JP2", "ESP FLASH RX", 305, 90, footprint=FP_HDR % 2,
    fields={"Note": "1=UART0_RX(GP33) 2=ESP_TXD0; shunt = RP2350 flashes U3. "
                    "Open shunts before using a USB-serial adapter on J8."})
J10 = add_symbol("Connector_Generic:Conn_01x08",
    ("Connector_Generic.kicad_sym", "Conn_01x08"),
    "J10", "EXPANDER GPIO", 520, 300, footprint=FP_HDR % 8,
    fields={"Note": "1=GND 2=+3V3_PERI 3=XGPIO_A7 4=XGPIO_B3 5=XGPIO_B4 "
                    "6=XGPIO_B5 7=XGPIO_B6 8=XGPIO_B7"})

# --- PWR_FLAGs (ERC power sources; not in BOM) ---
FLG_5R = add_symbol("power:PWR_FLAG", ("power.kicad_sym", "PWR_FLAG"),
    "#FLG01", "PWR_FLAG", 100, 300, inbom=False)
FLG_V5 = add_symbol("power:PWR_FLAG", ("power.kicad_sym", "PWR_FLAG"),
    "#FLG02", "PWR_FLAG", 40, 300, inbom=False)
FLG_GND = add_symbol("power:PWR_FLAG", ("power.kicad_sym", "PWR_FLAG"),
    "#FLG03", "PWR_FLAG", 40, 380, inbom=False)
FLG_12N = add_symbol("power:PWR_FLAG", ("power.kicad_sym", "PWR_FLAG"),
    "#FLG04", "PWR_FLAG", 40, 200, inbom=False)

# === nets ===================================================================
# --- Victor gold fingers <-> RP2350B (rev1 net names; slashes removed) ---
# data / address bus
BUS = [("BD0",25,"GP1"),("BD1",26,"GP2"),("BD2",24,"GP3"),("BD3",27,"GP4"),
       ("BD4",23,"GP5"),("BD5",28,"GP6"),("BD6",22,"GP7"),("BD7",29,"GP8"),
       ("A8",6,"GP9"),("A9",45,"GP10"),("A10",5,"GP11"),("A11",46,"GP12"),
       ("A12",4,"GP13"),("A13",47,"GP14"),("A14",3,"GP15"),("A15",48,"GP16"),
       ("A16",2,"GP17"),("A17",49,"GP18"),("A18",1,"GP19"),("A19",50,"GP20")]
for name, finger, gpio in BUS:
    net(name, GF1(finger), U1n(gpio))
# control signals GF1 <-> module (all on their exact rev-1 GPIOs)
CTRL = [("RD",11,"GP21"),("WR",14,"GP22"),("DT_R",12,"GP23"),("HOLD",17,"GP25"),
        ("XACK",21,"GP26"),("EXTIO",30,"GP27"),("READY",41,"GP28"),
        ("HLDA",18,"GP29"),("CLK5",38,"GP30"),("CLK15B",40,"GP31"),
        ("IR4",43,"GP32"),("IR5",42,"GP34"),("IO_M",10,"GP35"),
        ("SSO",7,"GP36"),("DLATCH",33,"GP37"),("CSEN",19,"GP38"),
        ("PHASE2",20,"GP39"),("DEN",8,"GP40")]
for name, finger, gpio in CTRL:
    net(name, GF1(finger), U1n(gpio))

# --- ALE + constant-current pulldown / clamp network ---
# ALE: finger 9 + GP24 + R7(2.2k)->-12V + D2 cathode(clamp toward node)
net("ALE", GF1(9), U1n("GP24"), R7(1), D2(1))
# clamp node: R8(470)<-+5V_RAIL, D2 anode, D3 anode
net("ALE_CLAMP", R8(2), D2(2), D3(2))
net("-12V", GF1(32), R7(2), C10(1), FLG_12N(1))

# --- RESET -> IC1 inverter -> RUN ---
net("RESET", GF1(13), IC1(2))
net("RUN", IC1(4), U1n("RUN"))

# --- RN2 pullups (common -> +5V_RAIL): attach each element to its control net ---
net("DT_R", RN2(2))
net("WR",   RN2(3))
net("RD",   RN2(4))

# --- microSD (SPI1, shared with ESP32/expander/OLED) + RN1 pullups ---
net("SD_DAT2",   J3(1), RN1(2))
net("SD_CS",     J3(2), U1n("GP41"), RN1(3))
net("SPI1_MOSI", J3(3), U1n("GP43"), RN1(4), U3(19), U5(13), J4(4))
net("SPI1_SCK",  J3(5), U1n("GP42"), RN1(5), U3(20), U5(12), J4(3))
net("SPI1_MISO", J3(7), U1n("GP44"), RN1(7), U3(21), U5(14))
net("SD_DAT1",   J3(8), RN1(8))
# J3 shield pads P1..P4 (stacked at one point): wire one, mark the rest used
J3("P2"); J3("P3"); J3("P4")

# --- U5 MCP23S17 expander: SPI + control ---
net("MCP_CS",    U1n("GP47"), U5(11))
net("MCP_RESET", U5(18), R4(2))
# A0/A1/A2 to GND (in GND list below); INTA/INTB NC (auto)

# --- ESP32-S3 link + control (EN/IO0/DRDY via expander GPA5/GPA6/GPA4) ---
net("ESP_CS",    U1n("GP45"), U3(18))
net("ESP_DRDY",  U5(25), U3(17))
net("ESP_TXD0",  U3(37), J7(5), JP2(2))
net("ESP_RXD0",  U3(36), J7(6), JP1(2))
net("ESP_EN",    U3(3), U5(26), R2(2), C6(1), J7(3))
net("ESP_IO0",   U3(27), U5(27), R3(2), J7(4))
net("ESP_USB_D-", U3(13), J7(7))
net("ESP_USB_D+", U3(14), J7(8))

# --- UI on the expander: OLED / encoder / activity LED; debug UART on U1 ---
net("OLED_CS",  U5(1), J4(7))
net("OLED_DC",  U5(2), J4(6))
net("OLED_RST", U5(3), J4(5))
net("ENC_A",   U5(21), J5(1))
net("ENC_B",   U5(22), J5(2))
net("ENC_SW",  U5(23), J5(3))
net("LED_DRV", U5(24), R1(1))
net("ACT_LED", R1(2), J6(1))
net("UART0_TX", U1n("GP46"), J8(2), JP1(1))
net("UART0_RX", U1n("GP33"), J8(3), JP2(1))

# --- expander spares -> J10 ---
net("XGPIO_A7", U5(28), J10(3))
for i in range(3, 8):                       # GPB3..GPB7 = pins 4..8
    net(f"XGPIO_B{i}", U5(i + 1), J10(i + 1))

# --- SWD header (module H4) ---
net("SWCLK", U1n("SWCLK"), J9(2))
net("SWDIO", U1n("SWDIO"), J9(3))

# === power rails ============================================================
net("VICTOR_5V", GF1(36), GF1(37), D1(2), FLG_V5(1))
net("+5V_RAIL", D1(1), U1n("5V"), U4(3), C1(1), C2(1), RN2(1), R8(1), FLG_5R(1))
net("+3V3_PERI", U4(2), C3(1), U3(2), C4(1), C5(1), R2(1), R3(1), R4(1),
    U5(9), C11(1), J3(4), C7(1), C8(1), J4(2), J5(4), J7(2), J10(2), RN1(1))
net("3V3_WEACT", U1(55), U1(56), U1n("SWD_3V3"), IC1(5), C9(1), J9(4))

# === GND ====================================================================
GNDpts = [
    GF1(31), GF1(35), GF1(39), GF1(44),
    U1(27), U1(28), U1(57), U1(59), U1(60), U1n("SWD_GND"),
    U3(1), U3(40), U3(41),
    U4(1), IC1(3),
    U5(10), U5(15), U5(16), U5(17),
    J3(6), J3("P1"),
    J4(1), J5(5), J6(2), J7(1), J8(1), J9(1), J10(1),
    C1(2), C2(2), C3(2), C4(2), C5(2), C6(2), C7(2), C8(2), C9(2), C10(2), C11(2),
    D3(1),
    FLG_GND(1),
]
net("GND", *GNDpts)

# === no-connect flags on every intentionally unused pin =====================
for ref, pmap in sym_pins.items():
    for num, (px, py, ang) in pmap.items():
        if num not in sym_used[ref]:
            nc = NoConnect(position=Position(px, py, 0))
            nc.uuid = newuuid(f"nc-{ref}-{num}")
            noconns.append(nc)

# === section titles =========================================================
from kiutils.items.schitems import Text
def title(txt, x, y):
    t = Text(text=txt, position=Position(snap(x), snap(y), 0),
             effects=eff(3.0, justify="left"))
    t.uuid = newuuid(f"t-{txt}")
    sc.texts.append(t)

title("VICTOR 9000 GOLD-FINGER BUS", 40, 55)
title("RP2350B CONTROLLER (WeAct Core, socketed)", 150, 55)
title("ESP32-S3 WIFI (optional, FSPI-linked)", 280, 55)
title("POWER: Victor +5 -> D1 -> +5V_RAIL -> LD1117 +3V3_PERI", 35, 300)
title("microSD (SPI1, shared) + RN1 pullups", 240, 300)
title("UI: MCP23S17 EXPANDER + OLED / ENCODER / LED HEADERS", 400, 55)

# === finalize ===============================================================
sc.titleBlock = TitleBlock()
sc.titleBlock.title = "Victor 9000 DMA Card — rev 2 (WeAct RP2350B + ESP32-S3)"
sc.titleBlock.date = "2026-07-10"
sc.titleBlock.revision = "2"
sc.titleBlock.company = "Intergalactic Microsystems"
sc.titleBlock.comments = {
    1: "GP0 = module PSRAM CS (QMI XIP_CS1n) — socket pad kept NC",
    2: "Remove module GP23 user-key / GP25 user-LED 0R links (bus pins DT_R/HOLD here)",
    3: "UI (OLED/encoder/LED) + ESP EN/IO0/DRDY on MCP23S17 (CS=GP47, <=10MHz) — works with U3 unpopulated",
    4: "SPI1 shared: SD 25MHz / ESP32 26MHz / MCP23S17 10MHz / OLED — one owner at a time, per-CS baud",
}
sc.noConnects = noconns
sc.graphicalItems = wires
sc.libSymbols = list(embedded.values())
sc.schematicSymbols = placed
sc.globalLabels = labels
sc.sheetInstances = [HierarchicalSheetInstance(instancePath="/", page="1")]
sc.to_file(OUT)

# === project file (netclasses; based on emulator, patched) ==================
pro = json.load(open(EMU_PRO, encoding="utf-8"))
pro["meta"]["filename"] = f"{PROJECT}.kicad_pro"
pro["schematic"]["top_level_sheets"] = [
    {"filename": f"{PROJECT}.kicad_sch", "name": PROJECT, "uuid": ROOT_UUID}]
pro["sheets"] = [[ROOT_UUID, PROJECT]]
classes = pro["net_settings"]["classes"]
# add a Power12N class for the -12V rail (copy of Power3V3W, wider priority)
p12 = copy.deepcopy(next(c for c in classes if c["name"] == "Power3V3W"))
p12.update({"name": "Power12N", "track_width": 0.4, "priority": 3})
if not any(c["name"] == "Power12N" for c in classes):
    classes.append(p12)
pro["net_settings"]["netclass_patterns"] = [
    {"netclass": "Power5V",   "pattern": "+5V_RAIL"},
    {"netclass": "Power5V",   "pattern": "VICTOR_5V"},
    {"netclass": "Power3V3",  "pattern": "+3V3_PERI"},
    {"netclass": "Power3V3W", "pattern": "3V3_WEACT"},
    {"netclass": "Power12N",  "pattern": "-12V"},
]
json.dump(pro, open(PRO_OUT, "w", encoding="utf-8"), indent=2)

print("wrote", OUT)
print("wrote", PRO_OUT)
print("symbols:", len(placed), " labels:", len(labels),
      " libsyms:", len(embedded), " noconns:", len(noconns))
