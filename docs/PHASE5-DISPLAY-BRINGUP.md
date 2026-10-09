# Phase 5 plan: port-owned display bring-up (RPL-S UHD 770)

Upstream's takeover inherits a mode the firmware already lit. On the VM 113
guest nothing lights the IGD: the phase 4 probe found the display engine
unpowered (only PG0, no power wells, PLLs off, every DDI clock gated). The
host BIOS keeps the dGPU as primary display, so the firmware/GOP route is out.
The port therefore brings the display up itself, porting Linux i915's
sequences, and only then hands the lit mode to upstream's takeover path.

Reference: Linux i915 v7.2 (`~/work/linux-i915-v7.2`). Every register,
field and step below is taken from those sources, not from memory.

## Constraints

- One HDMI monitor on a motherboard output; host BIOS unchanged.
- Start with the mode the host's i915 picks for that monitor, ideally
  1920×1080@60 (148.5 MHz TMDS: no HDMI 2.0 scrambling above 340 MHz).
- Register writes are new code. They live in a separate component, build only
  with `REIMS_DISPLAY_BRINGUP=1`, and run only when the user loads that kext
  with a fresh snapshot. `ReimsTarget::kHardwareWritesPorted` stays false;
  it guards the unported takeover and RCS paths.
- Each stage is verified by readback against the host reference before the
  next stage is written. Any timeout aborts; nothing retries or forces state.
- Recovery: stop the VM; the hookscript returns the IGD to i915. If i915
  cannot reinitialise it, reboot the host.

## Stage 0: host reference (read-only, iGPU on the host)

With the macOS VM stopped and the HDMI monitor connected, the host's i915
should light that output by itself (fbdev). Then on the host:

    apt install intel-gpu-tools
    bash proxmox/capture-igpu-reference.sh

It saves to `/root/igpu-reference-<timestamp>/`: connector status, modes and
EDID; i915 debugfs (`i915_vbt`, `i915_display_info`, `i915_shared_dplls_info`,
`i915_power_domain_info`, `i915_cdclk_info`, `i915_ddb_info`, `i915_dmc_info`);
the decoded VBT; and `intel_reg read` dumps of power wells, CDCLK, DBUF,
DPLLs, DPCLKA, DDI buffers, transcoders, pipes and planes. This is the target
state for this exact board, monitor and mode. If the connector reports
`connected` but `disabled`, stop and decide how to get a lit reference.

## Stage 1: what the reference tells us

From the VBT and `i915_display_info`: which DDI the HDMI connector uses
(A or TC1–TC4), its DDC/GMBUS pin, the chosen mode, DPLL, CDCLK, DBUF
slices and plane buffer allocation. Decode the reference MMIO with
`display_timing.hpp` (it must report `OK` for that transcoder) and record
the expected values per stage.

### Captured 2026-10-09 (`~/work/igpu-notes/igpu-reference-20261009-140748/`)

Host i915 lit a 3840×2160 monitor (HDMI 2.0, also offers 1080p60 at
148.5 MHz and 4K30 at 297 MHz). `display_timing.hpp` decodes the dump to the
exact mode i915 reports; `test_display_timing.cpp` keeps it as a regression
vector.

VBT outputs (no port A on this board; all HDMI level shifter 6):

| VBT | Port / PHY | Type | DDC | AUX |
|---|---|---|---|---|
| HDMI-B | TC1 / B | HDMI (monitor here) | 2 | – |
| HDMI-C | TC2 / C | HDMI | 3 | – |
| DP-D | TC3 / D | DP/HDMI | 4 | AUX-D |
| DP-E | TC4 / E | DP/HDMI | 5 | AUX-E |

Target state, guest idle (phase 4 probe) → host lit:

