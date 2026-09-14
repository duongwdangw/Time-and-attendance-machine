# ESP32 main - ESP-IDF 5.x

Firmware trung tam cho cham cong/kiem soat ra vao. **Khong su dung microSD**: PIN, UID RFID va nhat ky vong gan nhat nam trong NVS flash; mau van tay nam trong AS608; khuon mat duoc laptop xu ly qua ESP32-S3-CAM.

## Dau noi da khoa trong code

| Thiet bi | ESP32 |
|---|---|
| RC522 SCK/MOSI/MISO/CS | GPIO18 / GPIO23 / GPIO19 / GPIO21 |
| AS608 TX/RX | GPIO16 (RX2) / GPIO17 (TX2) |
| S3-CAM TX/RX | GPIO32 (RX) / GPIO33 (TX) |
| LD2410C OUT | GPIO35 (input only) |
| Keypad C1..C4 | GPIO4, 12, 13, 14 |
| Keypad R1..R4 | GPIO22, 25, 26, 27 |
| ST7735 CS / DC / RESET | GPIO15 / GPIO2 / ESP32 EN |

Tat ca GND phai chung. RC522 va ST7735 chi 3.3 V. ST7735 dung chung SCK GPIO18 va MOSI GPIO23 voi RC522, nhung moi module co CS rieng (RC522 GPIO21, ST7735 GPIO15), nen khong xung dot. AS608 cap dung dien ap ghi tren module (nhung UART phai muc 3.3 V; dung level shifter neu TX cua module la 5 V). RST RC522 duoc noi 3.3 V nhu so do, nen code khong chiem GPIO RST.

GPIO12 la chan strapping ESP32: khong de keypad keo len/xuong luc reset. Dung dien tro cach ly/series hoac giu nut khong nhan khi cap nguon. GPIO35 chi doc, khong co pull-up/pull-down noi bo; OUT LD2410C phai duoc noi truc tiep va cung GND. Neu day OUT dai/noi, them dien tro 10 kOhm keo xuong GND tai GPIO35.

## Build / flash

Mo terminal ESP-IDF trong VS Code:

```powershell
idf.py set-target esp32
idf.py build flash monitor
```

## Giao thuc UART voi S3-CAM

115200 8N1, dong ASCII ket thuc `\n`: `FACE_ID:<id>`, `FACE_UNKNOWN`, `CAM_READY`, `ENROLL:<id>`. Khi bam `A` trong che do admin, ESP32 gui `ENROLL:<id>`. Laptop phai gui `FACE_ID:<id>` chi sau khi da xac thuc anh, khong tin du lieu WebSocket truc tiep.

PIN master mac dinh `1234#`; doi no bang NVS truoc khi demo that. Nhan `*`, nhap PIN, `#`, sau do nhap ID nhan vien va `A` de gui yeu cau chup 30 anh khuon mat. Thiet bi chi mo relay sau mot phuong thuc hop le; co the nang cap thanh MFA bang bien `REQUIRE_SECOND_FACTOR` trong `main.c`.
