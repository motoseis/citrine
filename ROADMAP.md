# Roadmap

Citrine is a fork of Amber Video Editor. This roadmap lists what has shipped and what is still open. No new feature plans have been recorded yet.

## Shipped

### v1.09

- Renamed to Citrine with a new logo
- Drawline effect: option for constant line speed over the clip
- Fixed the highlight colour and undo behaviour when cutting a selected clip (Ctrl-K, Ctrl-Z)
- Next and previous marker navigation with Alt-Up and Alt-Down
- Global proxy generation: configured in Preferences, created automatically for imported and opened footage, ProRes Proxy format, "P" badge in the Project panel, and "Regenerate Proxy" and "Restore Original" in the Proxy menu
- Mouse wheel pans the timeline; Ctrl+wheel zooms (controlled by the Scroll Wheel Zooms preference)
- Dragging footage into a sequence centres the view on the sequence end

### Carried over from Amber 1.x

- Audio noise removal (RNNoise) and band-pass filter
- Text effect with keyframed reveal and animation
- Privacy mask, sharpen, LUT colour look-up, and highlight circle effects

## Open issues

- Restore Original is undone when a project is reopened, because proxies are regenerated automatically and there is no way to tell "restored" from "never proxied".
- Auto-generated proxies do not mark the project as modified, so their paths may not be saved until the next save.
- Regenerating a proxy overwrites the file even if an open clip is still reading it.
- The last few frames of a proxy may be dropped, because the decoder is not flushed at the end of the input.
- Proxy size is not checked for odd dimensions, which ProRes can reject.
- The cacher's stream-index mapping may shift when non-audio/video streams are dropped.
- The translation files in `src/ts/` are out of date with the proxy menu changes; run `lupdate` to refresh them.
- The mouse wheel fix (trackpad detection by device type only) has not been tested on Wayland.
- `src/project/proxygenerator_gemini.cpp` is an unused experiment and can be removed.
