# Feljegyzett ismert hibák

## Képarány/crop bevitel (0.1.9) — megoldva

### A második crop mező (Magasság) nem írható — ROOT OKA és megoldás
- Jelentés (2026‑09‑06, user): az "Egyéni" crop mezőibe nem lehet fizikailag
  átgépelni; amikor az ember a mezőbe kattint, előbb törölni kell (szóközök),
  a második mező (Magasság) pedig egyáltalán nem fogadott be karaktert.
- Tünetek/bizonyítékok:
  - `QT_IM_MODULE=fcitx` + fcitx5 (DBus plugin) a Wayland alatt elnyelte a
    fizikai billentyűket: a gombok megérkeztek az ablakhoz (keys.log:
    `af=… focus=1`), de a szöveg sosem jutott a TextField‑be.
  - A `999999` inputMask üres helyei ("     ") szóköznek látszanak; a kurzor
    az utolsó slot utánra esett, ezért addig nem fogadtak gépelést, míg
    vissza nem léptek.
  - A szintetikus kattintás (QWindowSystemInterface) nem érte el a scene‑et
    (af nem váltott) → a kattintás‑fókusz‑gépelés ösvényt csak user teszttel
    lehetett ellenőrizni.
- Megoldás (user teszttel igazolva — "működik"):
  1. `qunsetenv("QT_IM_MODULE")` az `app` létrehozása előtt src/main.cpp‑ben
     → a fizikai billentyűk eljutnak a mezőkbe (ez az 1. mezőt megjavította).
  2. A `999999` maszkot `IntValidator { bottom: 1; top: 999999 }`‑re cseréltük
     → nincs üres maszk‑töltelék, kattintás után azonnal lehet gépelni.
  3. `onActiveFocusChanged: if (activeFocus) Qt.inputMethod.reset()` mindkét
     crop mezőn → a kompozitor text‑input kontextusa újrakötődik az épp
     fókuszált szerkesztőhöz, nem a korábbi, telített mezőhöz.
  4. Az "Egyéni" megnyitásakor a mezők törlődnek és a Szélesség kap fókuszt.
- Érintett fájlok: src/main.cpp (qunsetenv), qml/Main.qml (validator, reset,
  auto‑clear/focus). Qt 6.11, fcitx5 5.1.22, Hyprland 0.56.