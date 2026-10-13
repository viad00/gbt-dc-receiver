#!/usr/bin/env python3
"""GBT 27930-2015 CAN log decoder.

Decodes sequence_XXX.csv produced by flash_dump_to_csv.py.
Usage: python decode_gbt_log.py sequence_005.csv
"""

import csv
import sys
from collections import defaultdict

# ── Source addresses ──────────────────────────────────────────────────────────
SA_BMS     = 0xF4
SA_CHARGER = 0x56

# ── PGN definitions ──────────────────────────────────────────────────────────
PGN_CHM    = 0x002600
PGN_BHM    = 0x002700
PGN_CRM    = 0x000100
PGN_BRM    = 0x000200
PGN_BCP    = 0x000600
PGN_CML    = 0x000800
PGN_BRO    = 0x000900
PGN_CRO    = 0x000A00
PGN_BCL    = 0x001000
PGN_BCS    = 0x001100
PGN_CCS    = 0x001200
PGN_BSM    = 0x001300
PGN_BST    = 0x001900
PGN_CST    = 0x001A00
PGN_TP_CM  = 0x00EC00
PGN_TP_DT  = 0x00EB00

PGN_NAMES = {
    PGN_CHM: "CHM", PGN_BHM: "BHM", PGN_CRM: "CRM", PGN_BRM: "BRM",
    PGN_BCP: "BCP", PGN_CML: "CML", PGN_BRO: "BRO", PGN_CRO: "CRO",
    PGN_BCL: "BCL", PGN_BCS: "BCS", PGN_CCS: "CCS", PGN_BSM: "BSM",
    PGN_BST: "BST", PGN_CST: "CST", PGN_TP_CM: "TP.CM", PGN_TP_DT: "TP.DT",
}


def decode_pgn(can_id):
    """Extract PGN from 29-bit J1939 CAN ID."""
    pf = (can_id >> 16) & 0xFF
    ps = (can_id >> 8) & 0xFF
    dp = (can_id >> 24) & 0x01
    if pf < 240:
        return (dp << 16) | (pf << 8), ps      # PGN, dest
    return (dp << 16) | (pf << 8) | ps, None   # PGN, no dest


def decode_sa(can_id):
    return can_id & 0xFF


def src_name(sa):
    return "BMS" if sa == SA_BMS else ("CHR" if sa == SA_CHARGER else f"0x{sa:02X}")


def pgn_name(pgn):
    return PGN_NAMES.get(pgn, f"0x{pgn:06X}")


# ── BST/CST reason decoders ──────────────────────────────────────────────────
BST_3511 = {
    0x01: "SOC reached", 0x02: "Insulation fault", 0x04: "Output connector overtemp",
    0x08: "BMS/connector overtemp", 0x10: "Charging connector fault",
    0x20: "Battery temp too high", 0x40: "HV relay fault", 0x80: "Voltage detection fault",
}

BST_3512 = {
    0: "Insulation fault", 2: "Output connector overtemp", 4: "BMS/connector overtemp",
    6: "Charging connector fault", 8: "Battery temp too high", 10: "HV relay fault",
    12: "Voltage detection fault", 14: "Other fault / comm timeout",
}

BST_3513 = {
    0x01: "Voltage mismatch", 0x02: "Current mismatch",
}


def decode_bst_cst_reasons(d):
    """Decode BST/CST stop reasons from bytes [0..3]."""
    r3511 = d[0]
    r3512 = d[1] | (d[2] << 8)
    r3513 = d[3]
    parts = []
    if r3511:
        bits = [name for bit, name in BST_3511.items() if r3511 & bit]
        parts.append(f"3511: {'|'.join(bits) if bits else f'0x{r3511:02X}'}")
    if r3512:
        bits = [name for bit, name in BST_3512.items() if r3512 & (1 << bit)]
        parts.append(f"3512: {'|'.join(bits) if bits else f'0x{r3512:04X}'}")
    if r3513:
        bits = [name for bit, name in BST_3513.items() if r3513 & bit]
        parts.append(f"3513: {'|'.join(bits) if bits else f'0x{r3513:02X}'}")
    return "  ".join(parts) if parts else "NONE (graceful stop)"


