# ESPHome-Zehnder-RF

ESPHome external component driving a ventilation unit over 868 MHz radio (the
Zehnder / BUVA protocol) with an **nRF905** module wired to an ESP32 — no HTTP
bridge in between, the ESP talks to the unit directly.

It exposes in Home Assistant the same features as the
[home-assistant-nrf905-vmc](https://github.com/cdamman/home-assistant-nrf905-vmc)
integration, which goes through the
[nRF905-API](https://github.com/eelcohn/nRF905-API) HTTP bridge instead:

| Entity | Purpose |
| --- | --- |
| `fan` | The **named modes** (`0 - Off`, `1 - Bas`, `2 - Normal`, `3 - Fort`), and an on/off control that *is* the boost |
| `number` **Durée du timer** | Boost duration, 1 to 240 min, kept across restarts |
| `button` **Boost** / **Arrêter le boost** | Run at high speed for the configured duration, or end it early |
| `sensor` **Timer restant** | Count-down, in minutes, of the running boost |
| `binary_sensor` **Timer actif** | A timer is running, whatever started it |
| `sensor` **Consommation** / **Consommation du jour** | Power derived from the speed, and daily energy reset at midnight |
| `number` **Puissance 0-3** | Watts drawn in each mode, set from Home Assistant and kept across restarts |
| `sensor` **Débit** | Unit output in %, as the unit reports it |

Every label — entity names and mode names alike — comes from a language file, so
the language is a one-line choice (see [Language](#language)). French is the
default. Code, comments and commits are English.

Changes made from a physical remote show up immediately: the radio listens
continuously and also decodes the frames addressed to the unit.

## Language

All user-facing strings live in `translations/`, and `vmc.yaml` pulls one file in:

```yaml
substitutions:
  <<: !include translations/fr.yaml   # French, the default
  # <<: !include translations/en.yaml # English
```

`fr.yaml` and `en.yaml` hold the same keys: the entity names and the four mode
labels. Switch the include, reflash, done. This is not cosmetic for Google Home:
it forwards these names and mode labels verbatim and ignores Home Assistant's own
translations, which is why they have to be right in the firmware.

To change a single label without editing a language file, set the same key again
in `vmc.yaml`'s own `substitutions` — keys there win over the included file:

```yaml
substitutions:
  <<: !include translations/fr.yaml
  name_fan: "VMC salon"
```

Adding a language means copying a file and translating its values.
`tests/check_translations.py` (run by CI, and runnable locally) checks that every
language provides every label: ESPHome only warns about an unresolved `${...}` and
would happily ship `${name_airflow}` as an entity name, so that check is what keeps
a half-translated file from reaching your board.

Note that renaming a mode changes the options Home Assistant offers for the fan:
after a language switch, automations or scripts that select a mode by name need
the new spelling.

## On/off is the boost, not a stop button

The `fan` entity's on/off follows the integration rather than the usual fan
convention, and for the same reason: on a ventilation unit, a toggle that stops
the airflow is a trap.

- **on** → the boost speed, with the timer, exactly like the boost button;
- **off** → back to `revert_speed` (normal), the unit keeps running;
- the entity reads **on** whenever the unit runs at the boost speed, so a boost
  started from a physical remote shows up as on too;
- to really stop the unit, pick the **`0 - Off`** mode.

That makes the fan toggle the one to expose to Google Home.

## Installation

`vmc.yaml` is a complete, ready-to-flash configuration for a **Wemos D1 mini /
D1 mini Pro**, wired exactly like nRF905-API, so a board already running that
firmware can be flashed with this one without touching the wiring:

| nRF905 | D1 pin | GPIO |
| --- | --- | --- |
| CE | D2 | GPIO4 |
| DR | D1 | GPIO5 |
| PWR | D3 | GPIO0 |
| TX_EN | D0 | GPIO16 |
| MOSI | D7 | GPIO13 |
| MISO | D6 | GPIO12 |
| CLK | D5 | GPIO14 |
| CS | D8 | GPIO15 |
| AM, CD | — | not wired |

AM and CD stay unconnected, as in nRF905-API — the D1 mini has no pins left for
them, and the component reads both from the nRF905's status register over SPI
anyway. Two ESP8266 details worth knowing: `restore_from_flash: true` is set, or
the values the `number` entities restore would sit in RTC memory and be lost on a
power cut; and `web_server` is left out to save the RAM the API connection needs.

The component itself is not tied to the ESP8266 — it builds for the ESP32 just as
well, and CI compiles both. On an ESP32, swap the `esp8266:` block for an `esp32:`
one and give the pins of your own wiring.

To start from your own YAML instead:

```yaml
external_components:
  - source: github://cdamman/ESPHome-Zehnder-RF

spi:
  clk_pin: GPIO14
  mosi_pin: GPIO13
  miso_pin: GPIO12

nrf905:
  id: nrf905_rf
  cs_pin: GPIO15
  ce_pin: GPIO4
  pwr_pin: GPIO0
  txen_pin: GPIO16
  dr_pin: GPIO5

fan:
  - platform: zehnder
    id: vmc_fan
    name: "Ventilation"
    nrf905: nrf905_rf
    preset_modes:
      "0 - Off": "off"
      "1 - Bas": low
      "2 - Normal": medium
      "3 - Fort": high
```

## Options of the `fan` platform

| Option | Default | Description |
| --- | --- | --- |
| `nrf905` | — | Id of the `nrf905` component (required) |
| `update_interval` | `30s` | How often the unit is polled |
| `model` | `zehnder` | `zehnder` (868.400 MHz) or `buva` (868.200 MHz) |
| `channel` | — | Explicit nRF905 channel, wins over `model` |
| `preset_modes` | — | Named modes as `"<name>": <speed>`, in display order. Speed is `off`, `low`, `medium`, `high`, `max` or 0 to 4; a mode mapped to `off` is what stops the unit |
| `boost_speed` | `high` | Speed the boost runs at, and the speed at which the entity reads "on" |
| `revert_speed` | `medium` | Speed to fall back to when the boost is switched off |
| `boost_duration` | `30min` | Default boost duration (1 to 240 min) |
| `network_id`, `main_unit_id`, `device_id` | — | Pin the pairing (all three together) so it survives a flash erase |
| `main_unit_type`, `device_type` | `0x01`, `0x03` | RF types; only change them if your install differs |
| `self_heal_after` | `10` | Polls without an answer before an automatic re-pair (0 disables) |

Methods available from a lambda: `startBoost(minutes)` (0 = configured
duration), `cancelBoost()`, `setSpeed(speed, minutes)`,
`set_timer_minutes(minutes)`, `isTimerActive()`, `getTimerRemainingMinutes()`,
`getFanPercentage()`, `startPairing()`.

## How the count-down works

The unit only reports *that* a timer is running (one bit of the status byte,
byte `0x09` of a `FAN_SETTINGS` frame), never the time left — nRF905-API reads
that same bit for its `timer` field. So the count-down is kept by the component:

- it starts from the duration actually asked for, either here or by a remote
  whose command was overheard;
- it falls back to `boost_duration` when the bit rises without a known duration
  (boost started out of earshot, or the ESP rebooted mid-timer);
- it clears as soon as the unit reports the timer is over, which also catches a
  command the unit turned out not to apply.

The sensor reads `unknown` while no timer is running.

## Power figures

The unit reports no consumption, so **Consommation** derives it from the speed
and the watts configured for that mode. Those four figures — one per mode,
*0 - Off* included, since a stopped unit is still powered — are `number` entities
(`entity_category: config`), so they are set from Home Assistant and survive a
restart. The integration kept them in its options dialog for the same reason. The
values in `substitutions` are only what they start at.

Speed 4 has no mode here and is not used; if a physical remote ever selects it,
the sensor falls back to the *Fort* figure rather than stall the daily total.

## Pairing

On first boot with no stored identity, the component starts discovery. The unit
has to be in its pairing window — on a ComfoFan S, power-cycling it opens one for
about 10 minutes. The `start_pairing` service replays the procedure on demand.

Once paired, the identifiers show up in the diagnostic sensors (*ID réseau*,
*ID groupe*, *ID télécommande*); copying them into `network_id`, `main_unit_id`
and `device_id` avoids having to re-pair after a flash erase.

## Development

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt   # pinned ESPHome version

.venv/bin/esphome compile vmc.yaml
.venv/bin/esphome run vmc.yaml              # compile + upload + logs
```

CI compiles `tests/ci-esp8266.yaml` and `tests/ci.yaml` — the same entities on the
ESP8266 and on the ESP32 — so the component and the boost/timer API stay building
for both, and validates `vmc.yaml` in every language:

```bash
python3 tests/check_translations.py
```

## Origin

- Forked from <https://github.com/StevenLooman/ESPHome-Zehnder-RF> (continuous RF
  listening, pinnable pairing, automatic re-pairing, radio diagnostics)
- itself from <https://github.com/Sanderhuisman/ESPHome-Zehnder-RF>
- Protocol documented by <https://github.com/eelcohn/nRF905-API>
- A CAN-based alternative: <https://github.com/yoziru/esphome-zehnder-comfoair>
