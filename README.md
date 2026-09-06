# omaplayer

Natív Qt Quick videolejátszó Omarchy / Arch Linux rendszerre, a libmpv
render API-jára építve. Wayland-őshonos, hardveres dekódolás (mpv/NVDEC),
magyar felülettel.

## Funkciók

- **mpv motor** — széles kodek-, konténer- és hálózati támogatás; hardveres
  dekódolás (vaapi/nvdec) az mpv-n keresztül
- **Liquid-glass vezérlősáv**: áttetsző „üveg" pill (fokozatos átmenet,
  felső élcsillanás, lágy árnyék), kék akcentusú csúszkákkal; a sorminta a
  volumen csúszkától indul, majd vissza/lejátszás-szünet/stop/előre, a jobb
  szélen a lejátszási lista és a beállítások. Alul a filmszalag: elöl az
  eltelt, hátul a teljes idő; a kereső automatikusan folytatja a lejátszást,
  a bár igazodik az ablak méretéhez (keskeny ablakban a volumen és az
  előző/következő gomb-pár kicsúszik). Bal felső sarokban kis **művelet-jelző**
  villan, ami kiírja, mi történt épp (lejátszás/szünet, hangerő, némítás,
  keresés, leállítás, sebesség stb.)
- **Könyvtár (G) és Beállítások (L) fiókok** — azonos méret, egyszerre
  csak az egyik lehet nyitva, a vezérlősáv felett jelennek meg; a
  lejátszási lista **élőben követi a lejátszott elemet** (kiemelés)
- **Csoportos fájlbetöltés**: natív GTK fájlválasztó multi-selejtekkel
  (`Ctrl+O` megnyitás, „Fájlok hozzáadása a listához" — a lejátszást nem
  szakítja meg)
- **CLI**: több fájl parancssorból is indítható (`omaplayer a.mp4 b.mp4 …`)
- **Beállítások (3 fül — a fogaskerék gombbal / `G`)**:
  - **Videó**: videosáv-információ (felbontás/kodek), képarány (alap, 4:3, 16:9,
    16:10, 21:9, 5:4), körbevágás (azonos arányok + egyéni), elforgatás
    (0/90/180/270°), sebesség, hardveres dekódolás, váltott soros szűrő, HDR
    be/ki, fényerő/kontraszt/telítettség/gamma/színárnyalat csúszkák
  - **Hang**: hangsáv-választó, külső hang file tallózója, hang-késleltetés,
    szabadon állítható 10-sávos hangszínszabályzó (31 Hz–16 kHz)
  - **Felirat**: be/ki, feliratsáv-választó, külső felirat tallózója,
    késleltetés, pozíció, nagyítás, betűméret/betűtípus, szín, keret
    (szín+szélesség) és háttérszín választó
- **MPRIS** (D-Bus) — media-kulcsok, GNOME/Wayland média-integráció
- **Elrejtés a tálcára**: jobb gomb → menüpont (a lejátszás közben a
  StatusNotifier-alapú tálcaikonra kerül; kattintás visszahozza, jobb-gombos
  menüjében lejátszás/szünet, előző/következő és kilépés). Ha nincs tálca
  (nincs StatusNotifier-host), minimalizálásra esik vissza
- **Beépített frissítész**: „Frissítések keresése" menüpont a GitHub
  Releases-től ellenőrzi az új verziót; gombra letölti és **sudo nélkül,
  csendben** feltelepíti a `~/.local`-ba (bináris a `~/.local/bin`,
  asztali belépő és ikon a `~/.local/share` alá), majd **automatikusan
  újraindul** a friss verzióval, és a lejátszott fájl is folytatódik.
  A letöltést `~/.cache/omarchy/omaplayer/`-ban tartja
- **Intro / recap / kreditek kihagyása**: a fájl fejezetcímei (Intro, OP,
  Opening, Recap, Credits, ED, stáblista stb.) alapján a bevezető/
  visszatekintés/stáblista-szakaszban kihagyás-gomb jelenik meg felül
  középen, és a találatra a következő fejezetig ugrik
- **Képernyőkép** `Ctrl+S`-re
- `yt-dlp` telepítése esetén webes források (YouTube stb.) is lejátszhatók

## Billentyűk

| Billentyű | Hatás |
|---|---|
| `Szóköz` | lejátszás / szünet |
| `←` / `→` | keresés ±5 mp |
| `↑` / `↓` | hangerő ±10% |
| `M` | némítás |
| `F` | teljes képernyő |
| `I` | ablak méret ciklus |
| `G` / `L` | beállítások / lejátszási lista |
| `[` / `]` | sebesség felezése / duplázása (0.25–4×) |
| `N` / `P` | következő / előző lista-elem |
| `Törlés` | kijelölt lista-elem törlése |
| `Ctrl+F` | lista keresés |
| `Ctrl+O` | fájl(ok) megnyitása |
| `Ctrl+S` | pillanatkép |
| `Ctrl+0` | hangerő 100% |
| `Esc` | fiókok / teljes képernyő bezárása |

## Építés

Követelmények: Qt 6.5+, CMake 3.28+, Ninja, libmpv.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
DESTDIR="$PWD/stage" cmake --install build   # opcionális, csomagoláshoz
```

## Telepítés (Arch / Omarchy)

A repo `packaging/PKGBUILD`-je a `git+` forrásból, a `v$pkgver` tag-ből épül —
a csomag így mindig a GitHubon lévő állapotból indul:

```sh
cd packaging
makepkg -s
sudo pacman -U omaplayer-*.pkg.tar.zst
```

Függőségek: `qt6-base` `qt6-declarative` `qt6-wayland` `mpv`; opcionálisan
`yt-dlp` a webes lejátszáshoz.

## Megjegyzés a fájlválasztóról

A natív (GTK) választó abban az egyetlen mód, ami igazán multi-selected —
ezért az app kényszeríti a `gtk3` QPA témát (a `qt6-base` magában hordozza
a plugint). A választó megjelenése ~0.6 s (a GTK chooser belső felépítése),
a tematizálástól függetlenül; a Qt-fallback dialógus gyorsabb lenne, de csak
egyetlen fájlt enged kijelölni — a csoportos betöltés miatt a GTK-t használjuk.

## Licenc

MIT — lásd `LICENSE`.