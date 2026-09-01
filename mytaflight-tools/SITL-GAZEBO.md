# MyTAflight — SITL + Gazebo su macOS (Apple Silicon)

Guida per validare in volo la **Feature D (POS HOLD `pos_hold_navmode = CRUISE`)** con
Betaflight SITL + Gazebo, pilotando con la **Taranis X9D Plus (EdgeTX)**.

> Gazebo gira **solo su Linux**. Su macOS lo si esegue dentro una **VM Ubuntu 24.04**
> (UTM, ARM-nativo su Apple Silicon). La radio si pilota **dal Mac** e l'RC viaggia via
> rete verso la VM — così si evita l'USB passthrough.

```
┌── macOS host ─────────────┐        ┌── VM Ubuntu 24.04 (UTM) ───────────────┐
│ Taranis X9D (USB joystick)│  UDP   │ betaflight_SITL.elf  <── 9004 RC input │
│ taranis-sitl-rc.py  ──────┼────────▶  Gazebo Harmonic  <──> 9002/9003        │
│   --host <VM_IP>          │ 9004   │ websockify 6761->5761                   │
└───────────────────────────┘        └──────────────┬─────────────────────────┘
        Configurator (app.betaflight.com) ── ws://<VM_IP>:6761
```

---

## Fase A — VM Ubuntu 24.04 con UTM (una tantum)

1. **Installa UTM**: `brew install --cask utm` (oppure da https://mac.getutm.app/).
2. **Scarica Ubuntu 24.04 LTS Desktop ARM64** (`.iso` per Apple Silicon) da
   https://ubuntu.com/download/server/arm oppure la Desktop ARM64 daily.
3. **Crea la VM** in UTM → *Create a New VM* → **Virtualize** (NON Emulate) → *Linux* →
   seleziona la ISO. Consigliati: **6–8 GB RAM**, **4 CPU**, **disco 48–64 GB**.
   Lascia attiva l'accelerazione hardware/OpenGL.
4. **Installa Ubuntu** dentro la VM (installazione normale).
5. Dopo il primo boot, in Ubuntu:
   ```bash
   sudo apt update && sudo apt install -y build-essential git curl wget \
        lsb-release gnupg python3 python3-pip
   ```
6. **Trova l'IP della VM** (serve al Mac per raggiungere SITL):
   ```bash
   ip -4 addr show | grep inet        # es. 192.168.64.x con la rete "Shared" di UTM
   ```
   Annota questo IP → è il `<VM_IP>` usato più sotto.

---

## Fase B — Pipeline Gazebo (dentro la VM)

```bash
# 1) i tre repo Betaflight
git clone https://github.com/betaflight/aeroloop_gazebo.git -b gz ~/aeroloop_gazebo
git clone https://github.com/betaflight/betaloop.git       -b gz ~/betaloop
# il firmware: usa il TUO fork sul branch con il CRUISE
git clone <TUO_FORK_URL> ~/betaflight && cd ~/betaflight && git checkout feature/poshold-vector

# 2) Gazebo Harmonic + plugin bridge
cd ~/aeroloop_gazebo && ./install_gazebo_harmonic.sh && gz sim --version
./build_plugin.sh          # -> plugins/build/libBetaflightPlugin.so

# 3) build SITL
cd ~/betaflight && make TARGET=SITL     # -> obj/main/betaflight_SITL.elf

# 4) avvia Gazebo (fisica + sensori + GPS virtuale)
export GZ_SIM_SYSTEM_PLUGIN_PATH=~/aeroloop_gazebo/plugins/build:$GZ_SIM_SYSTEM_PLUGIN_PATH
export GZ_SIM_RESOURCE_PATH=~/aeroloop_gazebo/worlds:$GZ_SIM_RESOURCE_PATH
cd ~/aeroloop_gazebo && ./start_gazebo.sh betaloop_iris_betaflight_demo_harmonic.sdf

# 5) in un altro terminale: SITL
cd ~/betaflight && ./obj/main/betaflight_SITL.elf
```

In alternativa il launcher fa tutto insieme:
```bash
cd ~/betaloop && python3 start.py --gazebo-assets ~/aeroloop_gazebo \
     --elf ~/betaflight/obj/main/betaflight_SITL.elf --gazebo
```

---

## Fase C — Configurator (dentro la VM, o dal Mac)

```bash
pip3 install websockify
websockify 0.0.0.0:6761 127.0.0.1:5761 &     # 0.0.0.0 così è raggiungibile dal Mac
```
Apri **https://app.betaflight.com** → *manual connection* →
`ws://127.0.0.1:6761` (dalla VM) oppure `ws://<VM_IP>:6761` (dal browser sul Mac).

---

## Fase D — Radio (dal **Mac**, niente USB passthrough)

1. Taranis: **SYS → USB Mode = "USB Joystick (HID)"**, collega l'USB al Mac (non DFU/flash).
2. Sul Mac:
   ```bash
   pip3 install pygame
   cd ~/Claude/Projects/MyTAflight
   python3 mytaflight-tools/taranis-sitl-rc.py --list       # trova l'indice della radio
   python3 mytaflight-tools/taranis-sitl-rc.py --monitor     # calibra assi/switch
   # regola AXIS_MAP / AXIS_INVERT / AUX_MAP in testa allo script, poi:
   python3 mytaflight-tools/taranis-sitl-rc.py --host <VM_IP> --port 9004
   ```
   Mappa in SITL fissa: **AETR + AUX1-4** = roll, pitch, throttle, yaw, aux1(ARM), aux2(POS HOLD)…
   Formato verificato: pacchetto UDP 40 byte `<d` + 16×`<H`.

---

## Fase E — Test della Feature D (CRUISE)

Nella CLI del Configurator:
```
set pos_hold_navmode = CRUISE
save
```
Tab **Modes**: assegna **POS HOLD** a un AUX (lo switch mappato su AUX2) e **ARM** a un altro.

Protocollo:
1. Attendi il **GPS fix** (SITL lo simula via GPS virtuale dal pacchetto FDM).
2. **Arma**.
3. Attiva **POS HOLD** a stick centrati → deve **tenere la posizione** (brake-to-hold).
4. **Pitch avanti** → deve prendere **velocità in avanti** costante (non angolo); a stick fermo
   deve **rifrenare a 0 e tenere** la nuova posizione.
   - ⚠️ Se va **indietro** a pitch avanti → il segno di `getRcDeflection(FD_PITCH)` è invertito:
     basta cambiare un segno in `cruiseVelocityFromSticks()` (autopilot_multirotor.c).
5. **Roll destra** → verifica il segno (destra = est nel frame, head-up).
6. Confronta con `set pos_hold_navmode = ANGLE` → in ANGLE lo stick comanda **angolo** (deriva
   col vento simulato), in CRUISE no. È la differenza che vogliamo dimostrare.

> La **rotazione earth-frame** è già validata offline sul Mac (test nativo:
> `mytaflight-tools/` — 13/13 casi OK). Il sim serve per: segno stick, stabilità closed-loop,
> brake-to-hold.

---

## Note

- SITL apre: `9002/9001` PWM out (Gazebo/RealFlight), `9003` FDM/sensori in, **`9004` RC in**,
  `5760/5761` MSP su TCP.
- SITL **block-bufferizza** lo stdout su file: per vedere i log in tempo reale usa un pty
  (`script -q out.log ./betaflight_SITL.elf`) o non killare con `-9`.
- Fonti: https://betaflight.com/docs/development/autopilot/SITL_Autopilot_Testing_Gazebo ·
  https://github.com/betaflight/aeroloop_gazebo · https://github.com/betaflight/betaloop
