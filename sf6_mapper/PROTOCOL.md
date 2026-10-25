# SF6 Mapper – protokół BLE (wersja 4, firmware 1.1.1)

Tracker używa tej samej usługi BLE co companion MeshCore (Nordic UART / BLEUart).
PIN parowania jest losowy (6 cyfr), tworzony przy pierwszym uruchomieniu i widoczny w Menu → Informacje (od 1.1.3). Nazwa BLE to zawsze `MeshCore-` + nazwa trackera.
Domyślnie jest to `MeshCore-SF6Map-XXXX`, gdzie XXXX to pierwsze 2 bajty klucza
publicznego, a po ustawieniu nazwy np. `MeshCore-Pawlo_T1`. Nazwa trackera jest
nickiem jego konta w Mapperze (każdy tracker ma osobne konto). Każdy zapis BLE to jedna ramka; pierwszy
bajt to kod. Liczby wielobajtowe są little-endian.

## Wykrycie trackera

Po połączeniu aplikacja wysyła `0xA0` (HELLO). Tracker odpowiada `0xA0`.
Zwykły companion odpowie błędem `0x01`, więc aplikacja wie, z czym rozmawia.

## Komendy (aplikacja → tracker)

| Kod | Nazwa | Argumenty |
|---|---|---|
| `0xA0` | HELLO | – |
| `0xA1` | GET | `from_seq u32`, `max u8` (1–200, 0 = 200) |
| `0xA3` | ACK | `up_to_seq u32` |
| `0xA4` | SET_RUN | `run u8` (0 = stop, 1 = mapuj) |
| `0xA5` | SET_INTERVAL | `secs u8`: 20, 30, 40, 50 albo 60 |
| `0xA7` | SET_NAME | nazwa ASCII 2–20 znaków (bez spacji i polskich liter) |
| `33` | SIGN_START | – (jak w companionie) |
| `34` | SIGN_DATA | dane do podpisu, w kawałkach (łącznie maks. 512 B) |
| `35` | SIGN_FINISH | – |

## Odpowiedzi (tracker → aplikacja)

**`0xA0` HELLO**

| Pole | Typ | Opis |
|---|---|---|
| proto_ver | u8 | wersja protokołu (1) |
| flags | u8 | bit0 mapowanie włączone, bit1 fix GPS, bit2 pauza (postój), bit3 pamięć pełna, bit4 GPS wyłączony w menu (1.0.3+), bit5 słaba bateria, bit6 zasilanie USB (1.1.0+) |
| pubkey | 32 B | klucz publiczny trackera (identyfikator mappera) |
| pending | u32 | punkty czekające na wysłanie |
| acked_seq | u32 | ostatni potwierdzony numer |
| last_seq | u32 | ostatni zapisany numer |
| total | u32 | wszystkie punkty zapisane od początku |
| batt_mv | u16 | napięcie baterii (mV) |
| sats | u8 | satelity |
| lat, lon | i32, i32 | bieżąca pozycja (stopnie × 1e6, 0 gdy brak fixu) |
| log_bytes, log_cap | u32, u32 | zajętość i pojemność pamięci logu |
| interval | u8 | interwał pingów (s) |
| fw_len, fw | u8 + tekst | wersja firmware |
| name_len, name | u8 + tekst | nazwa trackera (nick konta) |
| last_sync | u32 | czas ostatniego zrzutu (UNIX), 0 = nigdy (wersja 4) |
| session | u8 | numer bieżącej sesji |
| mem_pct | u8 | zajętość pamięci logu (%) |

**`0xA1` RECORD** – jeden punkt, wysyłany po GET:

| Pole | Typ | Opis |
|---|---|---|
| seq | u32 | numer punktu |
| ts | u32 | czas UNIX UTC (z GPS) |
| lat, lon | i32, i32 | stopnie × 1e6 |
| alt | i16 | wysokość (m) |
| sats | u8 | satelity |
| flags | u8 | bity 0–2: interwał pingów ÷ 10 (2–6, czyli 20–60 s); bit 3: pudło przy możliwym limicie repeatera (4 odpowiedzi w 2 min); bity 4–7: numer sesji (0–15, cyklicznie) |
| n | u8 | liczba odpowiedzi; **0 = pudło** |
| n × odpowiedź | 11 B każda | `prefix[8]` klucza repeatera, `snr_rx i8`, `snr_tx i8`, `rssi i8` |

`snr_rx` to SNR odpowiedzi repeatera u nas, `snr_tx` to SNR naszego pingu
u repeatera. Oba w dB × 4 (podziel przez 4). `rssi` w dBm.
Repeater rozpoznajemy po 8-bajtowym prefiksie klucza w `nodes.json`.

**`0xA2` END** – koniec paczki:

| Pole | Typ | Opis |
|---|---|---|
| count | u16 | liczba wysłanych punktów |
| last_seq | u32 | numer ostatniego wysłanego punktu |
| more | u8 | 1 = są kolejne punkty, wyślij następne GET |

**`0xA3` ACK** – `acked_seq u32`, `pending u32`

**`0xA4` RUN** – `run u8`, `flags u8`

**`0xA5` INTERVAL** – `secs u8` (aktualny interwał po zmianie)

**`0xA7` NAME** – nowa nazwa (tekst do końca ramki). W nazwie BLE zmiana jest
widoczna od następnego rozgłoszenia, czyli po rozłączeniu telefonu.

**Podpis (logowanie do Mappera)** – identycznie jak w companionie MeshCore:
`33` → `19` `[0][max_len u32]`; każde `34` → `0` (OK); `35` → `20` `[podpis Ed25519 64 B]`.
Podpisywane są surowe bajty przekazane w `34`. Od 1.1.3 tracker podpisuje **tylko** wyzwania logowania
Mappera, czyli tekst zaczynający się od `mmr1|`, o długości 20–80 B. Inne dane dostają odpowiedź `0x01` z kodem 6. Błąd stanu (np. `34` bez `33`)
zwraca `0x01` z kodem 4, przekroczenie 512 B – kod 5.

**`0x01` ERR** – `err u8`: 1 nieznana komenda, 2 złe argumenty (np. niedozwolony interwał lub nazwa), 3 zajęty (ACK w trakcie GET), 4 zły stan, 5 za dużo danych do podpisu

## Synchronizacja (zalecany przebieg)

1. HELLO → odczytaj `pubkey`, `name`, `pending`, `acked_seq`.
   Zaloguj się do Mappera kluczem trackera: wyzwanie z `api.php?action=challenge`
   podpisz komendami `33/34/35`, nick = `name`.
2. GET(`acked_seq + 1`, 200) → zbieraj RECORD aż do END.
3. Wyślij paczkę na serwer. **Dopiero po potwierdzeniu zapisu przez serwer**
   wyślij ACK(`last_seq` z END). Tracker usuwa punkty dopiero po ACK.
4. Jeśli `more = 1`, wróć do kroku 2 z `last_seq + 1`.

Przerwane połączenie niczego nie gubi: niepotwierdzone punkty zostają w trackerze.
Serwer powinien odrzucać duplikaty po parze (`pubkey`, `seq`) oraz (`pubkey`, `ts`, `lat`, `lon`).
