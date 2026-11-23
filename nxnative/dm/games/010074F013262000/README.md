<!-- game facts: made by docs/gen_tables.py from android.tsv; edit android.tsv, not this block -->
# Xenoblade Chronicles 3

![Xenoblade Chronicles 3](../../../../docs/images/xenoblade-chronicles-3.jpg)

| | |
|---|---|
| Status | **wip** |
| Title ID | `010074F013262000` |
| Tested with nx2android | `v0.1.0` |
| Tested build and device | v720896 OPPO PMA110 (Adreno 840) |
| App package | `org.nx2a.xc3` |

The profile sets these for you (`android.tsv`); a player changes the pause menu ones in the game, and the env rows in the data pack's `env.txt`:

| Row | What it does |
|---|---|
| `DM_NVN_ASYNC=1` | asynchronous replay: submissions and presents run on a replay thread, syncs wait for the job's replay and then its GPU fence |

Read the profile's porting log in the comment header of [`android.tsv`](android.tsv). Back to the [games list](../../../../README.md#games).
<!-- end of the game facts -->

## Game version

Updates 2.2.0 (`v655360`) and 2.2.1 (`v720896`) have identical main NSO bytes. The same docked 1920×1080 patch applies to both builds.

## Frame rate

- 30 fps.

## Tested devices

| Device | GPU | Vulkan driver | Game version | nx2android | Result |
|---|---|---|---|---|---|
| Lenovo Legion Y700 TB322FC (Snapdragon 8 Elite, Adreno 830, Android 16) | Adreno 830 | not recorded | 2.2.0 (v655360) | `v0.1.0` | wip |
| OPPO PMA110 | Adreno 840 | not recorded | 2.2.1 (v720896) | local build, 2026-11-13 | Simplified Chinese, four DLC mounted, imported save loaded in Colony Iota. |

In the same handheld 1280×720 view, 12-second samples showed 29.1 render / 29.2 display fps with frame generation off and 25.2 / 50.5 with Lossless. After switching to docked 1920×1080, the scene, characters and HUD drew correctly during a five-second camera pan. The Vulkan log reported stale texture aliases after the switch.

## Options

- Switch between handheld and docked in the pause menu.

## Quirks and known issues

- **work in progress**: some effects and models don't draw yet.
