# UART CLI — Runtime Field Commands

All commands are sent over **UART4** (`PA0` TX, `PA1` RX) at **115200 8N1**.
Each command must be terminated with `\n` or `\r`.

---

## Commands

### `STATE`

Returns the current GB/T 27930 charge state as a short code.

```text
STATE
STATE=CHG
```

Possible values: `WAIT`, `BHM`, `BRM`, `BCP`, `BRO`, `CHG`, `END`.

### `RLIST`

Lists every runtime field with its current value.

```text
RLIST
--- Runtime fields ---
  DEMAND_VOLTAGE=3931
  DEMAND_CURRENT=0
  MODE=1
  PACK_VOLTAGE=3500
  PACK_CURRENT=0
  ...
```

### `RGET <FIELD>`

Reads a single field.

```text
RGET SOC_PERCENT
SOC_PERCENT=50
```

If the field name is not recognised the device replies `UNKNOWN FIELD`.

### `RSET <FIELD> <value>`

Writes a value to a field.  The value is automatically **clamped** to the
allowed range shown in the table below.  On success the device echoes:

```text
SET DEMAND_CURRENT=100
```

If the field name is not recognised the device replies `UNKNOWN FIELD`.
If the arguments are malformed the device replies `USAGE: RSET <FIELD> <value>`.

> **Note:** Field names are **case-sensitive** and must be entered exactly as
> shown in the table (all uppercase, underscores).

---

## Field Reference

### Demand & Pack values

| Field name             | Unit      | Allowed range      | Default | Description |
|------------------------|-----------|--------------------|---------|-------------|
| `DEMAND_VOLTAGE`       | 0.1 V (dV) | 0 – 10000       | 3931    | Target output voltage sent to charger in BCL. 3931 dV = 393.1 V. |
| `DEMAND_CURRENT`       | 0.1 A (dA) | −4000 – 4000     | 0       | Target output current sent to charger in BCL. Positive = charge, negative = discharge direction. 100 dA = 10.0 A. |
| `PACK_VOLTAGE`         | 0.1 V (dV) | 0 – 10000       | 3500    | Battery pack voltage reported to charger in BCS. Also updated from charger CCS messages during charging. 3500 dV = 350.0 V. |
| `PACK_CURRENT`         | 0.1 A (dA) | −4000 – 4000     | 0       | Battery pack current reported to charger in BCS. Also updated from charger CCS messages during charging. |
| `MODE`                 | —         | 1 or 2             | 1       | Charging mode sent in BCL. `1` = CV (constant voltage), `2` = CC (constant current). Any value other than 2 is treated as 1. |

### Tail‑request thresholds

The firmware monitors pack voltage and current during charging.  When the
pack voltage **exceeds** `TAIL_SWITCH_VOLTAGE` **and** the absolute current
drops **below** `TAIL_SWITCH_CURRENT`, the demand is automatically switched
to `TAIL_VOLTAGE` / `TAIL_CURRENT` **once** for the remainder of the charge
session.

| Field name             | Unit        | Allowed range   | Default | Description |
|------------------------|-------------|-----------------|---------|-------------|
| `TAIL_SWITCH_VOLTAGE`  | 0.1 V (dV)  | 0 – 10000     | 3920    | Voltage above which tail‑request logic activates. 3920 dV = 392.0 V (~4.08 V/cell on a 96‑cell pack). |
| `TAIL_SWITCH_CURRENT`  | 0.1 A (dA)  | −4000 – 4000   | 70      | Current threshold (absolute value) below which tail‑request activates. 70 dA = 7.0 A. |
| `TAIL_VOLTAGE`         | 0.1 V (dV)  | 0 – 10000     | 408     | Demand voltage applied after tail switch. 408 dV = 40.8 V (example: single‑string adjustment; adjust for your pack). |
| `TAIL_CURRENT`         | 0.1 A (dA)  | −4000 – 4000   | −70     | Demand current applied after tail switch. −70 dA = −7.0 A (reduced charge current for CV taper). |

> **Tip:** To disable the automatic tail request, set `TAIL_SWITCH_VOLTAGE`
> higher than the maximum achievable pack voltage (e.g. 10000).

### Cell & temperature reporting

These fields control what the BMS **reports** to the charger.  They are
included in BCS and BSM messages and do **not** affect the charge control
logic itself.

