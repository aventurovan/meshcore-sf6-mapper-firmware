# SF6 Mapper – firmware dla Seeed Wio Tracker L1 / L1 Pro

Samodzielny tracker do mapowania zasięgu sieci MeshCore Południe (SF6).
Działa bez telefonu. Dane zrzuca się później przez BLE do aplikacji Mapper SF6.

## Co robi

- Radio ustawione na sztywno na preset SF6: 869.618 MHz, SF6, BW 62.5 kHz, CR 4/5.
- Po fixie GPS (co najmniej 4 satelity) co **20 / 30 / 40 / 50 / 60 s** (domyślnie 20 s) wysyła zero-hop discovery (tylko repeatery).
  Przez 10 s zbiera odpowiedzi.
- Każdy ping zapisuje z pozycją do flasha QSPI: trafienie (lista repeaterów z SNR w obie strony i RSSI) albo pudło.
  Pojemność to 1.5 MB, czyli około 35–40 tys. punktów.
- Po 4 pingach bez ruchu robi pauzę i nie nadaje. Wznawia dopiero przy prawdziwym ruchu (szczegóły w punkcie „Postój”).
- Nie przekazuje pakietów i nie wysyła adwertów. Jest niewidoczny w sieci poza samymi pingami.
- Ma własny klucz publiczny i nazwę. Każdy tracker to osobne konto w Mapperze, a nazwa trackera jest nickiem.
  Nazwa BLE to zawsze `MeshCore-` + nazwa, np. `MeshCore-Pawlo_T1`. Domyślnie jest to `MeshCore-SF6Map-XXXX`.
  Nazwę ustawia się z aplikacji: 2–20 znaków ASCII, bez spacji i polskich liter.
  Tracker podpisuje wyzwanie logowania swoim kluczem tak samo jak companion.

## Obsługa

**Start:** po włączeniu animacja – napis „SF6 Mapper” wjeżdża z prawej, pod nim rozchodzą się fale radiowe, potem pojawia się
nazwa trackera i wersja firmware. Po ok. 2 s (albo po naciśnięciu dowolnego przycisku) pokazuje się ekran główny z dwoma
krótkimi piknięciami. Piknięcia brzmią zawsze, także przy dźwięku wyłączonym w menu.

**Ekran główny** – trzy strony, przełączane joystickiem w lewo/prawo:
1. **Stan:**
   - pasek: godzina (czas polski z GPS), GPS z liczbą satelitów, Bluetooth, dźwięk, bateria;
   - stan mapowania z odliczaniem;
   - duże liczby pingów w sesji i punktów do wysłania;
   - ostatni repeater z SNR.

   Repeatery są pokazywane jako pierwsze 2 bajty klucza (np. `A1B2`). Nazwy zna serwer i pokazuje je na mapie.
2. **Sesja:** numer, dystans, pingi, skuteczność, czas oraz 5 ostatnich repeaterów z godziną i SNR.
3. **GPS:** satelity, ruch lub postój, współrzędne, wysokość, data i godzina.

**Przyciski na ekranie głównym:** środek joysticka otwiera menu, przycisk wstecz gasi ekran.

**Menu:** góra i dół wybierają pozycję, środek lub prawo zmienia wartość, lewo zmienia w drugą stronę, wstecz wychodzi.
Menu zamyka się samo po 30 s.

| Pozycja | Działanie |
|---|---|
| Mapowanie | WŁ / STOP; STOP wymaga drugiego naciśnięcia w ciągu 4 s |
| Ping co | 20 / 30 / 40 / 50 / 60 s |
| Nowa sesja | zeruje liczniki sesji (każde włączenie trackera to też nowa sesja) |
| Dźwięk | sygnał trafienia i ostrzeżeń |
| GPS | wł./wył.; wyłączony GPS oszczędza baterię, ale tracker wtedy nie mapuje |
| Bluetooth | wł./wył. |
| Blokada | blokada ekranu wł./wył. |
| Wygaszanie | 30 s / 1 min / 2 min / nigdy; przy zasilaniu z USB ekran nie gaśnie |
| Informacje | nazwa, **PIN Bluetooth** i klucz, firmware, pamięć, ostatni zrzut, napięcie baterii |
| Serwis | usuń dane, usuń parowania BT, nowy PIN BT, ustawienia fabryczne, nowa tożsamość |
| Wyłącz | uśpienie trackera; włączenie: naciśnij środek joysticka |

