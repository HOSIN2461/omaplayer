# omaplayer

Magyar videolejátszó Omarchy (Arch Linux) rendszerre — Qt Quick és a libmpv
render API-ra építve. Natív Wayland, erős hardveres dekódolás (mpv/NVDEC),
magyar felület.

## Funkciók

- mpv motor (`libmpv` render API + property/command API) — széles
  kodek-, hálózati és eszköz-támogatás
- Magyar nyelvű felület: lejátszás/szünet, kereső sáv előnézeti
  buborékkal, hangerő-csúszka, idő megjelenítés
- Picture-in-Picture: `--pip` kapcsolóval automatikusan is (vagy gombbal /
  `P` billentyűvel), átméretezhető ablak, a videóra kattintva kilép
- Teljes képernyős mód
- Billentyűk: `←`/`→` keresés ±5 mp, `↑`/`↓` hangerő ±10%
- URL-ek is lejátszhatók (yt-dlp telepítése esetén YouTube és társai is)

## Építés

Követelmények: Qt 6.5+, CMake 3.28+, Ninja, libmpv.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
```

## Telepítés

Csomag a `packaging/PKGBUILD`-del (felhasználóként, root nélkül):

```sh
cd packaging
makepkg -Cf
sudo pacman -U omaplayer-*.pkg.tar.zst
```

## Használat

```sh
omaplayer film.mp4
omaplayer "https://www.youtube.com/watch?v=..."
omaplayer --pip film.mp4   # azonnal picture-in-picture
```

## Ismert problémák

- **NVIDIA 580.178.04 driver összeomlás (SIGSEGV az `libnvidia-eglcore`-ban
  `QRhi::endFrame` közben).** A Qt Quick alapértelmezett RHI beállítása
  kiválthatja; a fix: `QSG_RHI_BACKEND=opengl` a `QGuiApplication` előtt
  (a `src/main.cpp` ezt már beállítja). Más backendar kipróbálásához:

  ```sh
  QSG_RHI_BACKEND=vulkan omaplayer film.mp4
  ```