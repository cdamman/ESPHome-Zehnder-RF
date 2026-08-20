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
| `fan` | The main control, carrying the device's own name: the **named modes** (`1 - Low`, `2 - Normal`, `3 - High`) and an on/off that *is* the boost. No percentage slider: a unit with three fixed speeds has nothing to slide |
| `number` **Timer duration** | Boost duration, 1 to 240 min, kept across restarts |
| `sensor` **Timer remaining** | Count-down, in minutes, of the running boost |
| `sensor` **Current consumption** / **Consumption today** | Power derived from the speed, and daily energy reset at midnight |
| `number` **Power 1-3** | Watts drawn at each speed, set from Home Assistant and kept across restarts |
| `sensor` **Airflow** | Unit output in %, as the unit itself reports it |
| `button` **Re-pair** | Re-runs the join handshake, for a unit whose pairing window is open |
| **Filter reminder** | A configurable interval in days (90 by default), the date they are next due, a `problem` flag once they are, and a button to press when they are changed — see [Filter change reminder](#filter-change-reminder) |

The fan is the device's main control, so it is configured with `name: None` and
inherits the device name (*Zehnder Fan*) rather than adding a word to it —
`esphome: friendly_name:` is what supplies that name, and ESPHome requires it
for this.

Every label — entity names and mode names alike — comes from a language file, so
the language is a one-line choice (see [Language](#language)). English is the
default; the committed `utility-bridge-d1-mini.yaml` selects French, being the
author's own unit.

Changes made from a physical remote show up immediately: the radio listens
continuously and also decodes the frames addressed to the unit.

## Language

All user-facing strings live in `translations/`, and the language is the file your
per-install config lists alongside `utility-bridge-common.yaml` --
`utility-bridge-d1-mini.yaml` does exactly that (`utility-bridge-esp32.yaml` the
same way, listing `en.yaml` instead):

```yaml
packages:
  vmc:
    url: https://github.com/cdamman/ESPHome-Zehnder-RF
    files: [translations/fr.yaml, utility-bridge-common.yaml]   # en.yaml for English, the default
    ref: main
    refresh: always
```

`en.yaml` and `fr.yaml` hold the same keys: the entity names and the four mode
labels. English is the reference list; change the file name, reflash, done. This is
not cosmetic for Google Home: it forwards these names and mode labels verbatim and
ignores Home Assistant's own translations, which is why they have to be right in
the firmware.

The choice has to be made in *your* config, not in `utility-bridge-common.yaml`: a config wins
over the packages it pulls in, never the other way round, so a default language set
down there could not be overridden from up here. That is also why `utility-bridge-common.yaml`
alone leaves every `${name_*}` unresolved.

The entity names in this README are the English ones. In French the modes read
`1 - Bas` / `2 - Normal` / `3 - Fort`, *Timer duration* becomes *Durée du minuteur*,
and so on -- see the two files for the full list.

There is no `0 - Off` mode: this unit has no stop mode in practice, only the three
running speeds. A VMC that does have one can add it back the same way any other
label is added -- an extra key in a language file and one more line in
`preset_modes` (see [Options of the `fan` platform](#options-of-the-fan-platform)).

To change a single label without editing a language file, set the same key again
in your per-install config's own `substitutions` — for the same reason, keys
there win:

```yaml
substitutions:
  name_airflow: "Living room airflow"
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

- **on** → the boost speed, with the timer;
- **off** → back to **the mode it was in** before the boost started, so the unit
  keeps running; `revert_speed` only steps in when that is unknown (a reboot
  mid-boost, or a speed no mode is mapped to);
- the entity reads **on** whenever the unit runs at the boost speed, so a boost
  started from a physical remote shows up as on too.

There is no way to actually stop the unit through this entity, on purpose: it
mirrors the hardware, which has no off mode either -- only three running speeds.
A VMC that does have a real off mode can map one to `preset_modes`' `off` (see
[Options of the `fan` platform](#options-of-the-fan-platform)), which then is what
stops it.

There is no percentage slider either: the component advertises no speed control, so
Home Assistant has none to draw. The unit has three fixed speeds, and the modes are
the honest control for them — the speed is still tracked internally, and still what
a mode or `set_speed` sets. The **Airflow** sensor
still reports the percentage the unit itself puts out — a reading, not a way to
drive it.

That makes the fan toggle the one to expose to Google Home, and it is why there is
no boost button — the toggle already starts and ends the boost. For a boost of an
unusual length, an automation sets **Timer duration** and then turns the fan on;
no per-install config defines user services, because the entities cover what they
did.

## Installation

The configuration is split in two:

- **`utility-bridge-common.yaml`** — the fan and everything around it: the named
  modes, the boost, the timer, the power figures, the diagnostics. Nothing about a
  board or a network lives here, so it is the same file whatever the hardware.
  Pulled in from GitHub, never copied -- and it is also the one that declares
  `external_components:`, fetching the component itself from GitHub too, since it
  is the file that actually uses it (`platform: zehnder`).
- **`utility-bridge-d1-mini.yaml`** / **`utility-bridge-esp32.yaml`** — one per
  board, each a plain ESPHome config (`esphome:`, `wifi:`, `api:`, `ota:`,
  `logger:`, the board, its pins) plus the package that fetches the file above.
  This is the file you keep and edit — it is the same shape as any ESPHome config,
  it just sources its VMC support from GitHub instead of declaring it inline.

So a full ESPHome configuration directory holds two files: one per-install config
(both committed here, complete — `utility-bridge-d1-mini.yaml` for a Wemos D1
mini, `utility-bridge-esp32.yaml` for an ESP32 devkit) and `secrets.yaml`:

```yaml
# secrets.yaml (see secrets.yaml.example)
wifi_ssid: "my-wifi"
wifi_password: "my-wifi-password"
esphome_vmc_api_key: "base64-encoded-key"
esphome_vmc_ota_password: "ota-password"
```

The keys stay yours: ESPHome resolves a `!secret` against the configuration
directory being built, so nothing in this repository ever needs your credentials.
There is no key for the fallback access point: it is open on purpose, so a Wi-Fi
that stops working is fixed from a phone through the captive portal without a
password to remember — it only exists while the Wi-Fi is down, and the OTA
password still guards a flash. To close it, add the passphrase directly to your
per-install config's own `wifi:` block — it is a plain ESPHome config, so this is
a normal edit, not a package override:

```yaml
wifi:
  ap:
    password: !secret esphome_vmc_ap_password
```

### Changing something utility-bridge-common.yaml does not leave open

Everything in your per-install config outside the `packages:` block —
`esphome:`, `wifi:`, `api:`, the board, the pins — is edited directly, the same
way as in any ESPHome config; the AP password above is one example. What
`utility-bridge-common.yaml` itself declares (the fan and its entities) is
reached in one of two ways instead:

- **A substitution** — the per-speed power figures, or a single entity label: set
  the key again in your per-install config's own `substitutions`. A config wins
  over its packages.
- **An option of an entity the package declares** — `id: !extend <the entity's id>`
  merges into that entry rather than adding a second one. This is how the pairing is
  pinned (see [Pairing](#pairing)), and how any `fan:` option is changed:

  ```yaml
  fan:
    - id: !extend ${devicename}_fan
      update_interval: 60s
      revert_speed: low
  ```

  `!remove` is the counterpart: `- id: !remove zehnder_fan_airflow` under
  `sensor:` drops an entity you do not want.

`utility-bridge-d1-mini.yaml` targets a **Wemos D1 mini / D1 mini Pro**, with
exactly nRF905-API's pins, so a board already running that firmware can be
flashed with this one without touching the wiring:

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

`utility-bridge-esp32.yaml` targets an **ESP32 devkit** (`esp32doit-devkit-v1`),
wired the way `utility-bridge.yaml` was in the fork this repository started from
(see [Origin](#origin)), before it was adapted here for a D1 mini:

| nRF905 | GPIO |
| --- | --- |
| CS | GPIO23 |
| AM | GPIO32 |
| CD | GPIO33 |
| CE | GPIO27 |
| PWR | GPIO26 |
| TX_EN | GPIO25 |
| DR | GPIO35 |
| CLK | GPIO14 |
| MOSI | GPIO13 |
| MISO | GPIO12 |

Both GitHub sources — the package pull in each per-install config, and the
component fetch inside `utility-bridge-common.yaml` itself — use `refresh: always`
rather than ESPHome's 1‑day default. The two are versioned together, and a cached
copy of one against a fresh copy of the other is exactly how you get
`[some_option] is an invalid option for [fan.zehnder]`: a new per-install config
reaching a stale, cached `utility-bridge-common.yaml`, or a fresh
`utility-bridge-common.yaml` reaching a stale component clone. The cost is a git
fetch per build, and a build that needs the network — set both back to `1d` once
you stop following changes here. (If you hit that error with a cached clone,
deleting `.esphome/external_components` also clears it.)

The D1 mini has no pins left for AM and CD, which nRF905-API also leaves
unconnected. Nothing is lost for **AM**: it is bit 7 of the status register, which
the component polls over SPI regardless. **CD** has no such fallback — the nRF905's
status register carries only DR and AM, so carrier detect exists on that pin and
nowhere else. The ESP32 devkit has pins to spare and wires both, which is the one
functional difference between the two boards here: **RF: Channel busy** is declared
in `utility-bridge-esp32.yaml` rather than in the common config, because on a board
without CD the component never samples it and the sensor would read a flat 0.0 %
forever rather than "the band is quiet". Two ESP8266-only details in
the D1 mini config: `restore_from_flash: true` is set, or the values the `number`
entities restore would sit in RTC memory and be lost on a power cut; and
`web_server` is left out to save the RAM the API connection needs — the ESP32
config has RAM to spare, so it turns the local web UI on instead.

The component itself is not tied to either board, and neither is the two-file
split: `utility-bridge-common.yaml` is exactly the same file behind both
per-install configs. Wiring a different ESP32 or ESP8266 board means editing the
`esp32:`/`esp8266:` and `spi:`/`nrf905:` blocks of the matching per-install
config, and nothing else.

To start from your own YAML instead of either committed config:

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
    id: zehnder_fan
    name: "Ventilation"
    nrf905: nrf905_rf
    preset_modes:
      "1 - Low": low
      "2 - Normal": medium
      "3 - High": high
```

## Options of the `fan` platform

| Option | Default | Description |
| --- | --- | --- |
| `nrf905` | — | Id of the `nrf905` component (required) |
| `update_interval` | `30s` | How often the unit is polled |
| `model` | `zehnder` | `zehnder` (868.400 MHz) or `buva` (868.200 MHz) |
| `channel` | — | Explicit nRF905 channel, wins over `model` |
| `preset_modes` | — | Named modes as `"<name>": <speed>`, in display order. Speed is `off`, `low`, `medium`, `high` or 0 to 3; a mode mapped to `off` is what stops the unit |
| `boost_speed` | `high` | Speed the boost runs at, and the speed at which the entity reads "on" |
| `revert_speed` | `medium` | Speed to fall back to when a boost ends and the mode it started from is not known |
| `boost_duration` | `30min` | Default boost duration (1 to 240 min) |
| `network_id`, `main_unit_id`, `my_device_id` | — | Pin the pairing (all three together) so it survives a flash erase |
| `main_unit_type`, `my_device_type` | `0x01`, `0x03` | RF types; only change them if your install differs |
| `self_heal_after` | `10` | Polls without an answer before an automatic re-pair (0 disables) |

Methods available from a lambda: `startBoost(minutes)` (0 = configured
duration), `cancelBoost()`, `setSpeed(speed, minutes)`,
`set_timer_minutes(minutes)`, `isTimerActive()`, `getTimerRemainingMinutes()`,
`getFanPercentage()` (the output percentage the unit reports, if you do want a
sensor for it), `startPairing()`.

The sensors derived from the fan's state are refreshed from its `on_state`
trigger, not just on their own interval, so a mode change or a boost shows up in
the consumption at once instead of up to 30 s later:

```yaml
    on_state:
      - component.update: zehnder_fan_power
      - component.update: zehnder_fan_timer_remaining
      - component.update: zehnder_fan_airflow
```

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

The unit reports no consumption, so **Current consumption** derives it from the
speed and the watts configured for that speed. Those three figures — one per
running speed — are `number` entities (`entity_category: config`), so they are set
from Home Assistant and survive a restart. The integration kept them in its options
dialog for the same reason. The values in `substitutions` are only what they start
at.

Speed 0 is not a real speed on this unit; it only shows up briefly at boot, before
the first successful poll, and borrows the *1 - Low* figure rather than get a
figure of its own. The protocol also has a fourth speed (*max*) that this
component never asks for: the schema refuses it and `setSpeed()` clamps to 3. A
unit that reports it anyway -- someone pressed max on a physical remote -- is
reported faithfully, matches no named mode, and its consumption falls back to the
*3 - High* figure rather than stall the daily total.

## Filter change reminder

Four entities, no automation required for any of it to work:

| Entity | Purpose |
| --- | --- |
| `number` **Filter interval** | How long a set of filters lasts, in days — 90 by default (the usual three months), and the deadline moves as soon as you change it |
| `sensor` **Filter change due on** | When they are next due. Home Assistant renders a timestamp as *3 November*, and as *in 2 months* on the cards that show relative time (diagnostic) |
| `sensor` **Filter life** | Share of the interval left, 100 % → 0 %, shaped as a battery so it lands on the Maintenance dashboard. **Disabled by default** — see [below](#about-the-maintenance-section) |
| `binary_sensor` **Filter status** | `device_class: problem`, so its value reads *Problem* / *OK* and Home Assistant shows it as something wrong with the device. Filed under *Diagnostic*, which does not stop an automation from watching it |
| `button` **Filters changed** | Press it after changing them: today becomes the new reference date and the deadline moves out by one interval. Filed under *Configuration* on the device page, with the other buttons |

The date of the last change is what is stored, not a remaining time — a filter ages
in calendar time, and a stored deadline would drift every time you changed the
interval. It survives reboots and power cuts (`globals` with `restore_value`), with
the same up-to-5-minute window as every other stored value here, since
`flash_write_interval` is `5min`.

The interval is in **days**, not months, because a month is not a length: counting
in months means picking a fudge (30? 31? 30.44?) and then explaining it forever,
and *120 days* is no harder to type than *4 months*. `filter_interval_days` is a
substitution, so a different starting value is a one-line override in your own
config — though the number entity is the place to change it once it is running.

Three things worth knowing:

- **It needs a clock.** `time:` lives in your per-install config (both committed
  ones declare `id: homeassistant_time`, which is the id the common config reads).
  Until Home Assistant has connected once, every entity above reads *unknown*
  rather than counting from 1970, and the button refuses to do anything and says so
  in the log.
- **First boot assumes fresh filters.** With no date stored, the first minute that
  has a clock records "now" and the deadline is set from there. If yours are
  already old, press **Filters changed** when you actually change them — same code
  path, and it is the only thing that moves the date afterwards.
- **A section change needs more than a reflash.** If **Filters changed** shows up
  under *Controls* instead of *Configuration* (or a sensor lands outside
  *Diagnostic*), the firmware is almost certainly right and Home Assistant is
  holding the category it recorded the first time it saw that entity: the entity
  registry stores `entity_category` at creation, and an entity that already exists
  is not moved when the device simply re-announces itself with a new one.

  **Reconfigure** on the device (Settings → Devices & services → ESPHome → the
  device → ⋮ → *Reconfigure*) applies it, and is the cheapest fix — verified on a
  real install after a category change did not take. Deleting the entity or the
  device and letting the integration re-create it also works and is what
  [ESPHome's own entity-category PR](https://github.com/esphome/esphome/pull/2636)
  suggests, but it throws away any rename or area you had set, so try Reconfigure
  first.

  Confirm which side you are looking at before rebuilding anything: `esphome
  config` on your own file prints the `entity_category:` the *next* build will
  carry, so if it says `config` there, nothing in this repository can change what
  the dashboard shows.

### Getting it onto your phone

The firmware deliberately does not know your phone: it exposes the `problem` flag
and lets Home Assistant decide. Two automations, the second one letting you clear
the reminder from the notification itself:

```yaml
automation:
  - alias: "VMC filters need changing"
    triggers:
      - trigger: state
        entity_id: binary_sensor.zehnder_fan_filter_status
        to: "on"
    actions:
      - action: notify.mobile_app_your_phone     # your device's own notify action
        data:
          title: "Ventilation filters"
          message: "Time to change the VMC filters."
          data:
            actions:
              - action: "VMC_FILTERS_CHANGED"
                title: "Done — reset the timer"

  - alias: "VMC filters changed, from the notification"
    triggers:
      - trigger: event
        event_type: mobile_app_notification_action
        event_data:
          action: "VMC_FILTERS_CHANGED"
    actions:
      - action: button.press
        target:
          entity_id: button.zehnder_fan_filters_changed
```

Pressing the button from the notification, from the device page, or from a
dashboard are all the same thing. (On Home Assistant older than 2024.10, write
`trigger:` / `action:` for the block keys and `service:` instead of `action:` for
the calls — the mechanism is unchanged.)

Entity ids follow the **language you chose**: with `translations/fr.yaml` the flag
is `binary_sensor.zehnder_fan_etat_des_filtres` and the button
`button.zehnder_fan_filtres_changes`. Check them under Developer tools → States
before pasting.

### About the "maintenance section"

There are two different things by that name, and this entity reaches neither of
them on its own.

There is no maintenance **entity category**: Home Assistant has exactly three —
primary (none), `config` and `diagnostic`. One was proposed for precisely this
kind of entity and the architecture team
[closed the proposal](https://github.com/home-assistant/architecture/discussions/1016).

There *is* a built-in **Maintenance dashboard**, added in
[2026.5](https://www.home-assistant.io/blog/2026/05/06/release-20265/) and
reachable under *Overview → Summaries*. It will not show this sensor either: it
"focuses on what is probably the most-requested view of all: your batteries",
and collects numeric battery-percentage sensors. Not `problem` binary sensors —
not even binary *battery* ones, which is
[an open complaint](https://github.com/home-assistant/frontend/issues/51905).
No `device_class` or `entity_category` puts a `problem` sensor on that card. The
only way in is to dress the filter life up as a battery percentage — so that is
what **Filter life** does, and why it is **disabled by default**:

- it reports the share of the interval left, 100 % the day the filters are
  changed and 0 % on the deadline, so on the Maintenance dashboard the filters
  sit next to the real batteries and drain towards empty as the deadline comes;
- enable it per device in *Settings → Devices & services → ESPHome → your device
  → **Filter life** → Enable*. Nothing appears until you do;
- the cost is that Home Assistant then believes it **is** a battery: battery
  cards, low-battery automations and the device's battery indicator all pick it
  up. That is the trade for being on that dashboard, and it is why this is opt-in
  rather than on for everyone;
- it is `diagnostic`, where real battery sensors live. If it fails to show up on
  the dashboard, set `filter_life_entity_category: ""` in your own config to make
  it a primary entity instead.

The other options, which stay honest about what the thing is:

- **`device_class: problem`**, which is what **Filter status** already
  uses. It is Home Assistant's own way of saying "this device needs attention",
  and it shows on the device page without any configuration.
- **A to-do list**, if you want it to sit in a list of chores rather than in a
  notification. Add to the first automation:

  ```yaml
      - action: todo.add_item
        target:
          entity_id: todo.maintenance
        data:
          item: "Change the VMC filters"
  ```

- **A label or category** named *Maintenance*, assigned to these entities in
  Home Assistant, which is what its
  [categories and labels](https://www.home-assistant.io/docs/organizing/categories/)
  are for — and which a dashboard can then filter on.

## Pairing

On first boot with no stored identity, the component starts discovery. The unit has
to be in its pairing window — on a ComfoFan S, power-cycling it opens one for about
10 minutes. The **Re-pair** button replays the procedure on demand; it drops the
current link first, so the fan is uncontrollable until the join succeeds. With the identity pinned in YAML, a re-pair also does not
survive a reboot — the pinned values are re-applied at boot.

Once paired, the identifiers show up as three diagnostic entities (*RF: Network
id*, *RF: Unit id*, *RF: Controller id*); pinning them in YAML avoids having to
re-pair after a flash erase. They belong to one unit, so `utility-bridge-common.yaml` does not
carry them — they go in your own config, extending the fan the package declares
(the block is there, commented, at the end of each per-install config):

```yaml
fan:
  - id: !extend ${devicename}_fan
    network_id: 0x12345678      # fanconfig "network"
    main_unit_id: 0xAA          # fanconfig "remote_id"    (the fan)
    my_device_id: 0xBB          # fanconfig "my_device_id" (this controller)
    main_unit_type: 0x01        # default, = fanconfig "remote_type"
    my_device_type: 0x03        # default, = fanconfig "my_device_type"
```

The three above are placeholders — use your own unit's actual identifiers, read off
the diagnostic entities. The three ids go in together — a partial pinning is
refused rather than half-applied — and the two type fields already default to
those values, so they can be left out. Everything else about the fan (platform,
radio, modes) stays as the package declares it: `!extend` merges into that entry
instead of adding a second fan.

### Reusing an nRF905-API pairing

A board already paired by nRF905-API does not have to pair again: its `fanconfig`
holds the same identity, and these option names mirror it. One trap, though — that
JSON calls the fan the **remote** (the remote end of the link), the opposite of what
"remote" suggests here:

| nRF905-API `fanconfig` | this component | what it is |
| --- | --- | --- |
| `network` | `network_id` | the fan's RF network |
| `remote_type` / `remote_id` | `main_unit_type` / `main_unit_id` | the ventilation unit |
| `my_device_type` / `my_device_id` | `my_device_type` / `my_device_id` | this controller |

So `fanconfig`'s `remote_id` goes into `main_unit_id`, not into `my_device_id`.
Getting that backwards leaves the controller claiming the fan's id and addressing
its commands to itself.

Keeping the same `my_device_id` also means retiring the nRF905-API firmware on that
network: two devices answering as the same id would collide.

(`my_device_id` rather than `device_id` is a hard requirement, not a preference:
ESPHome reserves `device_id` on every entity to attach it to a sub-device, and an
option of that name breaks entity setup with `'HexInt' object has no attribute
'id'`.)

## Development

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt   # pinned ESPHome version

.venv/bin/esphome compile tests/ci-esp8266.yaml   # builds the working copy
.venv/bin/esphome run utility-bridge-d1-mini.yaml # compile + upload + logs
```

Mind which source you are building: a per-install config fetches
`utility-bridge-common.yaml` from GitHub, which in turn fetches the component, so
local edits to either show up in neither. The two `tests/ci*.yaml` configs use
`type: local` and do build the working copy. To
build the real config against your edits, assemble it locally first — the
component from this checkout, the language included rather than left to the
caller:

```bash
python3 tests/local_config.py translations/fr.yaml > vmc-local.yaml
.venv/bin/esphome run vmc-local.yaml

# For the ESP32 config instead of the default (utility-bridge-d1-mini.yaml):
python3 tests/local_config.py translations/en.yaml utility-bridge-esp32.yaml > esp32-local.yaml
.venv/bin/esphome run esp32-local.yaml
```

That is the same script CI uses to validate the config, so it stays in step with
both per-install configs.

CI compiles `tests/ci-esp8266.yaml` and `tests/ci.yaml` — the same entities on the
ESP8266 and on the ESP32 — so the component and the boost/timer API stay building
for both, and validates the config in every language:

```bash
python3 tests/check_translations.py
```

## Origin

- Forked from <https://github.com/StevenLooman/ESPHome-Zehnder-RF> (continuous RF
  listening, pinnable pairing, automatic re-pairing, radio diagnostics)
- itself from <https://github.com/Sanderhuisman/ESPHome-Zehnder-RF>
- Protocol documented by <https://github.com/eelcohn/nRF905-API>
- A CAN-based alternative: <https://github.com/yoziru/esphome-zehnder-comfoair>