| Register | Guest | Host | Meaning |
|---|---|---|---|
| `FUSE_STATUS` `0x42000` | `0x88000000` | `0x8f000000` | PG0 → PG0–3 distributed |
| `PWR_WELL_CTL2` `0x45404` | `0` | `0x3f` | driver: power wells idx 0–2 requested and on |
| `PWR_WELL_CTL_DDI2` `0x45454` | `0` | `0xc0` | DDI IO power idx 3 (TC1) |
| `CDCLK_CTL` `0x46000` | – | `0x00380264` | CDCLK 307.2 MHz |
| `BXT_DE_PLL_ENABLE` `0x46070` | – | `0xc0000010` | CDCLK PLL on and locked |
| `DBUF_CTL_S` `0x45008`/`0x44fe8` | – | `0xc040c000` both | both DBUF slices on |
| `DPLL0_ENABLE` `0x46010` | `0` | `0xcc000000` | enabled, locked, powered |
| `DPLL0_CFGCR0/1` | stale | `0x001001d0`/`0x448` | 594 MHz (DCO 8910 MHz, p 3) |
| `DPCLKA_CFGCR0` `0x164280` | `0x01e07c00` | `0x01e07400` | PHY B clock on, from DPLL0 |
| `DDI_BUF_CTL` TC1 `0x64300` | `0` | `0x80000000` | DDI buffer on |
| `TRANS_CLK_SEL` A `0x46140` | `0` | `0x40000000` | transcoder A ← port TC1 |
| `TRANS_DDI_FUNC_CTL` A `0x60400` | `0` | `0xa0030011` | on, TC1, HDMI, 8 bpc, +h+v, scrambling + high TMDS |
| `TRANSCONF` A `0x70008` | `0` | `0xc0000000` | transcoder on |
| `PLANE_CTL_1` A `0x70180` | `0` | `0x84000000` | plane on, XRGB8888, linear |
| `PLANE_STRIDE/SIZE/SURF_1` A | `0` | `0xf0`/`0x086f0eff`/`0x00aa4000` | 15360 B stride, 3840×2160, GGTT offset |
| `PLANE_WM_1`/`WM_TRANS` A | – | `0x8000401f`/`0x8000002d` | watermarks |
| `PLANE_BUF_CFG_1` A `0x7027c` | – | `0x07ba0000` | DBUF blocks 0–1978 (`i915_ddb_info`) |

DMC firmware `i915/adls_dmc_ver2_01.bin` is loaded on the host; the port keeps
DC states disabled instead. The first guest mode should be 1080p60 or 4K30
(≤ 340 MHz, no scrambling/SCDC); 4K60 needs HDMI 2.0 scrambling over SCDC.

## Stage 2: display core init (`icl_display_core_init`)

Order as in `intel_display_power.c`:

1. `gen9_set_dc_state(DC_STATE_DISABLE)`; keep DC states off (no DMC firmware).
2. Wa_14011294188 (adl-s): `SOUTH_DSPCLK_GATE_D` |= `PCH_DPMGUNIT_CLOCK_GATE_DISABLE`.
3. `intel_pch_reset_handshake`.
4. `intel_combo_phy_init` → `icl_combo_phys_init` (`intel_combo_phy.c`).
5. Power well 1 (`hsw_power_well_enable`, `HSW_PWR_WELL_CTL2` request,
   wait for state and `SKL_FUSE_PG_DIST_STATUS(1)`).
6. CDCLK (`intel_cdclk_init_hw`): CDCLK PLL `BXT_DE_PLL_ENABLE` `0x46070`,
   `CDCLK_CTL` `0x46000`, to the reference's value.
7. `gen12_dbuf_slices_config`, `gen9_dbuf_enable` (`DBUF_CTL_S` `0x45008`,
   `0x44fe8`), `icl_mbus_init`, `tgl_bw_buddy_init`.
8. Wa_14011508470: `GEN11_CHICKEN_DCPR_2` bits.

Check: probe readback of power wells, fuse status, CDCLK and DBUF matches
the reference.

### Implementation (`display-bringup/`)

`core_init.hpp` runs the sequence above over a templated register interface;
`test_core_init.cpp` (in `scripts/test.sh`) drives it against a register model
that starts from the guest idle state the phase 4 probe measured and ends at
the host reference (`CDCLK_CTL` `0x00380264`, `BXT_DE_PLL_ENABLE`
`0xc0000010`). It covers dry run, idempotence and every timeout or refusal.
`bringup.cpp` wraps it in `ReimsDisplayBringup.kext`:

- Builds only with `REIMS_DISPLAY_BRINGUP=1 REIMS_TARGET=rpls`
  (`display-bringup/build.sh`); the header refuses to compile otherwise.
