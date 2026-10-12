# Notepad++ KEX

An unofficial build of [Notepad++](https://github.com/notepad-plus-plus/notepad-plus-plus) with:

- **Four editing views** in a 2×2 grid instead of two. Move or clone a document to any view, rotate the grid with the splitter, and give every new command a shortcut.
- **Snap-out views.** Drag a tab outside Notepad++ (or use *Move to New Window*) and that view becomes its own window; close it or drop it on the tab bar to snap it back.
- **Built-in KEX features**, each switchable in *Settings > KEX Features* (three pages in Preferences):
  - tab snapping: drag a tab to the edge of a view and a preview shows the new view it will open in; drop it on another view to move it there
  - JetBrains (IntelliJ) editor shortcuts: Alt+J next occurrence, Ctrl+W extend selection, Ctrl+/ comment, Ctrl+Y delete line, Shift+Alt+Up/Down move line and more
  - the same Notepad++ on every virtual desktop, with each desktop keeping its own active tab and position
  - falling letters when you delete text
  - a smooth, gliding caret
  - a quick note on a global hotkey (Ctrl+Alt+M): a small note fades in over any app and fades out on the next press
  - a tab strip that appears when you hover the top of a view in F12 mode
  - a glass text area (off until you tick it): Windows' live blur shows through the background of the text, while menus, toolbar and tabs stay solid

Not affiliated with the Notepad++ project. Built from the official source of each release.

## Install (Windows, x64)

1. Download `Notepad++-<version>-KEX.zip` from the [latest release](../../releases/latest).
2. Right-click it > Properties > tick **Unblock**, then extract it.
3. Either run `notepad++.exe` from that folder (portable, separate settings), or double-click **Install-KEX.cmd** to put KEX into your installed Notepad++ and, if you like, have new releases installed automatically.

`Uninstall-KEX.cmd` restores the official `notepad++.exe`. Details are in `KEX-README.txt` inside the zip.

## How it stays current

[`build.yml`](.github/workflows/build.yml) runs daily. When Notepad++ publishes a release, it:

1. clones that release and runs [`patch/codemod.py`](patch/codemod.py), which extends every per-view setting to views 3 and 4,
2. applies [the KEX patch](patch/) with a three-way merge (`patch/base/` holds the files it was written against),
3. copies in [the KEX features](suite/) and cross-compiles Notepad++ with MinGW-w64,
4. runs [the core tests](tests/KexTest.cpp) and the features' self-test under Wine,
5. publishes the official portable zip with the KEX `notepad++.exe` in it.

If a release changes Notepad++ too much for the patch, nothing is published and an issue is opened. To update the patch: `bash patch/kex-patch.sh <tag>`, resolve the conflicts, commit, export with `git format-patch -1`, copy the new pre-image files into `patch/base/`, and push. `ci/build.sh <tag> out` runs the whole build locally on Linux.

## For plugin authors

[`include/KexApi.h`](include/KexApi.h) lists the messages for the extra views. Plugins that don't know about them keep working in views 1 and 2; to also run in views 3 and 4, export `kexAware()`.

## Licence

GPL v3, like Notepad++ (see [LICENSE](LICENSE)).
