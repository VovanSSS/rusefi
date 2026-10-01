# M74.9 OEM-loader CLI (`:m749`)

Инструмент для чтения/записи флеша ЭБУ M74.9 (Itelma I865LB52/I812TA01) **через оригинальный
резидентный загрузчик** по UDS на диагностическом CAN `0x7E0/0x7E8`, 500 кбит/с. Порт инструмента
`fw-m74.9` в дерево rusefi как Gradle-подпроект `:m749` (`java_console/m749/`).

Не путать с OpenBLT: это **не** XCP/OpenBLT, а штатный UDS-загрузчик ЭБУ. Регионы загрузчика,
идентичности, парного ключа и хранилища при обновлении **сохраняются**.

## Сборка и запуск

Лаунчер собирает fat-jar и запускает CLI:

```bash
bash java_console/bin/m749_can.sh <команда> [опции]
```

- fat-jar: `./gradlew :m749:fatJar` -> `java_console/m749/build/libs/m749-all.jar`.
- main-класс: `com.rusefi.m749.M749Cli`.
- нативные библиотеки PCAN лежат в `java_console/` (`libpcanbasic_jni.dylib` на macOS,
  `PCANBasic_JNI.dll` на Windows); лаунчер выставляет `-Djava.library.path=.` и
  `--enable-native-access=ALL-UNNAMED` (под Java 25).
- Тесты: `./gradlew :m749:test`.

## Транспорты

| Опция | Значение |
| --- | --- |
| `--channel PCAN_USBBUS1` (или `auto`) | PCAN — работает и на macOS (MacCAN/libPCBUSB), и на Windows (PEAK) |
| `--slcan PORT` \| `auto` | SLCAN-адаптер (jSerialComm) |
| `--socketcan IFACE` | Linux SocketCAN (интерфейс уже поднят на 500 кбит/с) |
| `--serial-baud`, `--slcan-bus` | только SLCAN |
| `--block-size N` (0..255) | ISO-TP block size, 0 = без пауз |
| `--stmin MS` (0..127) | межкадровая задержка ISO-TP, мс |

По умолчанию (без транспорта) — SLCAN `auto`.

## Команды

### Идентификация ECU (только чтение, без смены сессии/безопасности)

```bash
bash java_console/bin/m749_can.sh --identify --channel PCAN_USBBUS1
```

### Список PCAN-каналов

```bash
bash java_console/bin/m749_can.sh --list
```

### Чтение полного флеша (бэкап)

```bash
bash java_console/bin/m749_can.sh --read-flash m749-full.bin --channel PCAN_USBBUS1 --reset-after
```

Диапазон по умолчанию `0x08000000..0x083EFFFF` (4 128 768 байт). Блок читается дважды
(верификация), результат публикуется только при совпадении + SHA-256. Рядом создаются
`*.part` и `*.properties` (чекпойнт, resume).

Быстрый режим (однократное чтение, ~2x):

```bash
bash java_console/bin/m749_can.sh --read-flash m749-full.bin --channel PCAN_USBBUS1 \
  --chunk-size 4080 --block-size 16 --stmin 0 --fast --reset-after
```

Возобновление прерванного чтения (для того же файла):

```bash
bash java_console/bin/m749_can.sh --read-flash m749-full.bin --channel PCAN_USBBUS1 \
  --resume --helper-running
```

Ограниченный пробный чит (валидация связки):

```bash
bash java_console/bin/m749_can.sh --read-flash m749-sample.bin --channel PCAN_USBBUS1 \
  --start 0x08000000 --length 128 --chunk-size 128 --reset-after
```

Доп. опции чтения: `--start ADDR`, `--length N`, `--chunk-size 1..4080`,
`--helper-running` (переиспользовать живой хелпер), `--pair-file`/`--immo-backup` (парный ключ).

### Запись (заливка)

HEX/SREC — прошивка rusEFI (калибровка сохраняется):

```bash
bash java_console/bin/m749_can.sh --write-flash rusefi.hex --channel PCAN_USBBUS1
```

BIN (полный OEM-бэкап) — восстановление приложения **и** калибровки
(`0x08001000..0x080FFFFF`), загрузчик/identity/pairing/storage сохраняются:

```bash
bash java_console/bin/m749_can.sh --write-flash OEM-full-backup.bin --channel PCAN_USBBUS1
```

`--write-flash` и `--upload` — синонимы. Только калибровка: `--calibration`.
Валидация без адаптера: `--dry-run`.

### Префлайт (проверка цели без записи)

