# gbt-receiver

Project for an STM32F405-based board that implements a GB/T 27930 charge receiver.

## Overview

This project implements a GB/T 27930 BMS charge-state machine and CAN communication helper on an STM32F405 microcontroller. It is designed to receive BMS messages, respond to charger requests with predefined values, and expose runtime control values via a local display/UI layer.

Key behaviors include:
- GB/T 27930 CAN message handling
- BMS charger state machine for BHM/BRM/BCP/BRO and charging flow
- CAN1 transmit/receive via STM32 HAL
- User interface support for runtime parameter adjustment
- Flash logging support for diagnostics

## Prerequisites

- STM32CubeIDE (project is stored in `gbt-receiver.ioc`)
- STM32F4 HAL drivers (included under `Drivers/STM32F4xx_HAL_Driver`)
- ST-LINK or compatible SWD programmer/debugger
- Python 3 for the `flash_dump_to_csv.py` utility

## Repository structure

- `Core/Inc/` - project headers and application interface declarations
- `Core/Src/` - application source files
  - `main.c` - startup and peripheral initialization
  - `gbt_27930_bms.c` - GB/T 27930 state machine and CAN helper logic
  - `display_ui.c` - UI handling and runtime parameter adjustment
  - `can.c` - CAN transmit/receive helpers
  - `flash_writer.c` - flash logging support
  - `helpers.c` - common utilities
- `Drivers/` - STM32 HAL and CMSIS device support files
- `Debug/` - build artifacts and debug output from STM32CubeIDE
- `flash_dump_to_csv.py` - utility script to convert raw flash dump output to CSV

## Flash logging

The firmware logs CAN traffic into a reserved flash region starting at `0x08020000`. Each logged entry contains:
- timestamp (`HAL_GetTick()`)
- CAN ID and direction flag
- DLC and 8 data bytes

The logger exposes a simple UART CLI over `UART4` at `115200 8N1`.

### UART CLI commands

Connect a serial terminal to `UART4` (`PA0` TX, `PA1` RX) and send commands terminated by newline or carriage return.

#### Flash log commands

- `INFO` — prints log region start, size, and used bytes.
- `COUNT` — prints the number of logged sequences found in flash.
- `SEQ <n>` — streams sequence `n` in CSV-like format over UART.
- `ERASE` — erases the configured flash log region.

#### Runtime field commands

- `RLIST` — lists all runtime fields with current values.
- `RGET <FIELD>` — reads a single field (e.g. `RGET SOC_PERCENT`).
- `RSET <FIELD> <value>` — writes a value to a field, clamped to allowed range (e.g. `RSET DEMAND_CURRENT 100`).
- `STATE` — prints the current charge state (`WAIT`, `BHM`, `BRM`, `BCP`, `BRO`, `CHG`, `END`).

> **See [CLI_RUNTIME.md](CLI_RUNTIME.md) for the full field reference, units, allowed ranges, and examples.**

Example terminal session:

```text
INFO
COUNT
SEQ 0
ERASE
```

The `SEQ` output format is compatible with the savvycan and includes fields such as timestamp, CAN ID, Tx/Rx direction, length, and up to 8 data bytes.

### Dumping flash data with Python

If you have a raw flash dump of chip, use `flash_dump_to_csv.py` to dump log sequences into CSV files:

```bash
python3 flash_dump_to_csv.py stm_dump.bin --out-dir csv_out
```

## Pinout

The firmware uses the following GPIO and peripheral pin assignments:

- Buttons:
  - `BTN_SHARP` = `PC6` (EXTI interrupt)
  - `BTN_STAR`  = `PC7` (EXTI interrupt)
  - `BTN_DOWN`  = `PA9` (EXTI interrupt)
  - `BTN_UP`    = `PA10` (EXTI interrupt)
- LEDs:
  - `LED1` = `PC11`
  - `LED2` = `PC12`
  - `LED3` = `PD2`
  - `LED4` = `PB3`
- CAN1:
  - `CAN1_RX` = `PA11`
  - `CAN1_TX` = `PA12`

### LED meaning
- `LED1` — toggles on incoming CAN message receive activity.
- `LED2` — toggles while the firmware is in the `SEND_BCP` state.
- `LED3` — toggles while the firmware is in the `CHARGING` state.
- `LED4` — toggles while the firmware is in the `SEND_BRM` state and also blinks once during startup initialization.
- I2C2:
  - `I2C2_SCL` = `PB10`
  - `I2C2_SDA` = `PB11`
- UART4:
  - `UART4_TX` = `PA0`
  - `UART4_RX` = `PA1`

## UI usage

The firmware supports an SSD1306 display with four buttons. The UI has two screens:

- **Main screen**: quick access to demand voltage/current, mode, and charger status.
- **Parameters screen**: browse and edit all runtime fields.

### Button meaning

- `BTN_UP` (`PA10`)  — increment selected value or move selection up.
- `BTN_DOWN` (`PA9`) — decrement selected value or move selection down.
- `BTN_SHARP` (`PC6`) — on main screen, step to the next quick field; on params screen, toggle edit mode.
- `BTN_STAR` (`PC7`) — open/close the parameters screen.

### Main screen behavior

The main screen shows:
- current charge state (`WAIT`, `BHM`, `BRM`, `BCP`, `BRO`, `CHG`, `END`)
- quick runtime fields:
  - `DV` = demand voltage
  - `DI` = demand current
  - `MD` = mode (`CV` or `CC`)
- PACK state fields:
  - `PV` = pack voltage from charger
  - `PI` = pack current from charger
  - `PC` = permit charge

Use `#` to select the next quick field. When the stop entry is selected, `UP` or `DOWN` triggers a manual stop.

<details>
<summary>Click to see image of main screen</summary>

