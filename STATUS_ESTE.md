# Állás 2026-09-08 éjjel — holnapi kezdéshez (NE commitold, munkaközi jegyzet)

## Ami MŰKÖDIK (tesztelve)
- Google Cast Sony KD-55XD8505-re: keresés → LOAD → PLAY pozícióval (logban BUFFERING→PLAYING)
- DLNA-felfedezés: SSDP + mDNS, auto-retry, stabil sorrend; DLNA SOAP változatlan
- MKV (H.264 + E-AC-3 5.1) → sztereó MP4 auto-konvertálás gyorsítótárral (`~/.cache/omarchy/omaplayer/cast/`), atomi `.part` írás
- Skipper: kivonat-ujjlenyomat (head 12p / tail 15p, video-copy, másodpercek) + gyorsítótár (`intro-v1/`)
- Stáblista-heurisztika: black-frame a végén, Landman E02-n 54:08-ra talált (szemre 54:09) ✓
- Előkészítő-kártya %-kal, PIN-sor auto-ürítéssel, kapcsolódás/lekapcsolódás gombok
- ufw: `allow from 192.168.1.0/24 to any port 8099 proto tcp` BEÁLLÍTVA — kell a TV→gép lehíváshoz!
- Telepítve: `~/.local/bin/omaplayer` (= build, 00:30 utániakra figyelj)

## 09-09 este (AirPlay 2 session) — EREDMÉNY
- Új kód (0.2.9, telepítve): `Bplist.*` (encoder Apple-plistlibbel ellenőrizve),
  `AirPlaySession.*` (nyers socket, pair-verify + HAP framing + RTSP + event
  csatorna + NTP timing + /feedback), CastManager átkötve.
- ✅ verify, titkosított control-csatorna, SETUP (eventPort!), event-csatorna,
  RECORD, /info, OPTIONS — MIND megy mentett credentiallal, PIN nélkül.
- ❌ `/play` + `/playback-info` + `/stop` + `/rate` + `/scrub` → **404**
  (üres body). Kipróbálva: bplist-full/mini, XML, text/parameters, RTSP-token,
  Session-header, Host-header, auth-setup (403), fp-setup (404!). Azonos a
  pyatv #1518-cal (LG webOS, 2021 óta nyitott — LG-n nincs /play!).
- `/command` → 400 (létezik! MRP-alagút?), `/auth-setup` → 403.
- Következő opciók: (a) DLNA a LG-re (TV-oldali beállítás kellhet),
  (b) iPhone-nal igazolni hogy az URL-AirPlay egyáltalán megy-e ezen a TV-n,
  (c) MRP-/command videó (nagy meló), (d) parkolás, Sony Cast marad.
