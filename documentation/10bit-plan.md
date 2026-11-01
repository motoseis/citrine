# 10-bit pipeline plan

Status: draft for review. No code changed yet.

## Goal

Keep 10-bit source precision from decode through compositing and export, so that
grades and effects on 10-bit footage don't band, and 10-bit output can be written
when the user asks for it.

## Current state (8-bit end to end)

| Stage | Where | Today |
|---|---|---|
| Decode filter graph | `src/engine/cacher.cpp:261` | `format` filter forces `yuv420p\|nv12\|rgba`; 10-bit input is converted to 8-bit |
| Upload | `src/engine/clip.cpp:558-600` | Only YUV420P and NV12, uploaded as `R8` / `RG8` |
| YUV to RGB | `src/rendering/shaders/yuv2rgb.frag` | Samples 8-bit values; output `RGBA8` (`clip.cpp:585`) |
| CPU RGBA path | `src/engine/clip.cpp:735` | `RGBA8` |
| Composite / render | `src/rendering/renderfunctions.cpp:356`, `renderthread.cpp:215-243` | `RGBA8` render targets |
| Viewer readback | `src/rendering/renderthread.cpp:338-376`, `src/ui/viewerwidget.cpp:1062` | `RGBA8888` QImage |
| Export format | `src/rendering/exportthread.cpp:140` | First codec-advertised pixel format (8-bit for most) |
| Export RGB to YUV | `src/rendering/exportthread.cpp:265-275` | `sws_getContext` from `AV_PIX_FMT_RGBA` (8-bit) |
| VAAPI export | `src/rendering/exportthread.cpp:138-141` | Hard-coded NV12 surfaces |

## Phases

Each phase is shippable on its own and behind a check, so a bad phase can be
reverted without losing the earlier ones.

### Phase 1: 10-bit decode and upload (no visible change for 8-bit footage)

1. `cacher.cpp`: add `yuv420p10le` and `p010le` to the format filter list when the
   clip doesn't need CPU RGBA. Keep 8-bit formats as they are.
2. `clip.cpp`: add 10-bit branches to the YUV upload. Planes go into 16-bit textures
   (`QRhiTexture::R16F` / `RG16F`, or the normalised 16-bit formats if the backend
   supports them). Check which formats each RHI backend accepts before choosing.
3. `yuv2rgb.frag`: add a `format_type` value for 10-bit planar and P010. Rescale the
   samples: P010 stores the 10 bits in the top bits of each 16-bit word, and the
   limited-range offsets are 64/1023 and 512/1023 rather than 16/255 and 128/255.
   Keep the existing 8-bit values.
4. Rebuild the `.qsb` shaders (`qt6_add_shaders` in `src/CMakeLists.txt`, near line 609).
5. Output of the conversion stays `RGBA8` in this phase. The conversion is the
   only place precision is lost, so this phase already improves 10-bit footage
   slightly, but the real gain comes in Phase 2.

Risks: the 16-bit texture formats may not sample or render on the OpenGL ES fallback.
Plan to fall back to 8-bit (the current path) when the backend rejects them.

### Phase 2: 16-bit intermediate precision

1. Change `clip.cpp` output textures (`:585`, `:735`), `renderfunctions.cpp:356`, and
   `renderthread.cpp:215-243` to `RGBA16F`.
2. Check the blending, premultiply, and effect shaders for anything that assumes 8-bit
   values (clamps at 1.0 are fine with float; fixed-point tricks are not). This is
   most of the work: about 29 shader effects plus the internal ones.
3. Keep 8-bit readback for the viewer for now. Convert `RGBA16F` to 8-bit with
   dithering on readback (`renderthread.cpp:338`). Showing true 10-bit depends on the
   display path and is a separate decision (see Open questions).

### Phase 3: 10-bit export

1. Export dialog: add a bit-depth option that lists only codecs with 10-bit formats
   (HEVC Main10 `libx265` / `hevc_nvenc` / `hevc_vaapi`), selected
   from `avcodec_get_supported_config()`. Default stays 8-bit.
2. `exportthread.cpp:140`: choose `yuv420p10le` (or `p010` for VAAPI) when 10-bit is
   selected. The VAAPI path (`:138-141`) needs P010 surfaces instead of NV12.
3. `exportthread.cpp:265-275`: the RGB-to-YUV conversion needs 16-bit RGB input.
   Either render the export at `RGBA16F` and read back 16-bit data, or do the
   conversion on the GPU. Pick one after measuring the readback cost.
4. Set colour metadata for 10-bit output (currently BT.709 limited range is set at
   `exportthread.cpp:153-156`; keep it consistent with the conversion matrix used).

### Phase 4 (optional): HDR signalling

Out of scope for this plan. Only SDR 10-bit is covered (BT.709, PQ and HLG are not).

## Verification

- Build with `cmake --build build`. Also run `ctest --test-dir build`.
- Use a known 10-bit test clip (HEVC Main10 and a ProRes or p010 hardware-decoded clip).
  Check the decoded frame with `ffprobe -show_frames` and compare the pixel format
  reported by the cacher debug output.
- Gradient test: a 10-bit ramp clip through a brightness effect. Check for banding
  before and after Phase 2. Compare exported frames against an `ffmpeg` reference.
- Regression: an 8-bit project should render pixel-identical to the current build
  (before and after Phase 1). Use a fixed frame and compare checksums.
- Hardware: `vainfo` must list `HEVCMain10` for the VAAPI decode/encode path.
  Without it, the software path is the only 10-bit path on this machine.

## Risks and things that could break

- Effects that rely on 8-bit values will look different at 16-bit. Anything that
  quantises on purpose (posterise) would change visually.
- VRAM and bandwidth roughly double for `RGBA16F` intermediates. Check on modest
  hardware, which is the project's target.
- Frei0r and CPU-RGBA effects stay 8-bit. A clip that uses them loses the 10-bit
  benefit, so decide whether to convert or warn.
- Qt RHI format support differs by backend. Test Vulkan, OpenGL, and D3D if possible.

## Decisions

1. **Target release:** 2.0. 1.x stays bug fixes and security patches only.
2. **Viewer:** 8-bit dithered preview is acceptable. Phase 2 readback stays 8-bit.
3. **Export codec:** HEVC Main10 only (`libx265`, plus `hevc_nvenc` / `hevc_vaapi` when
   available). ProRes is not planned: it is only used for preview.

## Open questions

1. **Scope:** start with Phase 1 only, or go through Phase 3 in one pass?

## Suggested first step

Phase 1 on its own branch (`feature/10bit-decode`), starting with a 10-bit upload
test and the `yuv2rgb` change. It's the smallest change that proves the format
path works on your GPU.