**Operacje z menu Serwis i wyłączanie** wymagają przytrzymania środka joysticka (2–3 s, z paskiem postępu):
- **Usuń dane:** kasuje punkty. Ostrzega, ile z nich nie zostało wysłanych.
- **Usuń parowania BT:** telefony trzeba będzie sparować ponownie.
- **Nowy PIN BT:** losuje nowy PIN, usuwa sparowane telefony i restartuje tracker.
- **Ustawienia fabryczne:** usuwa ustawienia, nazwę, dane i parowania BT. Klucz zostaje, więc tracker wraca na to samo konto, choć z nazwą `SF6Map-XXXX`.
- **Nowa tożsamość:** jak ustawienia fabryczne, ale z nowym kluczem, czyli nowym kontem. Wymaga dwóch kroków.

Wszystkie ustawienia zostają po wyłączeniu trackera.

**Blokada ekranu** (domyślnie włączona). Gdy ekran zgaśnie, tracker się blokuje. Przycisk tylko zapala ekran, a odblokowanie
wymaga przytrzymania środka joysticka przez 3 s (pasek postępu na dole ekranu). Przy zasilaniu z USB (tryb samochodowy) blokada i wygaszanie są wyłączone.

**Postój:** po 4 pingach w jednym miejscu tracker przestaje nadawać. Wznawia pracę dopiero przy prawdziwym ruchu,
czyli przemieszczeniu w ciągu ~20 s. Skok pozycji po ponownym złapaniu GPS albo odbicia sygnału nie wznawiają pingów.

**Ostrzeżenia:**
- **Pamięć:** przy 80%, 90% i 100% zajętości pojawia się komunikat i sygnał.
- **Słaba bateria (< 3,4 V):** mapowanie zostaje wstrzymane i wznawia się powyżej 3,55 V albo po podłączeniu USB.
- **Rozładowana bateria (< 3,3 V):** tracker sam się wyłącza, żeby nie uszkodzić zapisu.

**Możliwy limit repeatera:** pudło, przy którym któryś repeater odpowiedział nam już 4 razy w ostatnich 2 minutach,
dostaje znacznik. Na serwerze trafia on do kolumny `lim`, a w eksporcie do kolumny `mozliwy_limit`.

## Budowanie

Repozytorium zawiera tylko kod mappera. Workflow `.github/workflows/build.yml`
pobiera MeshCore w przypiętej wersji (`MESHCORE_REF`), wstawia mapper do
`examples/sf6_mapper` i buduje firmware. Rusza sam po każdym pushu na `main`,
a ręcznie przez Actions → „Build SF6 Mapper” → Run workflow. Gotowy plik `.uf2`
pobierzesz z sekcji *Artifacts* danego przebiegu.

## Wgrywanie

1. Podłącz tracker przez USB.
2. Kliknij szybko dwa razy reset. Pojawi się dysk USB.
3. Przeciągnij na niego plik `.uf2`.

## Bezpieczeństwo (od 1.1.3)

- **PIN Bluetooth jest losowy (6 cyfr)**, tworzony przy pierwszym uruchomieniu, a także po ustawieniach fabrycznych i nowej tożsamości.
  Sprawdzisz go w Menu → Informacje. Przy parowaniu telefon prosi o ten PIN.
  Telefony sparowane wcześniej (przed 1.1.3) działają dalej. Gdy chcesz je odciąć, użyj Serwis → Nowy PIN BT.
- **Podpis:** tracker podpisuje kluczem tylko wyzwania logowania Mappera. Nikt w zasięgu Bluetooth nie poprosi go o podpisanie czegoś innego.

## Ustawienia przy kompilacji

Można je nadpisać w `build_flags`:

| Flaga | Domyślnie | Opis |
|---|---|---|
| `COLLECT_MS` | 10000 | czas czekania na odpowiedzi |
| `STATIONARY_PINGS` | 4 | pingi w miejscu przed pauzą |
| `MOVE_THRESHOLD_M` | 30.0 | próg ruchu (m) |
| `MIN_SATS` | 4 | minimum satelitów |

