"""LCSC / JLCPCB part numbers, keyed by the Value field used on the board.

Fill in the `lcsc` entries and re-run jlc.py. Anything left blank comes out as
TBD in the assembly BOM so it is impossible to miss.
"""
# License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.

PARTS = {
    # value          lcsc      spec to search LCSC for
    "BUS":          ("-", "2x8 2.54mm shrouded/keyed box header, vertical THT"),
    "PWR IN":       ("-", "Molex Mini-Fit Jr 5569 2x3 right-angle, 4.2mm (39-30-1060)"),
    "1.1A PTC":     ("C210835", "Bourns MF-MSMF110/24X-2, 1.1A hold/2.2A trip, 24V, 60mOhm, 1812"),
    "0R5 PTC":      ("C163512", "Littelfuse 1206L050YR, 500mA hold/1A trip, 6V, 150mOhm, 100ms, 1206"),
    "2.6A PTC":     ("C2760294", "BNstar SMD1812-260C-16V, 2.6A hold/5.2A trip, 16V, 15mOhm, 1812"),
    "100n":         ("C49678", "YAGEO CC0805KRX7R9BB104, 100nF 50V X7R +/-10%, 0805"),
    "220u/25V":     ("-", "220uF 25V low-ESR radial electrolytic, 8mm dia, 3.5mm pitch"),
    "SS54":         ("C3014039", "FOSAN SS54C, Schottky 5A 40V Vf=0.55V@5A, IFSM 150A, SMC/DO-214AB"),
    "SMBJ13A":      ("C19077567", "R+O SMBJ13A, uni TVS 13V standoff / 15.9V breakdown / 21.5V clamp, 600W, SMB"),
    "SMBJ6.0A":     ("C19077560", "R+O SMBJ6.0A, uni TVS 6V standoff / 7.37V breakdown / 10.3V clamp, 600W, SMB"),
    "LED_RED":      ("C84256", "NATIONSTAR NCD0805R1, red 630nm, Vf 1.6-2.6V, 195mcd@25mA, 130deg, 0805"),
    "LED_GRN":      ("C84260", "NATIONSTAR NCD0805G1, green 520nm, Vf 2.5-3.6V, 400mcd@20mA, 0805"),
    "LED_YEL":      ("C84261", "NATIONSTAR NCD0805Y1, yellow 595nm, Vf 1.6-2.6V, 180mcd@25mA, 130deg, 0805"),
    "AO3401A":      ("C347476", "UMW AO3401A, P-ch MOSFET 30V 4.2A, RDS(on) 50mOhm@10V, Vgs(th) 1.3V, SOT-23"),
    "100k":         ("C2907293", "FOJAN FRC0805J104 TS, 100k 5% 125mW thick film, 0805"),
    "3k9":          ("C7319412", "YAGEO SR0805JR-473K9L, 3.9k 5% 500mW anti-surge, 0805"),
    "1k":           ("C2907295", "FOJAN FRC0805J102 TS, 1k 5% 125mW thick film, 0805"),
    # not fitted / no part
    "IN_N12": ("-", "solder pad, no part"), "IN_P12": ("-", "solder pad, no part"),
    "IN_P5": ("-", "solder pad, no part"), "GND": ("-", "solder pad, no part"),
    "M3": ("-", "mounting hole, no part"),
}


def lcsc(value):
    return PARTS.get(value, ("", ""))[0]


def spec(value):
    return PARTS.get(value, ("", ""))[1]