- Default build is a dry run: its write function only counts (published as
  `ReimsBringupBlockedWrites`, must be 0) and the log lists every planned
  write. `REIMS_BRINGUP_EXECUTE=1` builds the writing variant.
- Refuses unless the IGD is at `kPCIPath` with the exact identity, registry
  `device-id` `ffff0000`, only `IONDRVFramebuffer` attached, and PCI memory
  decoding already on. Preconditions: 38.4 MHz reference, PG0 fuses, DC
  states off.
- Every step skips when the hardware already holds its target value; the
  first timeout or unexpected readback stops the run. Result and log:
  `display-bringup/read_bringup.py`.

Checked against the second host capture
(`~/work/igpu-notes/igpu-reference-20261009-143055/`): all 55 stage 2 targets
match host i915's live values, including the combo PHY A–E procmon writes for
the measured 0.85 V dot0 process code (`COMP_DW3` `0xc0606321` on masters A/D,
`0xc0608021` elsewhere), IREFGEN on A and D only, MBUS credits, and the
DCPR and PCH workarounds. Host i915 also disables the BW buddy
(`BW_BUDDY_CTL(1/2)` = `0x80000000`), the same path the port takes.

Deliberate differences from i915:

- DC states are only verified off (`gen9_set_dc_state` is not ported; no DMC).
- CDCLK goes straight to the reference's 307.2 MHz (voltage level 0) instead
  of the 172.8 MHz minimum followed by a modeset raise.
- BW buddy takes i915's unknown-memory path (`BW_BUDDY_DISABLE`), since the
  guest cannot read DRAM type; host i915 does the same on this machine.
- Unknown combo PHY voltage/process codes and any pcode error status abort,
  where i915 warns and continues.
- DBUF slice S2 stays off, as in i915's core init; the modeset enables it.

## Stage 3: one HDMI output (`hsw_crtc_enable` order)

1. Power well 2 if the pipe is not A, and the DDI IO power well for the port.
2. DPLL: compute with `icl_calc_wrpll` / `icl_calc_dpll_state` (including
   WA #22010492432 at the 38.4 MHz reference measured in phase 4), enable
   with `combo_pll_enable` (`icl_pll_power_enable`, CFGCR0/1, `icl_pll_enable`).
   Prefer the reference's CFGCR values for the same mode.
3. `intel_ddi_pre_enable_hdmi`: DDI clock (`adls_ddi_enable_clock`, `DPCLKA`),
   DDI IO power, `intel_ddi_enable_transcoder_clock` (`TRANS_CLK_SEL`).
4. Signal levels: `icl_combo_phy_set_signal_levels` /
   `icl_ddi_combo_vswing_program` with `icl_combo_phy_trans_hdmi`, the
   table `adls_get_combo_buf_trans` selects for HDMI (`intel_ddi_buf_trans.c`),
   at the VBT's level.
5. Pipe: `intel_set_transcoder_timings`, `intel_set_pipe_src_size`,
   `bdw_set_pipe_misc`, `hsw_configure_cpu_transcoder`
   (`TRANS_DDI_FUNC_CTL` HDMI mode, bpc, sync), `hsw_set_transconf`.
6. Watermarks and DBUF: `PLANE_BUF_CFG_1` `0x7027c`, `PLANE_WM_1` `0x70240`
   (NootedGreen reports zero DBUF allocation gives fragmented output).
7. Enable DDI (`intel_ddi_enable_hdmi`: `DDI_BUF_CTL`).
8. Plane 1 on a test pattern: GGTT entries for a guest framebuffer (BAR0
   upper half), then `PLANE_CTL`/`STRIDE`/`SIZE`/`SURF`.

Check after each step: probe readback against the reference;
`display_timing.hpp` decodes the transcoder as `OK` with the reference mode;
then the monitor shows the pattern.

## Stage 4: handoff to upstream

Once the port lights the output, upstream's flow applies: the native
framebuffer decodes the running mode and takes over the plane. That part
(remove DPT handling, generalise 1920×1080 constants, rpls takeover behind
`kHardwareWritesPorted`) is the original phase 5 work item.

## Unknowns

- Whether host i915 lights the HDMI output with the dGPU primary (stage 0).
- GMBUS EDID reads in the guest (stage 3 can start from the reference mode).
- How Apple's TGL accelerator reacts to a port-initialised display versus a
  firmware one (phase 6).