Protokół BLE dla aplikacji opisuje `sf6_mapper/PROTOCOL.md`.


---

# Companion + SF6 Mapper (drugi firmware w tym repo)

Plik `Companion-SF6Mapper-WioTrackerL1-<wersja>.uf2` to **zwykły companion MeshCore** (oficjalny kod MeshCore,
interfejs ekranu „ui-new”) z wbudowanym SF6 Mapperem.

- **Jako companion:** działa jak zawsze z aplikacją MeshCore (wiadomości, **do 500 kontaktów** — oficjalny companion ma 350, 40 kanałów) i z Mapperem SF6 w telefonie
  (mapowanie z telefonu). PIN Bluetooth jest losowy i wyświetla się na ekranie, jak w oficjalnym firmware.
- **Mapowanie bez telefonu:** na ekranie companiona strona **„SF6 Mapper”**. Przełączasz ją joystickiem lewo/prawo,
  a środek joysticka włącza albo wyłącza mapper. Mapper pinguje co 20–60 s (tylko repeatery), zapisuje odpowiedzi z pozycją
  GPS do pamięci (1,5 MB, flash QSPI), robi pauzę na postoju i oznacza możliwy limit repeatera, tak jak tracker SF6 Mapper.
  Ustawienie WŁ/STOP zostaje po wyłączeniu. **STOP wymaga potwierdzenia** (drugie naciśnięcie w ciągu 4 s).
- **Blokada ekranu:** zgaszony ekran albo 60 s bez przycisku blokuje urządzenie. Przycisk tylko zapala ekran
  z podpowiedzią „Blokada: trzymaj OK”, a odblokowanie wymaga przytrzymania środka joysticka (na T-Echo przycisku) przez 3 s.
  W kieszeni nie da się więc przypadkiem wyłączyć mappera ani urządzenia.
- **Warunki pingowania:** radio companiona musi być ustawione na preset SF6 (869,618 MHz · BW 62,5 kHz · SF6), a GPS włączony
  z fixem. Ekran pokazuje, czego brakuje.
- **Gdy telefon sam mapuje** (Mapper SF6 wysyła pingi przez companiona), wbudowany mapper czeka, żeby nie dublować pingów,
  i wznawia pracę 2 minuty po ostatnim pingu telefonu.
- **Zrzut danych:** w aplikacji Mapper SF6 → Połącz pojawia się druga karta **„SF6 Mapper w companionie”** z liczbą punktów
  do wysłania, przyciskiem **Zrzuć dane na serwer**, „Pokaż na mapie”, interwałem i start/STOP. Punkty trafiają na konto
  companiona, czyli to samo co mapowanie z telefonu. Z trackera zostają skasowane dopiero po zapisie na serwerze.

Protokół zrzutu jest ten sam co w trackerze (`sf6_mapper/PROTOCOL.md`). W HELLO jest dodatkowa flaga `0x80`
(„companion z mapperem”), a zmiana nazwy działa przez aplikację MeshCore.

Kod: `companion_mapper/` (kopia `examples/companion_radio` z MeshCore + `CompanionMapper.*`), środowisko
`WioTrackerL1_companion_mapper_ble` w `platformio-sf6.ini`. Workflow buduje oba firmware naraz.

## LilyGO T-Echo

Ten sam „Companion + SF6 Mapper” jest budowany także dla **LilyGO T-Echo** (plik `Companion-SF6Mapper-T-Echo-<wersja>.uf2`,
środowisko `LilyGo_T-Echo_companion_mapper_ble`):

- **Ekran:** e-papier, interfejs oficjalnego companiona MeshCore dla T-Echo plus strona **„SF6 Mapper”**.
- **Przycisk:** krótkie naciśnięcie przełącza strony, przytrzymanie na stronie „SF6 Mapper” włącza lub wyłącza mapper.
- **Odliczanie:** na e-papierze nie ma odliczania sekund do pingu, bo każda zmiana wymaga odświeżenia ekranu.
- **Bez zmian względem Wio:** GPS, flash QSPI, 500 kontaktów, zrzut danych do aplikacji Mapper SF6.

Samodzielny tracker (`sf6_mapper`) jest tylko dla Wio Tracker L1, bo korzysta z joysticka i ekranu OLED 128×64.