```bash
bash java_console/bin/m749_can.sh --check-target FILE --channel PCAN_USBBUS1
```

### Чтение байта / парного ключа

```bash
bash java_console/bin/m749_can.sh --read-byte 0x08000004 --channel PCAN_USBBUS1
bash java_console/bin/m749_can.sh --read-pair ecu.pair --channel PCAN_USBBUS1
bash java_console/bin/m749_can.sh --export-pair ecu.pair --immo-backup PAIRED_FULLFLASH.bin
```

## Спаренные (IMMO) ECU

Если запись падает на входе в программирующую сессию с `UDS 10 rejected: NRC 22`
(`conditionsNotCorrect`) — ECU спарена с иммобилайзером и требует парной авторизации.
Добавь валидированный бэкап как источник ключа:

```bash
bash java_console/bin/m749_can.sh --write-flash OEM-full-backup.bin --channel PCAN_USBBUS1 \
  --immo-backup PAIRED_FULLFLASH.bin
```

Порядок: запусти команду -> дождись строки
`IMMO listener ready. Cycle ECU bench power now; waiting up to 60 seconds...`
-> **передёрни питание ECU** (полное отключение на пару секунд — L9779 держит VCC при
быстром щелчке ключа) -> инструмент ловит стартовый challenge `0x713`, делает
ChaCha-обмен доказательств `0x713`/`0x714`, отправляет разрешение на программирование,
входит в OEM-загрузчик и шьёт.

`--immo-backup` принимает только валидированный I865-бэкап
(SHA-256 `ac052cd5cacf0385b4c2de794f6b1ad476e3f9f1badab5b8c54619854e428b39`); ключ читается
локально из `0x08274000`, reference — из `0x0804C2B4`. Пара не меняется, иммо не отключается.

## Скорость (CAN 500 кбит/с — жёсткий потолок ~27 кБ/с на приём)

| Режим | Скорость (верифицированных байт/с) | Примечание |
| --- | --- | --- |
| default (двойное чтение) | ~13,8 кБ/с | надёжно |
| `--fast --block-size 16 --stmin 0` | ~19,7 кБ/с | надёжно |
| `--fast --block-size 0 --stmin 0` | ~26,5 кБ/с | **оверран буфера MacCAN**, теряются кадры |

`--block-size 0` (без паузы flow-control) слишком агрессивен для MacCAN: хелпер льёт
~584 кадра подряд, приёмный буфер переполняется (таймаут `SID 23 ... 3975/4085 bytes`).
Рабочий быстрый вариант — `--fast --block-size 16 --stmin 0 --chunk-size 4080`.

## Итоги валидации (живой I865 ECU, macOS + PCAN)

- `--list` детектит `PCAN_USBBUS1`; `--identify` читает OEM DIDs `F186`/`F189`/`F192`.
- Чтение `m749-full.bin` совпало с openocd-эталоном **1:1 во всех регионах**, кроме
  `EEPROM backing store 0` (runtime-данные, меняются при работе ECU).
- Запись OEM-бэкапа восстановила `0x08001000..0x080FFFFF` (app+calibration) — повторное
  чтение совпало с эталоном **1:1**. Остались 3 ожидаемых различия:
  - `0x08200004..0x0820000F` (12 байт) — programming-state метаданные (после активации стёрты в FF);
  - `0x0824E000` (256 байт) — журнал программирования (дописана запись `20261001` + `fw-m74.9 CLI`);
  - `EEPROM backing store 0` (runtime).

## Что добавлено в дерево (порт из fw-m74.9)

- `java_console/m749/` — подпроект `:m749` (CLI, uploader, UDS, транспорты, image-парсеры,
  checksum/byte/pair-reader, IMMO, read-flash; плюс 28 тест-классов).
- `com.rusefi.io.can`: `CanAddress`, `ClassicCanFrame`, `RawCanPort`, `SocketCanRawPort`,
  `SlcanPortScanner`, `SlcanVersion` (новая raw-CAN абстракция).
- `com.rusefi.ui.plugins.StartupTabProvider` (новый SPI вкладки).
- `HelloCommand` / `SerialAutoChecker` — перегрузки с таймаутом (аддитивно).
- `java_console/bin/m749_can.sh` — лаунчер; `settings.gradle` — `include ':m749'`.

Правки под macOS: `PcanDevice.scan()` — детект канала через `Initialize`-пробу вместо
`PCAN_CHANNEL_CONDITION` (MacCAN не заполняет буфер значения); убран Windows-only запрет PCAN;
`--fast` — однократное чтение блока.