Image: ![image of main screen](images/main%20display%20menu.jpg "image of main screen")
</details>

### Parameters screen behavior

The parameters screen lists extended runtime fields such as tail voltage/current, cell max values, SOC, temperature, and permit charge.

- `#` enters or exits edit mode.
- `^` / `v` move the selected parameter when not editing.
- `^` / `v` change the selected value when in edit mode.
- `*` returns to the main screen.

When a parameter is selected, the UI shows an edit marker and a small table of values so you can adjust the charging runtime data without rebuilding firmware.

<details>
<summary>Click to see image of Parameters screen</summary>

Image: ![image of Parameters screen](images/runtime%20params%20edit%20menu.jpg "image of Parameters screen")
</details>

### Note

There is no settings saving functionality, at it was not needed, but it will be helpfull if someone intended use it on daily basis (TODO). To change default settings (that are loaded on startup) you have to edit gbt_27930_bms.c file, variable g_runtime, see in file comments for meaning.

## How to build hardware

- Basicly you need these things to build it:
1. STM32F405 dev board (I used STM32F405RGT6)
2. CAN transceiver board sn65hvd230 ot simular
3. SSD1306 with buttons (it is better to have it to be able monitor params)
<details>
<summary>Click to see image of lcd used</summary>

Image: ![images/lcd display with buttons.jpg](images/lcd%20display%20with%20buttons.jpg "SSD1306")
</details>

4. GBT/DC outlet for car, avaliable on aliexpress.
<details>
<summary>Click to see image of gbt/dc outlet and its pinout</summary>

Image: ![GBT/DC outlet](images/gbt%20dc%20socket.jpg "GBT/DC outlet")

Image: ![GBT/DC outlet pinut](images/GBT_20234_(DC)source-wikipedia.svg "GBT/DC outlet pinout")
</details>

5. Any 12V input DC-DC downconverter to 3.3V dc with enough current capability to power the board and peripherals (used mini360 dcdc).

- Connect all wires according to pinout, flash firmware, do not forget to connect the 12V charger power supply A+ and A- pins to DCDC downconverter.

- LEDs are indicating diffrent state of GBT/DC state machine and one LED for can activity.

- There is a flash log function: all CAN data is saved into flash, then you can dump it via flash dump or uart cli in csv format for savvycan. If flash is full then no logging is avaliable, you need to clean it via cli or st-link flash utility. There is also dbc file for easy log viewing.

## How to charge 

### Important note: this actions are extremely electric hazard and by continuing you are absolutely understanding that you can cause damage to your car, charger, battery or even yourself. If you are not sure what you are doing, do not try to use this.

- Start session via app or charger. You should see a dialog on charger display to connect the cable. Connect the cable and wait for charger to detect it. It should enable 12V output and power up the board. You should see LEDs blinking and then state machine will step into `BHM` state, which means that charger is waiting for BMS handshake. **Do not connect battery for now**.

- After detection, charger will start to send CAN messages and you should see the state machine on lcd display. It should stop at `BRM`, `BCP` or `BRO` state, depending on charger. This means that charger is waiting for battery connection. Outlet will be locked and you can not remove the cable (if you have not removed protection lock hole from it).

- Connect battery (as fast as possible to prevent timeouts as in real world it will be done by car contactor automatically) and wait for state machine to step into `CHG` state. This means that charging now is in progress. You can monitor demand voltage/current, mode, pack voltage/current and permit charge status on main screen. Also you can change demand voltage/current and mode, and for example reported SOC in parameters screen.

- After some time (depends on charger settings and battery state) voltage and current will reach tail switch values and tail values will be applied. You can monitor this on display and also change them in parameters screen. They will only be applied once. 

- There is no auto stop implemented, so you need to stop session manually via app or charger. You can also trigger manual stop by selecting stop entry on main screen and pressing up or down button. After stop, state machine will step into `END` state and charging will be stopped. 

- Disconnect battery, charger will detect it and turn off the output, board will be powered down. You can then disconnect cable, at this moment charger should release the plug and you can remove it from outlet.

# Real world use cases

- The simpliest real world example of using this device is to connect any AC/DC charger to GBT/DC outlet and use it to charge any electric load (PHEV, E-bikes, E-scooters, etc) without DC fast charge support, which is still pretty common for many models. To use it with AC loads you need to set output voltage to approximately 330V (230V AC rectified) and current to the maximum supported by your AC charger. For example, if you have 3.3kW charger, you can set 330V and 10A, and it will work with any AC/DC converter load up to 3.3kW. If you have 6.6kW charger, you can set 330V and 20A. Just make sure that your load can handle the DC voltage (usually it rectifies AC input internally anyway so it does not actually matter that input voltage is DC) and current you set. Some non-dumb AC chargers may require different voltage settings, so you may need to experiment with it, safest solution is to try starting with the 200V and slowly increase it to the point where it works for your application.

- Other use case it to connect it directly to battery for charging, but in this case you need to be very careful and make sure that your battery can handle the voltage and current you set, and also that you have proper fusing and safety measures (propper BMS with balance functions and ability to dissconnect battery on error) in place. Also in many BMS there is a protection that will not allow direct charging if it detects that battery is directly consumes current without any other electrical components on can network reporting it, so it may not work in this way without modifications. Also for small barries it may be unavaliable to set voltage below 200V according to GBT/DC standard for safe charging, but some chargers may allow to set lower voltage, so it is worth to try if you want to charge small battery packs.

<details>
<summary>Click to see image of real world example of using it to charge PHEV without dc fast charge support</summary>

Image: ![car with gbt/dc outlet connected via xt90](images/in%20field%20testing.jpg "car with gbt/dc outlet connected via xt90")

</details>

# Code of charger communication is licensed under MIT License. All other provided files are licensed under their respective licenses and are held by their respective owners.
