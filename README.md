# omaplayer

Natív Qt Quick videolejátszó Omarchy / Arch Linux rendszerre, a libmpv
render API-jára építve. Wayland-őshonos, hardveres dekódolás (mpv/NVDEC),
magyar felülettel.

## Funkciók

- **mpv motor** — széles kodek-, konténer- és hálózati támogatás; hardveres
  dekódolás (vaapi/nvdec) az mpv-n keresztül
- **Friss vezérlősáv**: keresősáv automatikus folytatással (a tekercselést
  szünetből a keresés után folytatja), hátralévő idő kijelzés, a bár
  igazodik az ablak méretéhez (keskeny ablakban a gomb-blokkok kicsúsznak)
- **Könyvtár (G) és Beállítások (L) fiókok** — azonos méret, egyszerre
  csak az egyik lehet nyitva, a vezérlősáv felett jelennek meg; a
  lejátszási lista **élőben követi a lejátszott elemet** (kiemelés)
- **Csoportos fájlbetöltés**: natív GTK fájlválasztó multi-selejtekkel
  (`Ctrl+O` megnyitás, „Fájlok hozzáadása a listához" — a lejátszást nem
  szakítja meg)
- **CLI**: több fájl parancssorból is indítható (`omaplayer a.mp4 b.mp4 …`)
- **Beállítások**: hangerő/speeds, fényerő/kontraszt/telítettség/gamma,
  felirat-méret, hang-késleltetés, feliratok ki/be
- **MPRIS** (D-Bus) — media-kulcsok, GNOME/Wayland média-integráció
- **Elrejtés a tálcára**: jobb gomb → menüpont (a lejátszás közben a
  StatusNotifier-alapú tálcaikonra kerül; kattintás visszahozza, jobb-gombos
  menüjében lejátszás/szünet, előző/következő és kilépés). Ha nincs tálca
  (nincs StatusNotifier-host), minimalizálásra esik vissza
- **Beépített frissítész**: „Frissítések keresése" menüpont a GitHub
  Releases-től ellenőrzi az új verziót; gombra letölti a csomagot és
  `sudo pacman`-nal feltelepíti
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