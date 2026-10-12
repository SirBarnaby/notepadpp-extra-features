Notepad++ KEX
=============

This is the official Notepad++ portable package of the same version, except that notepad++.exe
is built from Notepad++'s source with the KEX patch: four views, snap-out windows and the KEX
features below, built in. (plugins\KexSuite is an empty leftover for older update scripts; Notepad++
KEX ignores it and Install-KEX.cmd removes it.)
The updater folder is removed and disableNppAutoUpdate.xml is present, so an official update
can't silently replace the KEX build. (The official notepad++.exe would drop the four views.)

Before you start: right-click the zip > Properties > tick "Unblock" > OK, then extract it.
Windows may still say "unknown publisher" the first time, because this build isn't signed.

Two ways to use it
------------------
1. Try it on its own: run notepad++.exe in this folder. It keeps its settings in this folder
   and doesn't touch your installed Notepad++.
2. Put it into your installed Notepad++: double-click Install-KEX.cmd and allow the admin prompt.
   Your settings, sessions and plugins stay. If your Notepad++ is older than this build, the
   official installer of this version runs first (downloaded from Notepad++'s own GitHub and
   checked for Notepad++'s signature). Uninstall-KEX.cmd puts the official notepad++.exe back.

   At the end it asks whether to install new releases automatically. If you say yes, a daily
   task checks this project's GitHub releases; when a KEX build of a new Notepad++ comes out it
   downloads it and installs it the next time Notepad++ is closed, with a notification.
   Log: %LOCALAPPDATA%\KEX-Notepad++\update.log

Four views
----------
  * Right-click a tab > Move/Clone to Other View works as before; View > Move/Clone Current
    Document now also has Move to View 1-4, Clone to View 1-4 and Focus View 1-4.
  * The views form a 2x2 grid. Right-clicking the middle splitter and rotating it rotates the grid.
  * Snap out: drag a tab outside Notepad++, or View > Move/Clone Current Document > Move to New
    Window / Snap This View Out. Snap it back by closing its window, dragging its window onto the
    tab bar of Notepad++, or Snap All Views Back In.
  * All the new commands can be given shortcuts in Settings > Shortcut Mapper.
  * Older plugins only know two views. Their commands are blocked (with a message) while view 3
    or 4 has focus, so they can't edit the wrong file.

KEX features (Settings > KEX Features)
--------------------------------------
Settings > KEX Features opens Preferences on the "KEX Editing" page; "KEX Tabs", "KEX Windows",
"KEX Highlighting" and "KEX Quick Note" are just below it. Each feature has an on/off tick there
and its settings next to it; changes apply at once.
  * Tab snapping (KEX Tabs): press a tab anywhere (its name too) and pull; the tab lifts off the
    bar and follows the mouse. Towards the edge of a view a preview shows where a new view will
    open (half of that view, so a quarter of the window once there are two side by side); drop it
    there. Drop it on another view to move it into that view. Hold Ctrl to clone instead of move,
    Esc cancels. "Lift the tab off the bar" can be unticked. Switched off, dropping a tab on its
    own view shows the old Move/Clone menu.
  * A Notepad++ for each desktop (KEX Windows): starting Notepad++ (taskbar, Start, opening a
    file) on a virtual desktop that has no Notepad++ window opens a new window there instead of
    switching you back to the old one. The new window starts with the same tabs in the same
    views, the same active tabs, carets and scroll positions, and a copy of every untitled tab;
    files show what is saved on disk. After that the windows are independent, like Notepad++'s
    own multi-instance mode: the first window keeps saving your session, the others don't.
    Windows never move between desktops. On a desktop that already has one, Notepad++ behaves
    as usual.
  * Falling letters: deleted text falls off the screen, CS2D style.
  * Smooth caret: the caret glides to its new position.
  * JetBrains shortcuts (KEX Editing): the IntelliJ editor keys in the editing views, e.g.
    Alt+J / Shift+Alt+J / Ctrl+Shift+Alt+J next / unselect / all occurrences (by default they
    match case and whole words the way Smart Highlighting does; "Alt+J case" and "Whole words"
    on the page change that), Ctrl+W and
    Ctrl+Shift+W extend / shrink the selection, Ctrl+/ comment, Ctrl+D duplicate, Ctrl+Y delete
    line, Shift+Alt+Up/Down move line, Alt+Left/Right switch tabs, Shift+Enter new line below.
    "All shortcuts..." on the page lists them. Untick "Also the keys Notepad++ uses" to keep
    Notepad++'s own Ctrl+Y (redo), Ctrl+W (close), Ctrl+R, F11 and Ctrl+Num +/- (zoom).
  * Quick note (KEX Quick Note, Ctrl+Alt+M by default): a small borderless note fades in at the
    centre of the screen over whatever you're doing, even with Notepad++ minimised. Press it again
    (or Esc) to fade it out and go back to what you were doing. Drag its top edge to move it and
    its edges to size it. What you type is saved as you go (kex-quicknote.txt in Notepad++'s
    settings folder) and is there next time. Options: line numbers, word wrap, current line
    highlight, always on top, and opening where you left it. With a Notepad++ on several desktops,
    the first window answers the hotkey and opens the note on the desktop you're on.
  * F12 hover tabs: in F12 (post-it) mode, touching the top edge of any view shows a floating
    strip with that view's tabs.
  * Glass text area (KEX Windows, off by default): Windows' own blur shows through the
    background of the text, live, so it follows the window as you move it. Menus, toolbar, tabs
    and title bar stay solid. Style: Blur (default) or Acrylic (Windows' frosted texture, much
    stronger; it can lag while you drag the window on Windows 10). Tint (%) sets how much of the
    theme colour stays on top of the blur (default 40). Text edges: Sharp (default) draws the text
    without smoothing while the glass is on, because smoothed letters always show a thin outline
    of the theme's background on the glass; Smooth and "As Notepad++ draws them" keep smoothing.
    "Margins too" puts the line numbers and folding margin on glass as well. Clicks on the glass
    go to the text under them. Needs Windows' "Transparency effects" switched on (Settings >
    Personalization > Colors). If it misbehaves on your PC, kex.ini [Glass] takes Engine=self
    (blur on the window itself) and KeepRenderer=1.
  * Live token styles (KEX Highlighting): Search > Style All Occurrences of Token (also in the
    right-click menu) keeps following the text: occurrences you type or paste later get the style
    too, and one you edit into something else loses it. The same submenu has Bold, Italic and
    Clear This Token. Ctrl+Shift+H (changeable) pops the same choices up at the caret. Clear Style
    > Clear all Styles clears them. Bold and italic work in every language, plain text included.
    Match case and whole word follow Notepad++'s Highlighting page;
    the colours are Mark Style 1-5 in the Style Configurator. The styled tokens of each file are
    remembered (kex-tokens.txt in Notepad++'s settings folder) unless you untick that.

New Notepad++ versions
----------------------
The four views are a source patch. A GitHub Actions build applies it to every new Notepad++
release, tests it, and publishes the result (see kex-repo.txt for the repository). If a release
ever changes Notepad++ too much for the patch, that build stops, an issue is opened in the
repository, and you keep the version you have until the patch is updated.

source\ holds everything this was built from (patch, KEX feature source, KexApi.h for plugin authors).
Notepad++ is GPL (license.txt); the KEX changes are under the same licence.