# ── Single-frame message decoders ────────────────────────────────────────────
def u16(d, i):
    """Little-endian unsigned 16-bit from byte list."""
    return d[i] | (d[i+1] << 8)

def s16(d, i):
    """Little-endian signed 16-bit from byte list."""
    v = d[i] | (d[i+1] << 8)
    if v >= 0x8000: v -= 0x10000
    return v

def decode_chm(d):
    """CHM: Charger protocol version."""
    ver = f"{d[2]}.{d[1]}.{d[0]}"
    return f"proto_ver={ver}"


def decode_bhm(d):
    """BHM: BMS max charging voltage."""
    vmax = u16(d, 0)
    return f"max_charge_V={vmax/10:.1f}"


def decode_crm(d):
    """CRM: Charger recognition."""
    confirm = d[0]
    chg_cap = u16(d, 1)
    chg_sn = bytes(d[4:8]).decode('ascii', errors='replace')
    return f"confirm={'0xAA (YES)' if confirm == 0xAA else f'0x{confirm:02X} (NO)'}  capacity_dV={chg_cap}  SN={chg_sn}"


def decode_cml(d):
    """CML: Charger max limits.

    Per GB/T 27930 Table 14:
      SPN2824 [0:1] max output voltage  0.1V/bit, 0 offset
      SPN2825 [2:3] min output voltage  0.1V/bit, 0 offset
      SPN2826 [4:5] max output current  0.1A/bit, -400A offset
      SPN2827 [6:7] min output current  0.1A/bit, -400A offset
    """
    vmax = u16(d, 0) / 10.0           # 0.1V/bit
    vmin = u16(d, 2) / 10.0           # 0.1V/bit
    imax = (u16(d, 4) - 4000) / 10.0  # 0.1A/bit, -400A offset
    imin = (u16(d, 6) - 4000) / 10.0  # 0.1A/bit, -400A offset
    return f"Vmax={vmax:.1f}V  Vmin={vmin:.1f}V  Imax={imax:.1f}A  Imin={imin:.1f}A"


def decode_bro(d):
    """BRO: BMS ready."""
    return {0xAA: "READY", 0x00: "NOT READY"}.get(d[0], f"0x{d[0]:02X}")


def decode_cro(d):
    """CRO: Charger ready."""
    return {0xAA: "READY", 0x00: "NOT READY", 0x01: "REJECTED"}.get(d[0], f"0x{d[0]:02X}")


def decode_bcl(d):
    """BCL: Battery charge demand. Table 17:
      [0:1] V demand 0.1V/bit  [2:3] I demand 0.1A/bit -400A  [4] mode 01=CV 02=CC"""
    vdem = u16(d, 0) / 10.0
    idem = (u16(d, 2) - 4000) / 10.0
    mode = {1: "CV", 2: "CC"}.get(d[4], f"0x{d[4]:02X}")
    return f"Vdemand={vdem:.1f}V  Idemand={idem:.1f}A  mode={mode}"


def decode_ccs(d):
    """CCS: Charger output status. Table 19:
      [0:1] Vout 0.1V  [2:3] Iout 0.1A -400A  [4:5] charge time min
      [6] bits0-1: permit (00=pause, 01=allow)"""
    vout = u16(d, 0) / 10.0
    iout = (u16(d, 2) - 4000) / 10.0
    chg_min = u16(d, 4)
    permit = d[6] & 0x03
    permit_s = {0: "PAUSE", 1: "ALLOW"}.get(permit, f"0x{permit}")
    return f"Vout={vout:.1f}V  Iout={iout:.1f}A  permit={permit_s}  chg_min={chg_min}"


def decode_bsm(d):
    """BSM: Battery status. Table 20:
      [0] max cell idx  [1] Tmax+50  [2] Tmax idx  [3] Tmin+50  [4] Tmin idx
      [6] bits4-5: SPN3096 permit (00=forbid, 01=allow)"""
    tmax = d[1] - 50
    tmin = d[3] - 50
    permit = (d[6] >> 4) & 0x03
    permit_s = {0: "FORBID", 1: "ALLOW"}.get(permit, f"0x{permit}")
    return f"cell_max#={d[0]}  Tmax={tmax}°C(@{d[2]})  Tmin={tmin}°C(@{d[4]})  permit={permit_s}"


