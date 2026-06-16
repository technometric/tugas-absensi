# ESP32-CAM Absensi Dashboard

Web dashboard untuk sistem absensi ESP32-CAM.

## Struktur

- `server.js` — Node.js proxy server
- `public/index.html` — UI frontend
- `public/app.js` — frontend logic
- `public/style.css` — gaya tampilan

## Syarat

- Node.js 18+ terpasang di laptop
- ESP32 dan laptop tersambung ke hotspot yang sama
- ESP32 sudah berjalan dan memiliki endpoint `/api` serta `/foto?path=`

## Cara pakai

1. Buka terminal di folder `webserver`
2. Install dependency:

```bash
npm install
```

3. Inisialisasi database lokal dari data yang diberikan:

```bash
npm run import-data
```

4. Jalankan server:

```bash
npm start
```

5. Buka di browser:

```
http://localhost:3000
```

5. Masukkan IP ESP32 di halaman dashboard.

> Karena laptop dan ESP32 terhubung ke hotspot HP yang sama, frontend akan langsung memanggil ESP32 lewat IP tanpa proxy.

## Catatan

- Foto diambil dari ESP32 melalui endpoint proxy `/esp32/foto?path=...`
- Frontend polling setiap 5 detik
- Jika ingin ganti port, set `PORT=xxxx`
