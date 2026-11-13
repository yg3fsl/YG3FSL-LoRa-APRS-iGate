# YG3FSL LoRa APRS iGate

iGate LoRa APRS sederhana menggunakan **ESP32-C3 SuperMini** dan **RA-02 (SX1278)**. Perangkat menerima paket LoRa APRS, lalu meneruskan paket yang memenuhi pemeriksaan ke APRS-IS melalui Wi-Fi.

Tidak ada GPS, OLED, atau modul Wi-Fi tambahan. Wi-Fi sudah tersedia di ESP32-C3.

> **Catatan:** RA-02 adalah modul radio yang menggunakan chip SX1278. Jadi, RA-02 dan SX1278 bukan dua modul yang perlu dibeli terpisah.

## Cara kerja

1. RA-02 menerima paket LoRa pada frekuensi dan parameter yang sudah ditentukan.
2. ESP32-C3 memeriksa format paket dan aturan gating.
3. Paket yang lolos diteruskan ke APRS-IS melalui Wi-Fi.
4. iGate juga mengirim beacon posisi ke APRS-IS berdasarkan koordinat manual di firmware.

Perangkat ini **receive-only** pada sisi LoRa. Firmware tidak mengirim beacon melalui radio LoRa. Beacon posisi iGate dikirim melalui koneksi internet ke APRS-IS.

## Perangkat yang digunakan

- ESP32-C3 SuperMini
- Modul Ai-Thinker RA-02 berbasis SX1278
- Antena yang sesuai untuk frekuensi 432,150 MHz dan konektor antena RA-02
- Kabel jumper
- Sumber daya USB 5 V untuk board ESP32-C3

Tidak diperlukan GPS, OLED, atau modul Wi-Fi eksternal.

## Wiring

![Wiring YG3FSL LoRa APRS iGate](images/wiring-igate-yg3fsl.png)

Versi vektor: [wiring-igate-yg3fsl.svg](images/wiring-igate-yg3fsl.svg)

| Pin RA-02 | ESP32-C3 SuperMini | Fungsi |
|---|---|---|
| VCC | 3V3 | Catu daya 3,3 V |
| GND | GND | Ground |
| SCK | GPIO4 | SPI clock |
| MISO | GPIO5 | Data radio ke ESP32 |
| MOSI | GPIO6 | Data ESP32 ke radio |
| NSS / CS | GPIO7 | SPI chip select |
| RESET | GPIO10 | Reset SX1278 |
| DIO0 | GPIO3 | Packet event |

Semua GND harus terhubung menjadi satu (*common ground*).

Gambar menunjukkan hubungan kabel dan GPIO, bukan ukuran atau tata letak pad PCB dalam skala asli. Ikuti tulisan pin pada board yang Anda gunakan.

**Penting:**
- RA-02 menggunakan tegangan 3,3 V. Jangan sambungkan VCC RA-02 ke 5 V.
- Pastikan catu 3,3 V stabil.
- Pasang antena yang sesuai sebelum perangkat digunakan untuk menerima sinyal.
- Wi-Fi ESP32-C3 menggunakan jaringan 2,4 GHz, bukan 5 GHz.
- LED onboard pada firmware menggunakan GPIO8 (aktif LOW); tidak perlu kabel LED tambahan.

## Konfigurasi firmware

Buka file berikut di Arduino IDE:

`firmware/YG3FSL_LoRa_APRS_iGate_v1_2/YG3FSL_LoRa_APRS_iGate_v1_2.ino`

Ubah bagian konfigurasi sesuai perangkat dan jaringan Anda:

```cpp
#define WIFI_SSID        "NAMA_WIFI_ANDA"
#define WIFI_PASS        "PASSWORD_WIFI_ANDA"

#define IGATE_CALL       "YG3FSL-10"

#define IGATE_LAT        (-6.985547669038758)
#define IGATE_LON        (107.5693955311255)
```

Periksa kembali `IGATE_CALL`, `IGATE_LAT`, dan `IGATE_LON` sebelum firmware dijalankan. Koordinat tersebut digunakan untuk beacon posisi iGate dan akan dikirim ke APRS-IS. Pastikan koordinatnya memang lokasi yang ingin dipublikasikan.