def decode_cst(d):
    """CST: Charger stop."""
    return decode_bst_cst_reasons(d)


def decode_bst(d):
    """BST: BMS stop."""
    return decode_bst_cst_reasons(d)


# ── Transport Protocol reassembly ────────────────────────────────────────────
class TPTracker:
    """Track TP.CM/TP.DT sessions for multi-frame BMS→Charger messages.

    Key insight: RTS comes from the DATA SENDER, but CTS/EOM come from
    the DATA RECEIVER. We key by (pgn, data_sender) so RTS, DT, and EOM
    all find the same session regardless of which direction the control
    frames travel.
    """

    def __init__(self):
        self.active = {}  # key: (pgn, data_sender_sa) -> session info

    def _sender_key(self, pgn):
        """Return the key for a session by its PGN + data sender.

        BMS sends BRM/BCP/BCS/BST, charger sends CRM/CML/CCS/CST.
        TRP.CM/TP.DT tx_dir is stored during RTS.
        """
        for k, sess in self.active.items():
            if sess['pgn'] == pgn:
                return k
        return None

    def handle_cm(self, ts, src, d):
        """Process a TP.CM frame. Returns decoded result or None.
        src = sender of this TP.CM frame.
        """
        cm_type = d[0]

        if cm_type == 0x10:  # RTS
            total = d[1] | (d[2] << 8)
            pgn = d[5] | (d[6] << 8) | (d[7] << 16)
            # RTS sender IS the data sender
            key = (pgn, src)
            self.active[key] = {
                'pgn': pgn, 'total': total, 'buf': bytearray(), 'ts': ts,
                'data_src': src,
            }
            return None

        elif cm_type == 0x11:  # CTS – find session by matching any active RTS
            # CTS comes from the receiver. Look for an active session
            # that hasn't sent EOM yet (just started or mid-transfer).
            return None

        elif cm_type == 0x13:  # End of message
            tpgn = d[5] | (d[6] << 8) | (d[7] << 16)
            key = self._sender_key(tpgn)
            if key and key in self.active:
                sess = self.active.pop(key)
                pgn = sess['pgn']
                data = bytes(sess['buf'][:sess['total']])
                return (pgn, data)
            return None

        elif cm_type == 0xFF:  # Abort
            tpgn = d[5] | (d[6] << 8) | (d[7] << 16) if len(d) >= 8 else 0
            key = self._sender_key(tpgn)
            if key:
                self.active.pop(key, None)
            return None

        return None

    def handle_dt(self, ts, src, d):
        """Process a TP.DT frame. src is the data sender, which matches RTS sender.
        Returns None (data appended to buffer, EOM triggers final decode).
        """
        # DT sender matches the data_src stored at RTS
        for key, sess in self.active.items():
            if sess['data_src'] == src:
                sess['buf'].extend(d[1:8])
                return None
        # If no exact match found, try all active sessions (shouldn't happen)
        return None


