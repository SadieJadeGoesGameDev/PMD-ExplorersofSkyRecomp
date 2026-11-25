# Sky Recomp — working notes

## Layout
- tools/inventory.py   — function inventory (pmdsky-debug + call/pointer scan) -> inventory.json
- tools/lifter.py      — ARM -> C; tools/thumb.py — Thumb -> C
- tools/lift_all.py    — lifts everything into gen/*.c, gen/dispatch.c (overlay fingerprints), gen/stubs.c
- runtime/cpu.h        — shared CPU state (R points at melonDS ARM9 regs), TICK/CHECK, TRACE_FN ring
- runtime/hybrid.cpp   — fiber runtime: native ARM9 inside melonDS; interp fallback; IRQ/SWI via trampoline
- runtime/native.c     — native SDK div helpers
- referee/harness.cpp  — headless melonDS (RECOMP=1 enables native ARM9; RECOMP_TRACE=1 traces)
- build/build_linux.sh — builds recomp_harness (CFLAGS_EXTRA=-DRECOMP_TRACE_FN for call ring)
- melonds-recomp.patch — melonDS changes (NativeExecuteHook, ARMv5::StepOne). melonDS at /tmp/melonDS (core built w/ GCC LTO -> link with g++ -flto)
- Windows toolchain: /tmp/toolchain/llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64

## Status (2026-11-24)
- 11,681 functions lifted (161 Thumb), 0 unsupported insns, all compiles.
- Hybrid boots: crt0 + init run natively, SWI via BIOS (trampoline at ITCM 0x01FFFF00), IRQs via BIOS->native handler.
- Fixed: 'ldr lr,=X; bx rN' = call returning to X; 'ldr pc,[sp],#4' = return.

## Threads (solved 2026-11-24)
- Per-thread fibers keyed by resume (pc,sp); exception returns that land elsewhere = thread switch.
- OS_LoadContext + ITCM OS_IrqHandler/ReturnFromInterrupt are interpreted (EXCLUDE); OS_SaveContext wrapped.
- Every native frame saves entry_lr/sp; after each call `if (RET != expected) divert()` re-syncs.
- Native helpers must set RET.
- MILESTONE: vanilla ROM reaches the title screen with native ARM9 (2400 frames in ~20s on Linux harness).

## Old notes: Nitro SDK threads
- OS_SaveContext 0x2079D08 / OS_LoadContext 0x2079D54 (setjmp/longjmp-style), OSi_RescheduleThread 0x2079340,
  OS_IrqHandler_ThreadSwitch (ITCM). Native call stack can't follow a thread switch.
- Plan: one host fiber per game thread (keyed by context ptr); native SaveContext/LoadContext switch fibers;
  IRQ-exit thread switch: detect exception return to (pc,sp) not our stop -> switch to the fiber waiting there or spawn one.
- Also: per-frame entry_lr/entry_sp + RET check after calls, to recover when a callee returns somewhere unexpected.

## Windows + presentation (2026-11-24)
- build-win/build_win.sh -> SkyRecomp.exe (llvm-mingw, static SDL2 2.32.10 at /tmp/sdl, melonDS core /tmp/melonDS/build-win)
- frontend/main.cpp: ROM check (code CRC CFB1B178), .sav next to ROM, single-screen (bottom = game; auto top when bottom dark), PiP, F1 stacked, F4 wide
- Widescreen v1: melonDS patch (GPU_Soft DrawWideMargins, GPU2D_Soft WideEdge): re-render each line with BG X scroll +-delta and OAM X shifted;
  256-wide text BGs + affine/bitmap BGs skipped in margins; window mask = edge value. Margin 43 -> 342x192.
- TODO widescreen v2: game-side tilemap streaming + sprite culling widen (right edge black in overworld).
- Saves: user's AR .duc converted (strip 500-byte header) -> PMD_Explorers_of_Sky_USA.sav (256KB, chip is 128KB, works).
- Frame pacing: overworld animates ~30fps (136/300 frames changed while walking).

## Save hang (fixed 2026-11-24, v0.3.10)
- ARM-mode SWI comment must be passed as (w>>16)&0xFF; was w&0xFFFFFF then <<16 in runtime -> garbage SWI number.
  Save code calls debug semihosting swi 0x123456 (sys_writec) -> became SWI 0x56 -> FreeBIOS soft reset -> hang.
- Runtime also handles real reboots: FreeBIOS ARM9 reset at FFFF06FC waits for ARM7 to write entry at 027FFE24,
  and a jump to 0x02000800 after boot = clean restart (reset_world: fresh fiber, waiting cleared).
- recomp_log.txt / recomp_log_previous.txt next to exe; watchdog; vectored crash handler.

## v0.3.12 (2026-11-24)
- Save hang root cause B: lifted 64-bit SDK helpers (_ll_mod/_ll_sdiv/_ll_udiv/_ull_mod/_ll_mul) branch into each other's bodies and use lr as scratch -> bad return -> jump to FFFF06FC. Replaced natively in runtime/native.c and added to NATIVE. Pending Sadie's test (well save + Beach Cave quicksave + Continue).

## v0.4.0 widescreen v2 (2026-11-24)
- Dungeon BG3 (512x256 ring, RAM copy 0x21B6EE0 DMA'd): ov29 0x2336F4C full redraw (33 cols x 27 rows from camX>>3),
  0x233711C edge column (r0=0 left / 255 right), 0x23372A4 edge row (33 cols, r0=x off, r1=0/192). camX at [*0x2353538 + 0x1A224].
  Native wrappers draw +-48px (45 cols). 
- Dungeon darkness = 3D BG0 translucent polys, BLDCNT 3E41. melonDS margin now replicates translucent edge 3D pixel (WideLine3D).
  Known gap: when the lit hole touches the screen edge the margin is undarkened. Proper fix = widen 3D clip/raster (FinalPosition is 9-bit masked).
- Sprite X cull arm9 0x201B9B0 (x<-64||x>=256) widened natively by 48. Y cull 0x201B9D4 untouched.
- IsPositionVisibleAndOnScreen x 6->8 tiles via CODE_PATCH in lift_all (ov29 0x22E2CD0, 0x22E2D6C).
- Overworld: ov11 0x22EED98 = 12x9-chunk window refresh (chunk 3x3 tiles), native version writes 16 chunks (k=-2..13), blank outside map width [o+8].
  Wrap-capable sibling 0x22EEFC0 not widened yet. Small maps (Crossroads) legitimately end -> black past map edge.
- Harness: DUMPBG=1 (regs, OAM, VRAM dumps), WATCHAT/WLO/WHI bus write watch (melonDS ARM9WriteWatch hook in NDS.cpp).
- v0.4.1: ground refresh variants all native (ov11 0x22EEAAC 1L, 0x22EEBF8 1L wrap, 0x22EED98 2L, 0x22EEFC0 2L wrap).
  Camera: ov11 0x22EF2D4 = set camera (ground_bg+0x200+layer*8) — native clamp to [W, max-1-W] (max=[map_render+0x10]=mapW-256),
  centre if max<=2W; skip wrap maps ([map_render+6]). 0x22EF2EC is the vanilla clamp (left vanilla). recomp_wide_margin set by frontend F4.
- v0.4.2: v0.4.1's clamp at the BG setter desynced sprites (sprites follow the screen camera *centre* target at
  0x23891F8+idx*0x54+0x48, not ground_bg+0x200). Correct hook: ov11 0x22F1F3C(min*,max*) = camera centre limits
  (8.8 fixed, map bounds +-128px/96px). Native wrapper shrinks x by W<<8, centres if lo>hi. 0x22EF2D4/0x22EF2EC back to vanilla.

## v0.5.x — Recomp Options, filters, Enhancements (2026-11-25)
- Frontend: SkyRecomp.ini (settings), filter worker thread (xBRZ 4x = 4xBR port; Scanlines = gaussian beams at output height),
  display windowed/borderless/fullscreen + resolution list, save slots (slot1 <rom>.sav, slotN <rom>_slotN.sav, switch = relaunch).
- In-game menu (native.c): GetStringFromFile hook serves private IDs 0x7F00+ (and Sadie string overrides);
  CreateParentMenuFromStringIds hook injects "Recomp Options" (result 9) into top ground menu tables and dungeon menu
  (0x2389DD4, window +1 row). UpdateTopGroundMenu (ov11 0x2300F50, intercept at state 4) and DungeonMenuLoop
  (ov31 0x2382B54, intercept at state 3) run our state machine (parent/options/simple windows via gcall into the game).
  Scratch data in unused ITCM 0x01FFC000-0x01FFC700.
- Enhancements: tools/sadie_patches.py assembles each patch group vs vanilla (team control's ov36 code -> ITCM 0x01FFD000,
  own helpers 0x01FFD500), emits runtime/sadie_patches.h byte runs + tools/sadie_funcs.json. Touched functions get an
  interpreter prologue + dispatch 'sadie' mask (lookup returns null when group on). RAM re-patched only on LoadOverlay
  (arm9 0x20040AC hook) + once at boot, so toggles take effect at the next floor/area load.
- v0.5.2: Any Starter's new code (ov13 +0x2E80, past vanilla ov13 end -> RAM owned by something else) relocated to ITCM 0x01FFD600.
  Verified New Game -> quiz -> bow (TOUCH key in rig) -> "Is Turtwig who you want? Yes/No" -> partner pick. ITCM map: 0x1FFC000 menu scratch,
  0x1FFD000 team control, 0x1FFD500 keep-items helper, 0x1FFD540 IQ helper, 0x1FFD600-0x1FFDB5C starter code.
- Scanlines filter: gaussian beams at output height, no mask; cloud shadows (3D) no longer stretched into margins outside dungeons
  (sr->WideExtend3D = ov29 loaded). TODO: real wide 3D (clip planes + raster width) for dungeon darkness + cloud shadows.
- v0.5.3: sea cutscene crash = "bad jump 02313104": jump table `cmp r0,#16; bgt; cmp r0,#0; addge pc,pc,r0,lsl#2` was sized
  from the nearest cmp (#0). Lifter now: addls -> nearest cmp; other conds -> cmp followed by b<gt|hi|ge|cs> gives the bound.
- v0.5.4: flight recorder. Frontend writes replay_last.rpl (prev run -> replay_previous.rpl): "SKYRPL1\0", u32 save size, save,
  u32 0, then 6 bytes/frame (keys u16, flags 1=touch 2=wide, x, y, enhancements mask). Touch now applied once per frame.
  Rig: REPLAY=file.rpl drives keys/touch/wide/sadie and loads the embedded save (frames arg 0 = whole recording).
  SetThreadStackGuarantee(128K) so stack overflows still get a crash report. Crash after forming the exploration team
  (new game, ~54000 frames) left no message at all -> likely stack overflow / hard kill; awaiting a recording.
- v0.5.5: PC-style quicksaves. F5 = quicksave, F9 = quickload; file <rom>_slotN.quicksave (melonDS savestate).
  Safe point: the lone CP15 wait-for-interrupt (lifter emits recomp_halt_pc = addr before it); while natively halted there,
  recomp_quicksave_prepare() packs flags + JumpTo(halt_pc+4) so the state resumes in the interpreter. Quickload:
  DoSavestate + recomp_quickload_reset() (retire/DeleteFiber all fibers, fresh fiber from loaded CPU state) + menu reset.
  Refused while Recomp Options menu is open. Tested in dungeon and town (save, move on, load, continue) in the rig.
  Note: interpreter-only melonDS is faster than hybrid in the rig (4.9s vs 8s / 1200 frames) — worth profiling later.
- v0.5.6: "hang" after the beach scene was the single-screen auto-switch: bottom faded first (top still lit) -> showed top;
  then top went black while bottom showed story text on black (also counts as "dark") -> stuck on black top screen while
  the game waited for A. Now both-dark counts as "bottom wins". (Also seen: wide margins ignore master-brightness fades.)

## v0.5.7 — picture screens get a blurred fill
- runtime/widefill.h: per screen, scores whether the margins belong to the picture. Row seam test: margin pixel next to
  the 4:3 edge differs a lot while the picture is smooth there. Flat test: outer 12 margin columns are one colour while
  the picture edge has detail. Score > 0.7 for 2 frames turns "picture" on; 8 good frames turn it off.
  Picture screens get the margins filled with an 8x-downsampled, 3-pass box-blurred copy zoomed to the full wide
  frame. It is dimmed to 0.7 when busy, undimmed when flat (white flashes, fades).
- Catches the boot intro artwork, Nintendo/ESRB screens, story text on black, and layer-based fades where the margins
  kept showing the un-faded scene.
- ground_refresh: looping maps narrower than the wide view (w*24 < 256+2W+24) no longer wrap into the extra columns.
  They are left blank, so the title-card logo stops repeating and the flat test triggers the blur.
  NOT verified in the rig: her.qs no longer advances the story here (it worked at 03:45).
- Rig: NOFILL=1 disables the fill in harness ppm output. wtools/wft runs the score/fill on ppm files.
- Still open: dungeon darkness and cloud shadows (needs a real wide 3D render), and rain in the storm cutscene
  (probably a 256-wide layer or sprites placed only in 0..255; not inspected yet).

## v0.5.8 / v0.5.9
- Filters: Smooth (sharp bilinear), Glow (scanlines + bloom, flat), DS Screen (sharp bilinear + faint pixel grid).
  FILTER_SMOOTH/GLOW/LCD; menu strings appended.
- v0.5.9: the picture detector is off while ov29 (dungeon) is loaded, because dungeon map edges are flat and that
  falsely triggered the blur. GPU_Soft margins skip 512-wide BG layers parked at scroll 0,0 (static overlays such as
  the dungeon minimap BG2), whose off-screen half held stray window boxes.

## v0.5.10
- Scanlines: beam table normalised to area 1, so average brightness matches Off (measured 57.2 vs 58.0). Previously
  a 1.25x gain over-brightened. Horizontal expansion is now done at the real picture width with sharp-bilinear edges
  (no GPU stretch). tex_hi is HI_W=4096 wide; FilterJob has dw.
- Glow: base*0.75 + 0.25 * gaussian blur (sigma 1.8 source px) of the whole picture. Energy-preserving halation instead of an additive bloom.
- Smooth/DS Screen also render at full picture width.

## v0.5.11
- Sadie scrapped the picture-screen detector/blur: removed from the frontend and harness, and the narrow-looping-map blanking in ground_refresh was reverted. runtime/widefill.h is kept but unused. The minimap overlay skip stays.

## v0.5.12
- Margins now render 256-wide text BGs on lines where their map row is a single repeated tile (full-screen tints such as the night dimming and some fades). Other 256-wide layers stay out.
- Outside dungeons the 3D edge pixel is carried into the margins when the 3D line is uniform (sampled at 0/64/128/192/255) and translucent.
- Verified: the Lapras intro fade now darkens the margins. Dungeon and town are unchanged. Some fades (white flash at b_02300, black at b_02000) still are not covered.

## v0.6.0-mockup
- Save gear placeholder: a procedural teal cog with a clock face (make_gear_texture). It spins in the bottom-right for about 3.25s whenever WriteNDSSave runs (Platform::g_save_writes), or on F6. It is drawn in real pixels (logical size cleared temporarily). Real autosave is still pending the Kangaskhan Rock trace.

## v0.6.1-mockup
- Recomp Options gains an "Autosave" entry (result 6) that opens an options window with Autosave On/Off. Stored as RecompSettings.autosave (default 1) and as autosave= in the ini. Verified in the rig from the dungeon menu.
- Rig note: in town (g.qs) X/Y do not open the menu in the harness, but in the dungeon (dun.mst) X works.

## v0.6.2 — wide 3D renderer
- GPU3D: X clip planes at +-W*(256+2M)/256 via s_clipNum/ClipLim<comp> in ClipSegment and ClipAgainstPlane (affects
  polygons and BoxTest). With WideMargin3D set, FinalPosition[0] is signed and unmasked (the same formula, no & 0x1FF).
  The first attempt squeezed Position[0] instead, which lost precision: a full-screen quad stopped at x=254.
- GPU3D_Soft: RenderPolygons does two extra passes with XShift = +256 / -256 (added to every FinalPosition[0] read),
  copying cols 256-M..255 / 0..M-1 into WideLines[2][192][64], then the normal pass. Cost is about 1% in the rig.
- GPU_Soft: WideMargin3D is set at line 0 from WideMargin. Margins use WideLines; where the wide render is empty and
  the real 3D line is a uniform translucent tint, the edge pixel fills (fades drawn as exact 4:3 quads).
- Backups: GPU3D*.prewide, GPU_Soft.cpp.prewide in /tmp/melonDS/src.
- Seen: at the dungeon's left map edge a black band plus stale tiles; this predates wide 3D (BG ring outside the map).

## v0.6.4: host stack leak (crash after Drowzee, Mt. Bristle crash)
- Both crashes were C00000FD stack overflows on a game fiber. The new gauge showed about 1.1 MB per 30s of growth in
  dungeons. Reproduced in the rig with her replay.
- Cause: the guest abandons frames and jumps to an outer level's return address with that level's SP (dungeon
  loop, 0x02003510 / 0x022E9FF8 pattern). The host run_interp/native frames in between never returned.
- Fix: each nested run_interp records {stop, sp, jmpbuf} per fiber (Fiber::frames). When the guest is in thread
  mode with SP above the innermost frame and lands on an outer frame's stop/sp, __builtin_longjmp unwinds the host
  there (stat_unwinds). Replay frames are pixel-identical; stack stays at about 3 KB.
- F5/F6/F9 are dev-only (#ifdef SKYRECOMP_DEV, DEV=1 build_win.sh).

## v0.6.5
- The v0.6.4 unwind used __builtin_setjmp/__builtin_longjmp. On Windows it looped forever at one guest pc (hundreds of millions of unwinds, zero interpreter steps): Win64 callee-saved registers were not restored. Switched to standard setjmp/longjmp (SEH-aware on mingw). Added a guard: 64 repeats at the same pc/sp with no guest progress turns unwinding off for the session and logs it.

## v0.6.6
- Fades drawn as a solid (alpha 31) full-screen 3D quad exactly 4:3 wide were skipped by the uniform-3D fallback, which only took translucent lines. It now takes any uniform 3D line with alpha > 0, filling only where the wide render is empty. Intro black and white flashes are full width now.

## v0.7.0
- Widescreen only: the toggle is gone from Graphics (3 rows now), F4 and the ini key are gone, and the default is 1.
- Portraits beside the text box: recomp_portrait_move() (native.c) reads WINDOW_LIST 0x22A88DC and finds a framed portrait box
  (update fn 0x202F70C, state 1-3) plus a dialogue box (0x202F488) on the same screen. GPU_Soft PortMove/PortCur is
  latched at line 0. On the source rows it renders once unmasked to capture (rect = window +-6 px), then renders
  with the window layers masked (BG0 3D frame/backdrop plus 256-wide text BGs) via SoftRenderer2D MaskX0/X1/MaskBits.
  On the destination rows it paints the capture left of the text box (dx = textbox_x - 6 - 2 - sw, clamped to -W-2),
  into the main line and the left margin after ExpandColor.
- v0.7.1: the text box moves too. Portrait + 2 px gap + text box are centred as a group across the wide screen (two MoveRects, two mask ranges).

## v0.7.2 — performance (Ally) + portrait halo
- Profile (callgrind, dungeon): about 60% was 2D drawing. Margin passes drew full 256-px lines for 43 kept pixels.
  SoftRenderer2D::DrawWidth (= W during margin passes) bounds the BG/compositing/sprite-interleave loops.
- The wide-3D margin passes are clipped to the kept strip (SoftRenderer3D ClipX0/ClipX1 in the span loops).
- The melonDS soft 3D render thread is on (RendererSettings.Threaded; NOTHREAD3D=1 turns it off).
- Margins are skipped for screens not on view (SoftRenderer::WideScreens, set by the frontend from layout/pip/show_top).
- Rig, 1200 dungeon frames on 2 cores: 11.1 s -> about 8.4 s, pixel-identical.
- Filters: per-column sharp-bilinear tables (smooth, crt expand); output identical.
- Portrait move: only pixels that change when the windows are masked are captured, so there is no scenery halo.

## v0.7.3
- Filter Off now uses sharp bilinear on the GPU: nearest-neighbour upscale into a render target at a whole-number factor K = ceil(out_h/vh) (capped at 4096), then a linear scale to the window. Every DS pixel is the same width (1080/192 = 5.625x had made 1-px minimap lines 5 or 6 px). Not testable in the Linux rig (SDL path).

## v0.7.4
- Darkness in the margins: the game sends its 2D-style 3D shapes already clipped to x = 0..256 (DBGPOLY showed no
  polygon ever reaches past the 4:3 edge). With WideMargin3D, a vertex of an untextured polygon that sits exactly
  on the viewport edge (0 or 256) is pushed out by +-M, so darkness and fades carry on across the margin. Textured
  shapes (HUD art) are left alone, because doing it for everything smeared the floor HUD bar.
- Enhancement 6 "Recruit from Anyone" (recruit_anyone):
  ApplyDamage 0x230A818/81C: is_team_leader(+7)==0 -> is_not_team_member(+6)==1 decides whether to skip RecruitCheck.
  RecruitCheck 0x230DD40 nop / 0x230DD58 b: no adjacency requirement. CanSeeTarget (0x230DD7C) is kept.
  Patched functions 2308FE0 and 230DBD0 fall back to the interpreter while the group is on.