## 09-09 21:25 — MIRROR PUSH: TV feketét ad, de phase-2 nem áll fel
- iPhone→PC(UxPlay) capture KÉSZ: teljes mirror-handshake (fp M1/M2/M3/M4 +
  ekey/eiv bytes, SETUP#1/#2, RECORD, event, NTP, TEARDOWN). LG-folyam:
  verify→fp 404 (nincs fp LG-n!)→mirror SETUP#1 200 (ekey-vel ÉS anélkül is!)
  →RECORD 200→phase-2 video: hol lóg ~22s→400, hol gyors 400; audio(ALAC)
  phase-2 szintén lóg. TV közben FEKETE (pipeline indul!).
- Gyanú: a gyors egymásutáni probe-sessionök mérgezik a TV állapotát
  (első probe: record timeout+500; későbbiek: record 200+phase-2 400).
  TERV: TV-t pihentetni pár percet, majd EGY tiszta futás. Ha úgy sem megy:
  DLNA-toggle a TV-n (5 perc, meglévő kód viheti!), különben FP-encrypt
  port + RTP-stack (napok).
## 09-09 21:40 — MIRROR MÉRLEG (máras leállítva az UxPlay-rig)
- phase-2 videó (type 110): min→gyors 400; full→~22s lógás→400. Kipróbálva:
  ekey replay / ekey nélkül / et nélkül / pontos iPhone streamConnID /
  event-tel / event nélkül (RECORD-hoz KELL!) / pihent TV. dataPort SOHA.
- TV a phase-2 alatt FEKETE (pipeline indul!), de csatornát nem oszt.
  ekey/no-ekey viselkedés AZONOS → a kulcs valószínűleg NEM a blokk.
- Megmaradt utak: (P1) FP-encrypt port PlayFair-inverz (1 nap, bizonytalan —
  LG licencelt Apple-FP-t használ, a PlayFair-burok elvileg jó bele);
  (P2) MFi /auth-setup (Apple-privátkulcs kell → VALÓSZÍNŰLEG LEHETETLEN);
  (P3) parkolás — Sony Cast ma is visz mindent.
- KÉSZ kód (0.2.9, telepítve, NEM commitolva): Bplist, AirPlaySession
  (verify/HAP/RTSP/event/NTP-timing/feedback + URL-flow + mirrorProbe),
  CastManager-átkötés. Újrahasznosítható bármely folytatáshoz.
## 09-09 22:20 — SAJÁT FÁJLTALLÓZÓ (GTK vége)
- Új `qml/FileBrowser.qml` (modális, ablakon belüli Item — nem Popup!):
  openFiles/openFile/saveFile módok, FolderListModel, multi-checkbox,
  mentésnél fájlnév-mező (+.m3u auto), rejtett-toggle, Up-nav, lastDir
  megjegyzi (Qt.labs.settings). Az 5 FileDialog helyette megy (azonos id-k!),
  `import QtQuick.Dialogs` + gtk3-kényszer (main.cpp) TÖRÖLVE.
- Build tiszta (QML-cache is fordul), qmllint rendben, offscreen füstteszt:
  app 12 mp-ig él (124 = timeout ölte meg, nincs QML-fatal).
- HÁTRA: kattintás-teszt mind az 5 helyen (megnyitás/hozzáadás/hang/felirat/
  mentés) + hogy sehol nem jön fel GTK.
## 09-09 22:00 — Airplay2OnWindows (C# vevő) tanulságok a küldőhöz
- Mirror videó-TCP: 128B header [u32 size][u16 type][u16 option][u64 ntp→pts
  | type1: float w/h @40,56] + payload. Type 0 = AES-CTR AVCC-videó (4B NAL-
  hosszak!), type 1 = SPS/PPS config. Videó-kulcs VÁLTOZAT (UxPlay-jel
  szemben!): eaes=SHA512(audioKey16||ecdhShared); key=SHA512("AirPlayStreamKey"
  +ID||eaes[0..16])[0..16] (IV ugyanígy "…IV…" prefixszel). → m_shared
  MOSTANTÓL MENTVE AirPlaySession-ben (későbbi empirikus döntéshez)!
- ekey/eiv a C#-vevőnél is Opcionális SETUP-szinten; FP-handshake viszont
  kapuz (nincs fp → 400). LG-n nincs fp-endpoint → keyless út elvben járható.
  Ports: (ushort)(short) konverzió = az unsigned-16 olvasásom HELYES.
## 09-09 20:30 — DÖNTÉS: az LG nem tud URL-videót, csak tolt H264-et
- iPhone-ról a Files-videó is csak TÜKRÖZÉSSEL megy → a TV-n nincs `/play`
  (V2-only: bit0=0/bit49=1). A 404 tehát TV-korlát, nem a mi bugunk. MRP-vonal
  lefújva (az sem indítana URL-t). Helyes irány: **H264-push** (mirror-cső:
  ANNOUNCE video-SDP + SETUP + RECORD + titkosított RTP/H264, ffmpeg-forrás).
  A mostani AirPlaySession 90%-a (verify/HAP/RTSP/event/timing/feedback)
  újrahasznosítható. Előbb DLNA-gyorsteszt (ha a TV ad renderert, az percek).
1. **AirPlay PIN (LG 43NANO763QA, 192.168.1.102)**: HAP-kliens kész, SRP-matek független Python-ellenőrzéssel bájtra stimmel (lásd `/tmp/opencode/srpcheck.py`), TLV-sorrend pyatv szerint, H(g) minimális kódolás javítva. A TV minden proofot Error 2-vel dobott — gyanú: kijelzett PIN ≠ munkamenet PIN, vagy LG-specifikus eltérés. Protokoll: EGY begin → 5 mp → leolvas → azonnal beír+OK (15 mp-en belül), PIN-t ide is beírni!
2. **HLS élő session** (`src/HlsSession.*`): kód kész + helyben igazolva (playlist/szegmensek ffprobe-val), de TV-n még nem ment át rajta vetítés. Ha az MKV-vetítés HLS-t indít és akad, a log (`/tmp/opencode/cast-debug.log`, `hls` sorok) mondja meg.
3. **Recap-menet**: kód kész (korábbi részek head/tail vs első 3 perc), de pozitív teszteset nem volt (Landmanben nincs ismétlődő intró/outro). Kellene anime/szitkom próba.
4. **Jellyfin MediaSegments** (`GET /MediaSegments/{id}`): kód kész, szerver-oldali provider megléte ismeretlen — Jellyfin-epizód indításakor kiderül (ha nincs, csendben marad).
5. **GTK dialógus**: portal-téma zsákutca (QML FileDialog ki sem nyílik). Marad GTK (~1 mp beállási idő, addig nem kattintani) vagy saját tallózó (#3 részeként).

## Fontos tények
- Sony = 192.168.1.135 (TLS) / .35 + .77 is felbukkant (több IP!); LG = 192.168.1.102 (csak AirPlay PIN-nel + 403); Xiaomi = .177; SmartBox = .218
- TV backoff: hibás M3-ak után 19 mp → 2 → 8 → 17 → 26+ perc! Csak fegyelmezett körökkel.
- Teszt-MP4-ek: `/mnt/data/.../Landman.S01.../Landman.S01E01.{cast,stereo}.mp4`, `cast-test.mp4` (törölhetők, ha nem kellenek)
- Debug: `/tmp/opencode/cast-debug.log` (cast+hls+skip+pair sorok) — rebootkor /tmp ÜRÜL!
- Crashes: volt SIGABRT (nested-event-loop QML-fatal) — javítva `requestCast`/`finishAirPlayPairNow` deferrel. Ha újra crashel: `coredumpctl`.