# ── Multi-frame decoders ─────────────────────────────────────────────────────
def decode_brm(d):
    """BRM: Battery identification (multi-frame).

    Layout per build_and_send_BRM (49 bytes):
      [0]     protocol ver major (0x01)
      [1]     protocol ver minor (0x01)
      [2]     protocol ver patch (0x00)
      [3]     battery type
      [4:5]   rated_Ah_0p1 (Ah/10)
      [6:7]   rated_V_dV (dV)
      [8:11]  manufacturer (4 ASCII)
      [12:15] pack_code (4 ASCII)
      [16]    year offset from 1985
      [17]    month (BCD)
      [18]    day (BCD)
      [19:21] plant (3 ASCII)
      [22]    ownership code
      [24:40] VIN (17 ASCII)
      [41:48] SW version (8 ASCII)
    """
    if len(d) < 49:
        return f"[short {len(d)}B]"

    proto = f"{d[0]}.{d[1]}.{d[2]}"
    batt_type = d[3]
    rated_ah = u16(d, 4) / 10.0      # 0.1Ah
    rated_v = u16(d, 6) / 10.0       # dV
    mfr = bytes(d[8:12]).decode('ascii', errors='replace')
    pack_code = bytes(d[12:16]).decode('ascii', errors='replace')
    prod_year = d[16] + 1985
    prod_month = ((d[17] >> 4) & 0xF) * 10 + (d[17] & 0xF)
    prod_day = ((d[18] >> 4) & 0xF) * 10 + (d[18] & 0xF)
    vin = bytes(d[24:41]).decode('ascii', errors='replace').rstrip('\x00').rstrip('\xff')
    sw_ver = bytes(d[41:49]).decode('ascii', errors='replace').rstrip('\x00').rstrip('\xff')
    return (f"MFR={mfr}  Pack={pack_code}  VIN={vin}  "
            f"Type={batt_type}  Rated={rated_ah}Ah/{rated_v}V  "
            f"Date={prod_year}-{prod_month:02d}-{prod_day:02d}  "
            f"SW={sw_ver}  Proto={proto}")


def decode_bcp(d):
    """BCP: Battery charge parameters (multi-frame).

    Layout (per build_and_send_BCP):
      [0:1]  encode_cell_0p01V(maxCell_mV) → 0.01V, 12-bit
      [2:3]  encode_current_off400(maxChargeCurrent_dA) → 1A, offset -400A
      [4:5]  nominalEnergy_0p1kWh → 0.1kWh
      [6:7]  maxChargeVoltage_dV → 0.1V
      [8]    maxTemp_C + 50
      [9:10] soc_percent * 10 → 0.1%
      [11:12] packVoltage_dV → 0.1V
    """
    cell_raw = u16(d, 0) & 0x0FFF      # 12-bit, 0.01V
    imax_raw = u16(d, 2)               # 1A/bit, -400A offset
    energy   = u16(d, 4)               # 0.1kWh
    vmax     = u16(d, 6)               # 0.1V
    tmax     = d[8] - 50                # °C, -50 offset
    soc      = u16(d, 9)               # 0.1%
    pack_v   = u16(d, 11)              # 0.1V, if present
    return (f"CellMax={cell_raw/100:.2f}V  Imax={imax_raw - 400}A  "
            f"Energy={energy/10:.1f}kWh  Vmax={vmax/10:.1f}V  "
            f"Tmax={tmax}°C  SOC={soc/10:.1f}%  PackV={pack_v/10:.1f}V")


def decode_bcs(d):
    """BCS: Battery charge status (multi-frame).

    Layout per build_and_send_BCS (9 bytes):
      [0:1]  packVoltage_dV → 0.1V
      [2:3]  encode_current_off400(packCurrent_dA) → 1A, offset -400A
      [4:5]  (maxCellGroup<<12) | cell_mV/10 → 0.01V, 12-bit + 4-bit group
      [6]    soc_percent → 1%
      [7:8]  remaining_min
    """
    if len(d) < 9:
        return f"[short {len(d)}B]"
    pack_v   = u16(d, 0)          # dV → 0.1V
    iout     = u16(d, 2) - 400    # 1A/bit, -400A offset
    cell_raw = u16(d, 4)
    max_cell = cell_raw & 0x0FFF  # 12-bit → 0.01V
    max_grp  = (cell_raw >> 12) & 0xF
    soc      = d[6]               # 1%
    remain   = u16(d, 7)          # minutes
    return (f"Vpack={pack_v/10:.1f}V  Iout={iout}A  "
            f"CellMax#={max_grp}={max_cell/100:.2f}V  "
            f"SOC={soc}%  Remain={remain}min")


TP_PGN_DECODERS = {
    PGN_BRM: ("BRM", decode_brm),
    PGN_BCP: ("BCP", decode_bcp),
    PGN_BCS: ("BCS", decode_bcs),
}