`APRS_PASSCODE` disetel ke `-1`, sehingga firmware menghitung passcode berdasarkan callsign. Gunakan callsign yang benar dan sesuai dengan stasiun Anda.

## Parameter radio

Firmware v1.2 menggunakan konfigurasi berikut:

| Parameter | Nilai |
|---|---|
| Frekuensi | 432.150 MHz |
| Bandwidth | 125 kHz |
| Spreading Factor | SF12 |
| Coding Rate | 4/5 |
| Sync Word | `0x12` |
| Preamble | 8 |
| CRC | Aktif |
| Mode | Receive-only |

Pengaturan radio pada iGate harus sama dengan perangkat LoRa APRS yang mengirim paket.

Firmware mengenali prefix LoRa APRS `3C FF 01` dan membuang prefix tersebut sebelum paket diteruskan. Paket TNC2 tanpa prefix juga dapat diproses jika formatnya valid.

## APRS-IS

| Pengaturan | Nilai |
|---|---|
| Server | `asia.aprs2.net` |
| Port | `14580` |
| Callsign default | `YG3FSL-10` |
| Beacon posisi | Setiap 15 menit |
| Simbol beacon | `L&` |

Koordinat beacon diatur secara manual melalui `IGATE_LAT` dan `IGATE_LON`. Tidak ada modul GPS pada perangkat ini.

## Persiapan Arduino IDE

1. Pasang dukungan board **ESP32 by Espressif Systems**.
2. Pilih board **ESP32C3 Dev Module**.
3. Atur **USB CDC On Boot** ke `Enabled`.
4. Pasang library **LoRa by Sandeep Mistry**.
5. Buka file `.ino` di folder firmware.
6. Isi SSID, password Wi-Fi, callsign, dan koordinat iGate.
7. Pilih port yang sesuai, lalu upload.

Library `WiFi` dan `SPI` sudah disediakan oleh Arduino-ESP32 Core.

Serial Monitor menggunakan baud rate **115200**.

## Pemeriksaan setelah upload

Perhatikan Serial Monitor. Beberapa pesan yang perlu diperiksa:

- `LoRa OK` — RA-02 berhasil diinisialisasi.
- `WiFi OK` — ESP32-C3 berhasil tersambung ke Wi-Fi.
- `APRS-IS ... Terhubung` — koneksi ke server APRS-IS berhasil dibuat.
- `Login verified` — server mengonfirmasi login APRS-IS.
- `[GATE]` — paket RF berhasil diteruskan ke APRS-IS.
- `[STAT]` — ringkasan status Wi-Fi, APRS-IS, paket diterima, paket diteruskan, dan paket dibuang.

`LoRa OK` hanya menunjukkan radio berhasil diinisialisasi. Itu belum membuktikan iGate menerima paket dari perangkat lain. Pengujian penerimaan dan forwarding perlu dilihat dari log.

## Catatan

- Firmware ini dibuat untuk menerima paket dengan konfigurasi radio yang sesuai. Perbedaan frekuensi, bandwidth, spreading factor, coding rate, atau sync word dapat membuat paket tidak terbaca.
- Paket yang tidak sesuai format atau memiliki path yang melarang gating akan dibuang.
- Jika Wi-Fi terputus, firmware mencoba menyambungkan kembali.
- Pastikan penggunaan frekuensi 432,150 MHz dan perangkat radio sesuai dengan ketentuan serta izin yang berlaku di wilayah Anda.

## File repository

```text
YG3FSL-LoRa-APRS-iGate/
├── README.md
├── GITHUB_INFO.md
├── .gitignore
├── firmware/
│   └── YG3FSL_LoRa_APRS_iGate_v1_2/
│       └── YG3FSL_LoRa_APRS_iGate_v1_2.ino
└── images/
    ├── wiring-igate-yg3fsl.png
    └── wiring-igate-yg3fsl.svg
```

## Lisensi

Belum ada file lisensi di repository ini. Tambahkan lisensi jika Anda ingin menentukan izin penggunaan, modifikasi, dan distribusi kode.