| Field name         | Unit    | Allowed range | Default | Description |
|--------------------|---------|---------------|---------|-------------|
| `MAX_CELL_MV`      | mV      | 0 – 24000    | 4000    | Highest single‑cell voltage in millivolts. 4000 mV = 4.000 V. Encoded into BCS as 0.01 V with a 4‑bit group index. |
| `MAX_CELL_GROUP`   | —       | 0 – 15        | 0       | Cell group (4‑bit) associated with `MAX_CELL_MV`. Used in the upper nibble of BCS byte 5. |
| `SOC_PERCENT`      | %       | 0 – 100       | 50      | State‑of‑charge percentage reported in BCS byte 7. Can be adjusted at runtime via the LCD or CLI. |
| `REMAINING_MIN`    | min     | 0 – 600       | 120     | Estimated remaining charge time in minutes, reported in BCS bytes 8‑9. |
| `MAX_CELL_INDEX`   | —       | 0 – 255       | 1       | Index of the cell with the highest voltage, reported in BSM byte 1. |
| `TEMP_MAX_C`       | °C      | −50 – 120     | 30      | Highest cell temperature, reported in BSM byte 2. Encoded as `value + 50` on the wire. |
| `TEMP_MAX_INDEX`   | —       | 0 – 255       | 1       | Sensor index of the highest temperature, reported in BSM byte 3. |
| `TEMP_MIN_C`       | °C      | −50 – 120     | 25      | Lowest cell temperature, reported in BSM byte 4. Encoded as `value + 50` on the wire. |
| `TEMP_MIN_INDEX`   | —       | 0 – 255       | 2       | Sensor index of the lowest temperature, reported in BSM byte 5. |

### Charge control

| Field name       | Allowed values | Default | Description |
|------------------|----------------|---------|-------------|
| `PERMIT_CHARGE`  | 0 or 1         | 1       | Charge‑permission flag sent in BSM byte 7 bits 4‑5. `1` = charging allowed, `0` = charging not permitted. Setting to 0 will cause the BMS to signal the charger to stop. |

---

## Unit Summary

| Abbreviation | Meaning | Conversion example |
|--------------|---------|-------------------|
| dV | deci‑volts (0.1 V) | 3931 dV = 393.1 V |
| dA | deci‑amps (0.1 A) | 100 dA = 10.0 A |
| mV | millivolts | 4200 mV = 4.200 V |
| % | percent | 50 = 50 % |
| min | minutes | 120 = 2 hours |

---

## Examples

```text
> RLIST
--- Runtime fields ---
  DEMAND_VOLTAGE=3931
  DEMAND_CURRENT=0
  MODE=1
  PACK_VOLTAGE=3500
  PACK_CURRENT=0
  TAIL_SWITCH_VOLTAGE=3920
  TAIL_SWITCH_CURRENT=70
  TAIL_VOLTAGE=408
  TAIL_CURRENT=-70
  MAX_CELL_MV=4000
  MAX_CELL_GROUP=0
  SOC_PERCENT=50
  REMAINING_MIN=120
  MAX_CELL_INDEX=1
  TEMP_MAX_C=30
  TEMP_MAX_INDEX=1
  TEMP_MIN_C=25
  TEMP_MIN_INDEX=2
  PERMIT_CHARGE=1

> RGET DEMAND_VOLTAGE
DEMAND_VOLTAGE=3931

> RSET DEMAND_CURRENT 100
SET DEMAND_CURRENT=100

> RSET MODE 2
SET MODE=2

> RSET TAIL_SWITCH_VOLTAGE 10000
SET TAIL_SWITCH_VOLTAGE=10000

> RSET SOC_PERCENT 75
SET SOC_PERCENT=75
```

---

## Clamping Behaviour

All values are **silently clamped** to the allowed range before being stored.
For example:

```text
> RSET DEMAND_CURRENT 5000
SET DEMAND_CURRENT=4000          ← clamped to maximum

> RSET SOC_PERCENT -10
SET SOC_PERCENT=0                ← clamped to minimum

> RSET TAIL_SWITCH_VOLTAGE 99999
SET TAIL_SWITCH_VOLTAGE=10000    ← clamped to maximum
```

No error is reported; the echoed value reflects the clamped result.

---

## Tips

- Use `RLIST` first to see all current values before making changes.
- To **disable** automatic tail request, set `TAIL_SWITCH_VOLTAGE` to its maximum (`10000`).
- Setting `PERMIT_CHARGE` to `0` will cause the BMS to tell the charger to stop charging on the next BSM frame.
- `DEMAND_VOLTAGE` and `DEMAND_CURRENT` can be changed freely during a charge session to steer the charger in real time.
- The `PACK_VOLTAGE` and `PACK_CURRENT` fields are **overwritten** by incoming charger CCS messages during an active charge session, so setting them via CLI only takes effect before charging starts.
- The `MODE` field accepts only `1` (CV) or `2` (CC); any other value defaults to `1`.
