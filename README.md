# ABOT Firmware (ESP32-S3)

Firmware untuk perangkat ABOT: wajah OLED, suara, navigasi, musik, dan OTA.
Target board: **ESP32-S3 N8R8** (8 MB flash, OPI PSRAM), Arduino-ESP32 core **3.3.12**.

## Struktur

Satu sketch utama `ABOT_V3_51_8.ino` (186 baris) yang hanya berisi `#include` dan
`setup()`/`loop()`. Semua logika ada di modul `.h`:

| File | Isi |
|------|-----|
| `abot_config.h` | `FW_VERSION`, state bersama, kredensial runtime dari NVS |
| `abot_types.h` | enum, struct, topik MQTT |
| `abot_helpers.h` | log, util JSON, `linkSend()` |
| `abot_gfx.h` | layer gambar U8g2 + overlay (`listening`, `thinking`, `sleep`) |
| `abot_audio.h` | I2S, mic, TTS, efek suara |
| `a_ai.h` | Groq/LLM, parsing aksi suara |
| `b_voice.h` | pipeline rekam dan kirim |
| `c_comm.h` | MQTT, parser perintah, CFG via serial |
| `d_ota.h` | OTA HTTP(S) + verifikasi sha256 |
| `e_navigation.h`, `h_map.h` | rute, peta, GPS |
| `f_personality.h` | mood mata, sleep mode, reaksi karakter |
| `g_system.h` | status sistem, baterai, diagnostik |
| `i_companion.h` | mode teman/percakapan |
| `j_ui.h` | layar dan transisi |
| `k_commands.h` | perintah suara/teks |
| `l_ble.h` | BLE Nordic UART (provisioning + debug) |
| `z_main.h` | orkestrasi loop |
| `certs.h` | bundle root CA untuk TLS |

Jangan menambah tab `.ino` kedua: Arduino IDE menyalin tab `.ino` dengan directive
preprocessor di awal dan hasilnya `stray '##' in program`. Modul baru harus `.h`.

## Build

1. Buka `ABOT_V3_51_8.ino` di Arduino IDE 2.x, board package esp32 3.3.12.
2. Pastikan `partitions.csv` ada di folder sketch. Skema default tanpa slot OTA
   akan membuat `Update.begin()` gagal.
3. Library: U8g2, PubSubClient, ArduinoJson, FluxGarage_RoboEyes, ESP32-audioI2S.
4. `Verify`, lalu `Upload` (USB) atau `Sketch > Export Compiled Binary` untuk rilis.

## Kredensial

Tidak ada kredensial di source. Semua disimpan di NVS (namespace `cfg`) dan diisi
sekali lewat Serial 115200:

```
SET ssid=NamaWiFi
SET pass=PasswordWiFi
SET auth=tokenAuth
SET groq=API_KEY
SET topic=abot/<device>/v1
CFG?
```

`CFG?` hanya menampilkan status terisi/kosong, bukan nilainya. `CFG CLEAR` menghapus semua.

## MQTT

Default basis topik `abot/wokwi/477375325542330369/v337`, dengan subtopik
`/command` dan `/status`. Topik bisa diganti lewat `SET topic=`.

Perintah dikirim sebagai JSON satu baris: `{"t":"..."}`.

- Tanpa auth: `ping`, `status`
- Perlu `{"t":"auth","token":"..."}` dulu: `route`, `gps`, `trip_start`, `trip_end`,
  `reset`, `play` (`url`), `music_stop`, `vol`, `ota_start`, `ota_cancel`

## BLE

Nordic UART, JSON per baris (`\n` sebagai pemisah):

- service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- RX (HP ke ABOT) `6e400002-...`
- TX (ABOT ke HP) `6e400003-...`

BLE hidup saat WiFi mati dan mati saat MQTT tersambung (RAM tidak cukup untuk
Bluedroid + TLS bersamaan). `ping`/`status` publik, `SET`/`CFG` butuh token.

## OTA

`ota_start` membawa `url`, `sha256`, dan `size`. Device mengunduh sendiri lewat
WiFi, mengikuti redirect maksimal 4 hop, memverifikasi sha256 sebelum menulis ke
partisi OTA. Kegagalan dilaporkan sebagai `ota_error` berisi `code`, `err`, dan
`heap`.

## Rilis

1. Naikkan `FW_VERSION` di `abot_config.h`.
2. Build, lalu unggah `.bin` ke GitHub Releases dengan tag `V3_52_x` dan nama
   asset `ABOT_V3_52_x.ino.bin`.
3. Perbarui `manifest.json` di branch `main` (`version`, `url`, `size`, `sha256`).
   App membaca manifest itu untuk menawarkan update.

Manifest hanya boleh diperbarui setelah asset diunduh ulang dan sha256-nya cocok.

## Lisensi

Belum ditentukan.
