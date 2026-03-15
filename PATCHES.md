# pyrotek45/ardour — test/all-fixes patch notes

Applied on top of Ardour 9.3 (`983df8bc1f`).

## Fixes

- **build**: add SRATOM to libardour uselib (NixOS LV2 include path)
- **gtk2_ardour/main.cc**: don't raise SIGTRAP on fatal GLib/GDK log messages
- **editor_mouse**: fix Shift+Right-click delete on fade items; object-mode only
- **libs/pbd/mutex**: fix `Cond::wait()` mutex ownership after wakeup (waveview crash)
- **automation_line**: include point at region end boundary in `reset_callback()`
- **editor_ops**: paste automation at leftmost selected point, not timeline origin
- **editor_ops**: duplicate automation points with Ctrl+D alongside regions
- **editor_ops**: use region span as stride when duplicating automation
- **editor_ops**: auto-duplicate track automation when duplicating regions (Ctrl+D)
- **editor_ops**: fix `fast_simple_add` for automation duplication
- **selection**: fix `move_time()` so duplicate shortcut advances the range
- **midi_view**: fix piano-roll drag-copy position (`move_copies` `_on_timeline`)
- **midi_view**: fix note resize snap in Pianoroll (not on timeline)
- **midi_view**: fix note resize snap snapping to wrong bar grid
- **midi_view**: fix note resize snap — add local tempo map scope
- **midi_view**: fix snap misaligned when clip not on beat boundary
- **transport_control**: keep loop button lit when stopped in loop-is-mode
- **pianoroll**: add FL Studio-style draw mode (FL mode)