# ── Main decoder ─────────────────────────────────────────────────────────────
SINGLE_DECODERS = {
    PGN_CHM: ("CHM", decode_chm),
    PGN_BHM: ("BHM", decode_bhm),
    PGN_CRM: ("CRM", decode_crm),
    PGN_CML: ("CML", decode_cml),
    PGN_BRO: ("BRO", decode_bro),
    PGN_CRO: ("CRO", decode_cro),
    PGN_BCL: ("BCL", decode_bcl),
    PGN_CCS: ("CCS", decode_ccs),
    PGN_BSM: ("BSM", decode_bsm),
    PGN_BST: ("BST", decode_bst),
    PGN_CST: ("CST", decode_cst),
}


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <sequence_XXX.csv> [--brief]")
        sys.exit(1)

    fname = sys.argv[1]
    brief = "--brief" in sys.argv

    tp = TPTracker()
    state_changes = []  # track key transitions

    print(f"{'TS':>8}  {'Dir':>3}  {'PGN':5}  {'Src':3}  {'Decoded'}")
    print("─" * 90)

    with open(fname, newline='') as f:
        reader = csv.reader(f)
        header = next(reader)  # skip header

        for row in reader:
            ts = int(row[0])
            can_id = int(row[1], 16)
            d = [int(x, 16) for x in row[6:14]]  # D1..D8

            pgn, dest = decode_pgn(can_id)
            src = decode_sa(can_id)
            direction = "Tx" if row[3].strip() == "Tx" else "Rx"
            src_label = src_name(src)
            name = pgn_name(pgn)

            # TP.CM
            if pgn == PGN_TP_CM:
                cm_type = d[0]
                cm_names = {0x10: "RTS", 0x11: "CTS", 0x13: "EOM", 0xFF: "ABORT"}
                cm_label = cm_names.get(cm_type, f"0x{cm_type:02X}")

                if brief and cm_type == 0x11:
                    continue  # skip CTS in brief mode

                extra = ""
                if cm_type == 0x10:
                    total = d[1] | (d[2] << 8)
                    tpgn = d[5] | (d[6] << 8) | (d[7] << 16)
                    extra = f"  pgn={pgn_name(tpgn)}  total={total}B"
                    tp.handle_cm(ts, src, d)
                elif cm_type == 0x13:
                    tpgn = d[5] | (d[6] << 8) | (d[7] << 16)
                    extra = f"  pgn={pgn_name(tpgn)}"
                    tp_result = tp.handle_cm(ts, src, d)
                    if tp_result:
                        t_pgn, t_data = tp_result
                        t_name, t_decoder = TP_PGN_DECODERS.get(t_pgn, (None, None))
                        if t_decoder:
                            extra += f"  → {t_decoder(t_data)}"
                else:
                    tp.handle_cm(ts, src, d)

                print(f"{ts:>8}  {direction:>3}  TP.{cm_label:4s}  {src_label:3}{extra}")
                continue

            # TP.DT
            if pgn == PGN_TP_DT:
                tp.handle_dt(ts, src, d)
                if brief:
                    continue
                print(f"{ts:>8}  {direction:>3}  TP.DT   {src_label:3}  seq={d[0]}")
                continue

            # Skip raw broadcast
            if can_id == 0x4144414C:
                if not brief:
                    raw = ' '.join(f"{x:02X}" for x in d)
                    print(f"{ts:>8}  {direction:>3}  RAW     ---  {raw}")
                continue

            # Single-frame messages
            dec_info = SINGLE_DECODERS.get(pgn)
            if dec_info:
                name, decoder = dec_info
                decoded = decoder(d)
                print(f"{ts:>8}  {direction:>3}  {name:5s}  {src_label:3}  {decoded}")
            else:
                raw = ' '.join(f"{x:02X}" for x in d)
                print(f"{ts:>8}  {direction:>3}  {name:5s}  {src_label:3}  [raw] {raw}")

    # ── Summary ────────────────────────────────────────────────────────────
    print("\n" + "═" * 90)
    print("SUMMARY: Charging session analysis")
    print("═" * 90)


if __name__ == "__main__":
    main()