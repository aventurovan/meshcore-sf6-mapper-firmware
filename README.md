# MeshCore SF6 Mapper – firmware

Firmware do mapowania zasięgu sieci **MeshCore Południe (SF6)**. Dane z urządzeń zrzuca się przez Bluetooth do aplikacji
**Mapper SF6** (https://meshcoresouth.pl/mapper/, Android, iPhone).

| Firmware | Urządzenie | Plik |
|---|---|---|
| **SF6 Mapper (tracker)** – samodzielny tracker, tylko mapuje | Seeed Wio Tracker L1 / L1 Pro | `SF6Mapper-WioTrackerL1-<wersja>.uf2` |
| **Companion + SF6 Mapper** – zwykły companion MeshCore z mapowaniem bez telefonu | Seeed Wio Tracker L1 / L1 Pro | `Companion-SF6Mapper-WioTrackerL1-<wersja>.uf2` |
| **Companion + SF6 Mapper** | LilyGO T-Echo | `Companion-SF6Mapper-T-Echo-<wersja>.uf2` |
| **Companion + SF6 Mapper, bez ekranu** (np. uszkodzony e-papier) | LilyGO T-Echo | `Companion-SF6Mapper-T-Echo-bez-ekranu-<wersja>.uf2` |

## Pobieranie i wgrywanie

**Najnowsze pliki:** [Releases → „Najnowszy build”](../../releases/tag/latest-build). Każdy plik pobierasz osobno, bez ZIP-a,
a strona aktualizuje się sama po każdej zmianie w repo. Wydania z numerem wersji (np. v1.1.2) są w [Releases](../../releases).

1. Podłącz urządzenie przez USB.
2. Kliknij szybko **dwa razy reset**. Pojawi się dysk USB.
3. Przeciągnij na niego plik `.uf2`. Urządzenie samo się zrestartuje.

Pliki `…-ota.zip` służą do aktualizacji przez Bluetooth (np. nRF Connect). Zwykle nie są potrzebne.
Jeśli Chrome zablokuje pobieranie, w `chrome://downloads` kliknij przy pliku „Zachowaj”.

---

# SF6 Mapper (tracker) – Seeed Wio Tracker L1 / L1 Pro

Samodzielny tracker do mapowania zasięgu. Działa bez telefonu, a dane zrzuca się później przez BLE do aplikacji Mapper SF6.

## Co robi

- Radio ustawione na sztywno na preset SF6: 869.618 MHz, SF6, BW 62.5 kHz, CR 4/5.
- Po fixie GPS (co najmniej 4 satelity) co **20 / 30 / 40 / 50 / 60 s** (domyślnie 20 s) wysyła zero-hop discovery (tylko repeatery).
  Przez 10 s zbiera odpowiedzi.
- Każdy ping zapisuje z pozycją do flasha QSPI: trafienie (lista repeaterów z SNR w obie strony i RSSI) albo pudło.
  Pojemność to 1.5 MB, czyli około 35–40 tys. punktów.
- Po 4 pingach bez ruchu robi pauzę i nie nadaje. Wznawia dopiero przy potwierdzonym ruchu (szczegóły w punkcie „Postój”).
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

**Postój:** po 4 pingach bez ruchu tracker przestaje nadawać i wznawia pracę dopiero przy **potwierdzonym ruchu**:
przesunięciu o co najmniej 25 m w ciągu ok. 20 s, po w miarę prostej drodze, widocznym w 3 kolejnych odczytach GPS (ok. 6 s).
Drgania pozycji GPS w budynku, gdzie pozycja potrafi „skakać” o kilkadziesiąt metrów, skoki po ponownym złapaniu sygnału
ani odbicia nie są uznawane za ruch. Od wersji 1.1.4 ta sama reguła decyduje też o wejściu w pauzę, więc urządzenie
leżące na biurku zatrzymuje się po 4 pingach. Ruch wykrywany jest od ok. 3 m/s (rower, auto). Przy powolnym marszu
mapper może przejść w pauzę.

**Ostrzeżenia:**
- **Pamięć:** przy 80%, 90% i 100% zajętości pojawia się komunikat i sygnał.
- **Słaba bateria (< 3,4 V):** mapowanie zostaje wstrzymane i wznawia się powyżej 3,55 V albo po podłączeniu USB.
- **Rozładowana bateria (< 3,3 V):** tracker sam się wyłącza, żeby nie uszkodzić zapisu.

**Możliwy limit repeatera:** pudło, przy którym któryś repeater odpowiedział nam już 4 razy w ostatnich 2 minutach,
dostaje znacznik. Na serwerze trafia on do kolumny `lim`, a w eksporcie do kolumny `mozliwy_limit`.

## Budowanie

Repozytorium zawiera tylko kod mappera. Workflow `.github/workflows/build.yml` pobiera MeshCore w przypiętej wersji
(`MESHCORE_REF`), wstawia `sf6_mapper/` i `companion_mapper/` do `examples/` i buduje trzy firmware: tracker Wio,
companion Wio i companion T-Echo. Rusza sam po każdym pushu na `main`, a ręcznie przez Actions → „Build SF6 Mapper”
→ Run workflow. Gotowe pliki trafiają do wydania **„Najnowszy build”** (tag `latest-build`, nadpisywany przy każdym
buildzie) oraz do *Artifacts* przebiegu.

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
- **Ekran na Wio Tracker L1 (od 1.2.0): interfejs „MC SF6”**, cały po polsku, z polskimi literami:
  - **animacja startowa „MC SF6”** z falami radiowymi, nazwą i wersją, dwa piknięcia;
  - **pasek stanu** na każdym ekranie: godzina, kropka mappera (miga przy pingu), kłódka blokady, nieprzeczytane
    wiadomości, GPS z liczbą satelitów, Bluetooth (połączony = odwrócona ikona), bateria;
  - **ekrany** (joystick lewo/prawo): **Start** (nazwa, godzina, nowe wiadomości, stan mappera) → **Wiadomości**
    (najpierw wszystkie kanały, np. #Public, potem rozmowy prywatne; liczba nieprzeczytanych; kanał bez wiadomości też
    można otworzyć i napisać) → **Kontakty** (ulubione z gwiazdką na górze — także ulubione repeatery — potem ostatnio
    słyszane; przy repeaterach, pokojach i czujnikach typ zamiast czasu; OK → Napisz / Ulubiony / Zarządzaj (repeater) /
    Usuń kontakt; pierwsza pozycja **„> Usłyszane”**: ostatnio odebrane adverty, OK dodaje urządzenie do kontaktów) → **Mapper** (stan, a joystick w dół
    pokazuje ostatnie repeatery z nazwami i SNR) → **Repeatery** (słyszane repeatery według odległości, szczegóły:
    klucz, odległość i kierunek, pozycja, ostatni advert) → **Ustawienia**;
  - **Ustawienia:** Mapper (mapowanie, ping co, dźwięk, nowa sesja), Bluetooth (wł./wył., PIN), Radio (preset SF6
    jednym kliknięciem, moc nadawania, własne ustawienia: częstotliwość edytowana cyfra po cyfrze, szerokość BW, SF, CR —
    zapisywane dopiero przez „Zastosuj”; domyślnie po pierwszym uruchomieniu jest preset SF6), GPS (wł./wył., pozycja w advercie), Advert (do sąsiadów / do całej sieci),
    Ekran (wygaszanie 15/30/60 s/nigdy, blokada, budzenie przy wiadomości), Dźwięk (wiadomości, mapper),
    Informacje (przewijane góra/dół), **Odtwórz kontakty** (z kopii ostatnich 100 advertów zapisanych w urządzeniu), Hasła repeaterów, Restart, Wyłącz;
  - **emotki w nazwach** (urządzenia, repeaterów, wątków) są pomijane — ekran pokazuje tylko litery, cyfry i znaki;
  - **nowa wiadomość:** okienko z nadawcą i początkiem treści, dźwięk. OK otwiera wiadomość;
  - **pisanie (od 1.3.0):** w wiadomości OK → **Napisz** (klawiatura ekranowa z polskimi literami: joystick wybiera znak,
    OK wpisuje, wstecz usuwa, „Aa” wielka litera, „WYŚLIJ”), **Szybka odpowiedź** (OK, Jadę, Jestem na miejscu, Odbiór?…)
    albo **Moja pozycja** (współrzędne i link do mapy). Kanał otwierasz z listy Wiadomości, a pierwszą wiadomość do osoby piszesz z Kontaktów („Napisz”);
  - **wiadomości zostają po restarcie** (ostatnie 32, zapis w pamięci flash);
  - **Repeatery:** pierwsza pozycja „> Co tu słychać?” wysyła jednorazowy ping i pokazuje repeatery, które odpowiedziały,
    z SNR odbioru i nadawania. W szczegółach repeatera OK → **Ulubiony** (gwiazdka, zawsze na górze listy, ten sam znacznik
    co w aplikacji MeshCore; ulubione nie są usuwane przy pełnej liście kontaktów);
  - **zarządzanie repeaterem (od 1.4.0):** w szczegółach repeatera OK → **Zaloguj jako admin** (hasło z klawiatury,
    można je zapamiętać; „*” = hasło zapamiętane) albo **Zaloguj jako gość**. Po zalogowaniu:
    - **Stan:** czas działania, bateria, szum radia, ostatni RSSI/SNR, pakiety odebrane i wysłane, kolejka i błędy;
    - **Sąsiedzi:** repeatery słyszane bezpośrednio przez ten repeater, z SNR;
    - **Akcje** (admin): wyślij advert, moc nadawania, preset SF6, **pozycja = moja pozycja GPS**, zmień nazwę,
      synchronizuj czas, restart (z potwierdzeniem). Zmiana radia działa po restarcie repeatera;
    - **Konsola** (admin): `ver`, `get radio`, `get tx`, `get name`, `stats`, `clock` albo własna komenda z klawiatury,
      odpowiedź repeatera na ekranie;
    - zapamiętane hasła usuniesz w Ustawieniach → Hasła repeaterów.
  - **sterowanie:** góra/dół przewija listy, OK wybiera, wstecz cofa, przytrzymanie wstecz wraca na Start.
- **Skróty przycisku OK (Wio):**

  | Gest | Działanie |
  |---|---|
  | przytrzymanie 2 s | odblokowanie ekranu |
  | 2× szybko (na Starcie i ekranie Mappera) | mapper WŁ + GPS |
  | 3× szybko (na Starcie i ekranie Mappera) | mapper STOP + GPS wył. |
  | przytrzymanie 8 s (na każdym ekranie, także zablokowanym) | **SOS** na kanał Public z aktualną pozycją i linkiem do mapy |

  Od 5. sekundy trzymania widać odliczanie „SOS za … s”, a puszczenie OK anuluje wysyłkę. Bez fixu GPS wysyłana jest
  ostatnia znana pozycja (z dopiskiem), a bez żadnej pozycji sama prośba o pomoc.
- **Blokada ekranu (Wio):** po wygaszeniu ekranu. Na zasilaniu USB ekran nie gaśnie i się nie blokuje.
- **Ekran na T-Echo:** dotychczasowy interfejs companiona z dwiema stronami mappera („SF6 Mapper” i „Ostatnie RPT”)
  oraz menu mappera (przytrzymanie przycisku).
- **GPS włącza się i wyłącza razem z mapperem:** włączenie mappera (z ekranu albo z aplikacji) włącza GPS, a STOP go wyłącza.
  Po restarcie z włączonym mapperem GPS włącza się sam.
- **Sygnał przy odpowiedzi repeatera** (Wio, buzzer): krótkie piknięcie. Gra także przy wyciszonych wiadomościach,
  a wyłączasz je w menu mappera. T-Echo nie ma buzzera.
- **Mapowanie bez telefonu:** pingi co 20–60 s (tylko repeatery), zapis odpowiedzi z pozycją GPS do pamięci (od 1.5.2 maks. 512 kB, ok. 12–15 tys. punktów, bo flash QSPI jest wspólny z kontaktami i kanałami; przy mniej niż 384 kB wolnego miejsca mapper przestaje zapisywać i prosi o zrzut),
  pauza na postoju (te same zasady co w trackerze, punkt „Postój”), znacznik możliwego limitu repeatera. Ustawienia (WŁ/STOP, interwał, dźwięk)
  zostają po wyłączeniu.
- **Warunki pingowania:** radio companiona musi być ustawione na preset SF6 (869,618 MHz · BW 62,5 kHz · SF6), a GPS włączony
  z fixem. Ekran pokazuje, czego brakuje.
- **Gdy telefon sam mapuje** (Mapper SF6 wysyła pingi przez companiona), wbudowany mapper czeka, żeby nie dublować pingów,
  i wznawia pracę 2 minuty po ostatnim pingu telefonu.
- **Zrzut danych:** w aplikacji Mapper SF6 → Połącz pojawia się druga karta **„SF6 Mapper w companionie”** z liczbą punktów
  do wysłania, przyciskiem **Zrzuć dane na serwer**, „Pokaż na mapie”, interwałem i start/STOP. Punkty trafiają na konto
  companiona, czyli to samo co mapowanie z telefonu. Z trackera zostają skasowane dopiero po zapisie na serwerze.

### Mapper a połączenie z telefonem

Wbudowany mapper działa także przy połączonym telefonie. Zależy to tylko od tego, czy **telefon sam mapuje**:

| Sytuacja | Co robi mapper w companionie |
|---|---|
| Telefon niepołączony | mapuje sam (jeśli włączony) |
| Telefon połączony **aplikacją MeshCore** (wiadomości, kontakty) | **mapuje dalej**, połączenie mu nie przeszkadza |
| Telefon połączony **Mapperem SF6**, ale bez „Start mapowanie” | **mapuje dalej**; w aplikacji widać jego pingi w oknie LIVE (podgląd) i kartę „SF6 Mapper w companionie” |
| W Mapperze SF6 na telefonie włączasz **„Start mapowanie”** | **wstrzymuje się**, żeby nie było podwójnych pingów: mapuje telefon, a pomiary idą od razu na serwer |
| Zatrzymujesz mapowanie w telefonie | mapper w companionie **wraca sam po 2 minutach** |

- **Odpowiedzi na pingi wbudowanego mappera** zostają w companionie i nie trafiają do aplikacji, więc nie mieszają się
  z mapowaniem z telefonu.
- **Dane:** pomiary z telefonu lecą od razu na serwer, a pomiary z mappera w companionie czekają w jego pamięci,
  aż zrobisz „Zrzuć dane na serwer”. Wszystko trafia na to samo konto.
- **Warunki pingowania są zawsze te same:** radio companiona na presecie SF6, GPS z fixem, mapper włączony
  (na ekranie albo w aplikacji).

W praktyce mapper w companionie można zostawić stale włączony: w drodze mapuje sam, a gdy włączysz mapowanie
w telefonie, sam ustąpi.

Protokół zrzutu jest ten sam co w trackerze (`sf6_mapper/PROTOCOL.md`). W HELLO jest dodatkowa flaga `0x80`
(„companion z mapperem”), a zmiana nazwy działa przez aplikację MeshCore.

Kod: `companion_mapper/` (kopia `examples/companion_radio` z MeshCore + `CompanionMapper.*`), środowisko
`WioTrackerL1_companion_mapper_ble` w `platformio-sf6.ini`. Workflow buduje wszystkie firmware naraz.

## LilyGO T-Echo

Ten sam „Companion + SF6 Mapper” jest budowany także dla **LilyGO T-Echo** (plik `Companion-SF6Mapper-T-Echo-<wersja>.uf2`,
środowisko `LilyGo_T-Echo_companion_mapper_ble`):

- **Ekran:** e-papier, interfejs oficjalnego companiona MeshCore dla T-Echo plus strona **„SF6 Mapper”**.
- **Przycisk:** krótkie naciśnięcie przełącza strony. Przytrzymanie na stronie „SF6 Mapper” otwiera menu mappera,
  w którym kliknięcie przechodzi niżej, dwuklik wyżej, a przytrzymanie zmienia wartość. Blokadę zdejmujesz przytrzymaniem przez 3 s.
- **Dźwięk:** T-Echo nie ma buzzera, więc piknięcia przy odpowiedzi repeatera nie ma.
- **Odliczanie:** na e-papierze nie ma odliczania sekund do pingu, bo każda zmiana wymaga odświeżenia ekranu.
- **Bez zmian względem Wio:** GPS, flash QSPI, 500 kontaktów, zrzut danych do aplikacji Mapper SF6.

Samodzielny tracker (`sf6_mapper`) jest tylko dla Wio Tracker L1, bo korzysta z joysticka i ekranu OLED 128×64.

---

Firmware oparte na [MeshCore](https://github.com/meshcore-dev/MeshCore) (licencja MIT) — szczegóły w [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

### T-Echo bez ekranu (uszkodzony e-papier)

Plik `Companion-SF6Mapper-T-Echo-bez-ekranu-<wersja>.uf2` to ten sam companion z mapperem, ale bez obsługi wyświetlacza:

- **PIN Bluetooth jest stały: `123456`.** Zwykła wersja pokazuje losowy PIN na ekranie, a bez działającego ekranu
  nie dałoby się go odczytać. PIN można później zmienić w aplikacji MeshCore (Ustawienia → BLE PIN).
- **Mapperem sterujesz z aplikacji Mapper SF6:** karta „SF6 Mapper w companionie” (Włącz/Zatrzymaj razem z GPS,
  ping co 20–60 s, zrzut danych, podgląd LIVE). Ustawienia zostają po wyłączeniu, więc wystarczy raz włączyć mapper.
- Wiadomości, kontakty i kanały działają normalnie przez aplikację MeshCore.
