# Co-op by driving an AI ally: feasibility (2026-10-18)

Question: can a second player take control of one of Cole's AI allies (Zeke, Kuo,
Nix) on the shared screen, with the friend's input arriving as pad port 1
(Sunshine/Moonlight on the host)?

Static analysis only (EBOOT BCUS98125 01.00, the index from `rpi/game-re`). Nothing
was run. **H** marks a hypothesis; section 7 lists the traces that confirm or kill
each one. Addresses are guest EAs; `who.py <addr>` shows the evidence.

## 0. Verdict

**Feasible as a limited prototype; not feasible as "the friend plays a second Cole-like
character" without much more work.**

- The input side is better than expected. The engine (Sly Cooper lineage) supports
  several joysticks, and input goes **per object**, not through a global. The game
  initialises **2 pads**, polls **port 1 every frame** into its own joystick object,
  and every playable object (`CPo`) has a script field `joyid`. Nothing in the
  shipped game seems to bind joystick 1 (H, run 1 checks).
- Cole and the allies share the character base `CStep` (move orders, HP, skill
  manager), but their **brains are different**: Cole's root skill `CHeroGunSkill`
  reads the stick and runs the hero power system; an ally's root skill `CNpcSkill`
  runs a strategy and tactics with no stick input. A human brain for an ally means
  **issuing AI orders from pad 1** (run to here, use attack X on target Y), not
  putting the ally on Cole's input path. The engine gives us the hooks for that:
  the per-NPC update is virtual, move orders are one function, and the ally
  attacks have script-facing `FORCE_*` entry points (they exist so missions can
  stage the ally's moves).
- What it will feel like: the friend steers a character that walks or runs where the
  stick points and fires its signature attacks on a button, with NPC animation and
  NPC turning. There is no jumping, parkour or free aiming unless the NPC rig has them.
- The hard limits are outside the input code: allies exist only where mission data
  spawns them (their assets stream per mission), the camera follows the joystick-0
  player only, and mission scripts drive the allies too.

## 1. Pad → player → actions

### 1.1 Pad layer (fully read)

| What | Address | Evidence |
|---|---|---|
| Pad init | 0x18CB44 | `cellPadInit(2)`; `cellPadSetPortSetting(port, 6)` for ports 0 and 1; then `JoyInit(J0, 0)` and `JoyInit(J1, 1)` (0x18C8B4, stores the port at +0x20) |
| Per-frame input update | 0x18C2D8 (from 0x1DB544) | `cellPadGetInfo2`, checks the system-intercept bit, then `JoyUpdate(J0)` and `JoyUpdate(J1)` (0x18B724 twice) |
| Joystick update | 0x18B724 | `cellPadGetData(*(J+0x20), buf)`; builds button bits through the mapping table at 0xA06610/0xA06614 |
| Joystick objects | **J0 = 0xA058D0, J1 = 0xA05F60** (stride 0x690), **null joystick 0xA065F0** (index 2, never polled) | `J(n) = 0xA058D0 + n*0x690` in `CPo::get_VIEW` 0x28FFD0, `CView::set_joyid` 0x43E250, `CPo::vf79` 0x290054 |

Joystick fields used so far: +0x1C button bits (this frame), +0xF4 previous bits,
+0x20 port, +0x24 present flag, +0x600 current input focus (a binding),
+0x610 binding slots (7 priorities, scanned from the top down by 0x18A11C),
+0x64C a hands-off counter (0x189744, 0x18A3FC, 0x18B364: they take a
joyid, 0 → J0, 1 → J1, anything else → both), +0x67C view.

**Does the game ever read port 1? Yes.** J1 is initialised on port 1 and updated every
frame. Whether anything consumes it is the open part: no shipped code path writes
`joyid = 1` that I could find (H1). The port's host side already maps it:
`libs/input/cellPad.c` (SDL2 backend on Linux) gives host controller *i* to port *i*,
`cellPadInit(2)` makes ports 0 and 1 valid, and `cellPadGetInfo2` reports each one's
status. A second SDL game controller (the Moonlight virtual pad Sunshine creates)
**becomes port 1 without code changes**. Caveat: ports follow SDL device order, so
the host's own pad and the Sunshine pad can swap (on reconnect, or with an extra
non-pad joystick device present). Pinning a port by GUID or name through an env
variable is a small `cellPad.c` change and should come before any real session.

### 1.2 Joystick → object (input focus)

- Every `CPo` (id 7, size 0x320; parent of `CStep`, `CHero`, `CNpc`) embeds a
  **binding** at +0x2B0 `{vptr 0x8DEE28, +4 joystick*}` (built by 0x189A04 in
  `CPo::CPo` 0x290110). +0x4 starts at the null joystick 0xA065F0.
- `CPo.joyid` is a script field at **+0x2BC** (type 0x1007; the ctor sets −1).
  **`CPo::vf79` 0x290054 = SetJoyId(n)**: it takes the binding off the old joystick's
  slot 0 and puts it in slot 0 of `J(n)` (0x18A1C0 → 0x18A11C). Whichever binding
  becomes the joystick's focus (+0x600) gets that joystick in its +4. The previous
  focus is handed the null joystick. Menus and the radial menu use higher slots, so
  they take input away from the hero while open.
- So **`CPo+0x2B4` is "the joystick this object listens to right now"**: J0, J1, or
  the null joystick (no input).
- `CPo.PLAYABLE` (get 0x28F8EC / set 0x28F92C): an intrusive list of playable
  objects, head at 0xA112B0, link at +0x2B8.
- `CPo.VIEW` (0x28FFD0): `J(joyid)+0x67C`; if that is null, the main view at
  `*(0xA32EB8)`. Stick input is turned into world space with this view.
- **"The hero" is found through joystick 0**, not through a global:
  `PlayableForJoy(n)` 0x28FC60 (`J(n)` slot 0 → binding vf0 → owner) has 28
  callers. Most pass a literal 0 (`CReactToHeroTactic::vf5`, `CControlPrompt::vf80`,
  0x28A380…), and cameras pass their own joyid (`CCmManual::vf44`, field +0x120).
  None of the 109 `CHero` functions writes a global. A second object on joystick 1
  therefore stays invisible to every "where is the hero" query, which is what we
  want for an ally.

### 1.3 Who reads the stick

Two kinds of reader:

1. **Generic skills that read `root->+0x2B4` and `CPo::get_VIEW`.** `root` is the
   skill's `ROOT_OBJECT` at skill+0x1C. Examples: `CBalancedRunSkill` (input
   0x47244, `INPUT_SCALE` +0xE8), `CAttackSkill` (0x3F350), `CJumpSkill`
   (0x18EB2C/0x193930 region), `CEdgeSkill`/`CPoleSkill`/`CBarSkill`/
   `CBarHangSkill`/`CSconceSkill` (shared 0x224764), `CWallSkill` (0x2511AC),
   `CFendGroundSkill::vf8`, `CHeroDashSkill::vf5` 0x1551E4. They all go through
   `JoyStickInView(joy, view)` 0x18A824 (25 callers). 0x47244 touches only
   CPo-level state and animation calls, so these skills **are not tied to CHero by
   type** (H2: some may read CHero fields elsewhere).
2. **The hero's root skill `CHeroGunSkill`** (id 383, size 0x1170, vtable 0x8DE430,
   update vf5 0x172308). It holds `HERO_POWERS_MANAGER` (+0xC00), maps buttons to
   powers (`BTNK_FROM_POWERK`, `LPOWERK_FROM_BTNK`, `TRIGGER_POWER`,
   `FIRE_CURRENT_POWER`) and debits magic on `CHero` (`CURRENT_MAGIC_POINTS`
   +0x1F38). It is a hero-only skill and cannot be put on an NPC: `CHero` is
   0x2BC0 bytes, `CNpc` 0x2070, and the hero fields sit where the NPC has
   different data.

Chain for Cole: `cellPadGetData(0)` → J0 (0xA058D0) → focus binding = Cole+0x2B0 →
Cole+0x2B4 = J0 → `CHeroGunSkill` and the generic movement skills on his skill
tree read J0 through `ROOT_OBJECT` and `get_VIEW` → `CStep` locomotion and
animation.

## 2. Allies and their brains

| Class | id | Role (H3: which character) |
|---|---|---|
| `CNpc` | 9 (size 0x2070, ctor 0x1E0900, Create 0x6BD01C, vtable 0x8E0908) | every ally is a CNpc (`IS_STORY_CHARACTER`, `IS_CONDUIT` +0x1F58) |
| `CNpcSkill` | 397 (size 0x820, vtable 0x8E0C70, **update vf5 0x1FA6D0**) | NPC root skill: orders, strategy, guns, attacks |
| `CAngelCustomStrategy` | 437 (vtable 0x8D4F98) | Kuo (ice, the "good" choice); `IGNORE_HERO` +0x1050 |
| `CAngelIceAttackTactic` | 486 (Create 0x6AA458, vtable 0x8D7A58) | Kuo's ice attacks: `FORCE_ATTACK` 0x4E5AF0 → 0x6C87C, `END_FORCED_ATTACK` 0x4D903C, `ENABLE_ATTACK` 0x4E5B40, target +0x5E8, `ASSIST_SOURCE_POINT` +0x670 / `ASSIST_TARGET_POINT` +0x680 |
| `CDevilStrategy` | 439 (vtable 0x8D9D60) | Nix (fire, the "evil" choice): `FORCE_GROUND_VINE` 0x4D9280 → 0xB8FB4 (takes a position), `FORCE_FIREBIRD` 0x4E085C → 0xBDA60(strat, target), `FORCE_MAGIC` 0x4E080C → 0xBD908, `FORCE_FETCH_PED` 0x4E07E4 → 0xBA238, `FORCE_TELEPORT_STOMP` 0x4E07BC → 0xBD9C4, `END_ALL_FORCED_ATTACKS` 0x4E0798 → 0xBD8C0, `IGNORE_HERO` +0x130 |
| `CDevilMoveTactic`, `CTeleportStompTactic`, `CFirebirdTactic`, `CGroundVineTactic`, `CFetchPedTactic` | 485, 483, 507, 481, 482 | Nix's moves as tactics |
| `CBuddyAttackTactic` | 487 (Create 0x6AA404, vtable 0x8D79D0) | Zeke (gun): `FORCE_ATTACK` 0x4E131C → 0x6EBD4, `END_FORCED_ATTACK` 0x4E12F8, `SET_ATTACK_RANGE_HERO`, target +0x5D0 |
| `CFollowHeroTactic` | 484 (Create 0x6AA500) | keeps an ally near Cole: `TELEPORT_TARGET`, `CAN_RESTART_TELEPORT` +0x4C, `HERO_PREDICTION_TIME` +0x54 |
| `CCustomStrategy` / `CCustomTactic` / `CBaseScriptTactic` | 436 / 474 / 450 | script-defined brains (`UPDATE_FUNCTION` +0x7C, `TRIGGER_FUNCTION` …) |

**The ally brain is a stack.** `CNpc.STRATEGY` (+0x1FC8, setter 0x1DE128) holds a
`CStrategy` (`FIND_TACTIC` 0x4E9AA4, `ENABLE_TACTIC` 0x4E3334, `ENABLE_ALL_TACTICS`
0x4E330C). The strategy picks among `CTactic`s by `PRIORITY` (+0x28). The active
tactic acts through the NPC's root skill `CNpcSkill` (orders: `SET_IDLE_ORDER`,
`SET_GUARD_ORDER`, `CANCEL_ORDER` 0x4DDF08 → 0x1F4144, `ENEMY_TARGET` 0x1EF8A0)
and through **`CStep` order verbs that Cole uses too** (cutscenes move him with
them): `RUN_TO` 0x4DEC5C → 0x2FBED0 → `Step_MoveOrder(step, kind 11, target,
pos, slack, …)` 0x2FBD24, `MOVE_TO` 0x4DEDCC → 0x2FBF30, `WALK_TO`, `JUMP_TO`
0x4DEBC8, `TELEPORT` 0x4DEA70, `SHOOT_AT` 0x4DE568 → 0x1DD5D0, `Melee` 0x4DA534.

Script method ABI (from those wrappers): r3 = object, r4 = argv (16-byte slots:
object at +0, vector at +0x10, float at +0x20, bool at +0x24, word at +0x28), and
r5 = a bitmask of which optional arguments were passed. Each wrapper unpacks argv
and calls a native function, and the natives are the ones listed above.

**Do both go through a common character-controller layer?** Partly. The common
layer is `CStep` + `CSkillManager` (`SET_ROOT_SKILL` 0x4DB6A4 → 0x3421F8) + the move
order builder 0x2FBD24 + the per-object joystick binding. Above that, Cole's
"brain" is a skill that reads the joystick and an ally's brain is a skill that
reads the strategy. So the right plan is **a human-input brain that issues the
same orders the tactics issue**, not routing an ally through Cole's skills.

## 3. Spawning outside missions

- Every Create (`CNpc::Create` 0x6BD01C, the tactic Creates, `CHero::Create`
  0x6BCEE4, `CHeroGunSkill::Create` 0x6BE264) has **0 direct callers**: objects
  are built from data through the class registry. The strategies have no Create
  of their own (H: built by `CNpcSkill.ENSURE_STRATEGY` 0x4E9B34 from data).
- Characters arrive through proxies and mission locations:
  `CProxySite.SPAWN`/`UNSPAWN_ALL`/`DEFER_UNSPAWN_ALL`,
  `CProxySiteConditional.FORCE_SPAWN`, `CMissionLoc.SPAWN_PROXY` and
  `NO_DEFERRED_UNSPAWN` (+0x2B4), `CSystemicCharacterLoc` (id 230). Their assets
  stream per mission (`CMission.ARE_DESIRED_CHARACTERS_LOADED` /
  `WAIT_DESIRED_CHARACTERS_LOADED`).
- The skill trees, strategies and tunables (follow distance, teleport, HP) live in
  the mission and character scripts inside `core.psarc`, which nobody has unpacked
  yet. **Not answerable from the EBOOT.**
- H4: UGC (`CUgcWorld`, `CEditNpcTool`, `SET_MAP_ICON_FOR_CLASS`) may be able to
  place story characters, which would be an "ally in free roam" route without
  touching mission data. Unverified.

Practical reading: **co-op works first inside missions where the ally is already
present.** Keeping an ally in free roam needs its proxy spawned and its character
assets kept resident. That is data RE, not EBOOT RE.

## 4. Camera and range

- Cameras follow a view. `CCmStandard::vf44` 0x7E7F0 takes its view at cam+0x108
  and the view's subject (0x43DDB8 → view+0x89B8), plus a joyid at cam+0x120.
  Views are per joystick (J+0x67C; `CView.joyid` +0x30C). The engine could in
  principle do a second view per joystick (split screen), but that means a second
  full scene render. Not studied, and out of scope on cost.
- **Something to keep both on screen exists:** `CCameraVolume.SECONDARY_FOCUS`
  (+0x194, setter 0x6B6A24), `USE_SECONDARY_FOCUS_TILT` (+0x1C0),
  `SECONDARY_FOCUS_OFFSET` (+0x1D0), `SECONDARY_SPEED_FUNC`/`LIMITS`. The update
  is `CCmStandard::vf44` (it uses that field) and `CCmSnipe`. H5: setting the
  active camera volume's secondary focus to the ally makes the standard camera
  frame Cole and the ally together. Cole's `PUSH_POI`/`POI_CAMERA_VOLUME` is
  another lever.
- Leash: `CFollowHeroTactic` teleports the ally back (`TELEPORT_TARGET`,
  `CAN_RESTART_TELEPORT`, `CTeleportSkill` with `MAP_DISTANCE_TO_TRAVEL_TIME`
  +0xA4). Its distances are data tunables, not in the EBOOT. With that tactic off
  (`ENABLE_TACTIC`), nothing in the code read so far pulls the ally back. What
  will stop it is streaming: world and collision load around the camera, and
  mission-loc proxies unspawn. The co-op mod should enforce its own leash (for
  example, disable the move order beyond N m from Cole, or teleport with `CStep.TELEPORT`).

## 5. Health, death, powers

- Both use the same `CStep` HP block: `MAX_HIT_POINTS` +0x7BC,
  `CURRENT_HIT_POINTS` +0x7C0 (setter 0x6ABD84), `ADJUST_HIT_POINTS`,
  `WOUNDED_HIT_POINT_LIMITS` +0x7D0, `LAST_DAMAGE_RECORD` +0x1E4C,
  `DIE_IMMEDIATE` 0x4DAE6C → 0x3454E8, `INJURE`/`WOUND`/`HEAL_INJURY`,
  `SHOULD_BE_DEAD`… They share the damage pipeline `CDamageRec` (id 510:
  `source_object` +0x64, `target_object` +0x6C, `powerk` +0x88, `xpmonid` +0x8C,
  `INFLICT_DAMAGE` +0x78).
- NPC only: `CNpc.IS_DEAD` 0x1DC2F4, `IS_WOUNDED`, `IS_DEFEATED`, `WOUNDABLE`,
  `KILLABLE_BY_NPC` (CNpcSkill +0x7D9), `HERO_DAMAGE_INSTANT_DEATH` +0x1F5A.
  Hero only: HP regen (`PUSH/POP_HP_REGEN_DISABLE`), `DIE`, `INCAPACITATED`
  +0x1EFD. `CHealOtherSkill` (id 399) heals others (H: Cole reviving).
  Ally death and respawn rules are mission script (H6: allies are kept from dying
  by script or `WOUNDABLE`; trace run 2).
- Powers: **different systems.** Cole uses `CHeroGunSkill` +
  `CHeroPowersManager` (powerk, btnk, magic points on CHero). Allies use NPC
  tactics (above), plus `CNpc.POWERS` +0x1FE4, `ADD_POWER` 0x4DE90C and
  `REMOVE_POWER`. Damage is common, so ally kills already count in fights.

## 6. Minimal prototype plan

Hook mechanism: `work/tools/ppu_native_entries.py` + `port/ppu_native.cpp` (a
`PPU_NATIVE_HOOK` at the entry of a lifted function. It also catches direct `bl`
calls, and returning 0 declines so the lifted body still runs). This makes a
**pre-hook** at no cost: do our work, return 0.

1. **Input (host).** Pin pad 1 to the Sunshine controller (cellPad.c env var,
   section 1.1). The brain can read J1 (0xA05F60: +0x1C buttons, sticks as
   0x18B724 stores them; H: the offsets need run 1) or read port 1 straight from
   the host pad state in C. The second is simpler and does not depend on J1
   being idle.
2. **Select the ally.** Native pre-hook on **`CNpcSkill::vf5` 0x1FA6D0** (every
   NPC, every frame). Take `root = *(skill+0x1C)`, `strat = *(root+0x1FC8)`, and
   tell allies apart by `*(strat)` = 0x8D9D60 (devil), 0x8D4F98 (angel), or by
   `FIND_TACTIC` returning a `CBuddyAttackTactic` (Zeke). Keep the pointer while the
   NPC exists (clear it in its destroy/leave-world).
3. **Take the brain.** On "co-op on" (a pad-1 button combination), call
   `CStrategy` `ENABLE_ALL_TACTICS(false)` (0x4E330C wrapper; native 0x36F1CC)
   or disable only `CFollowHeroTactic` and the attack tactics. Then
   `CANCEL_ORDER`.
4. **Move.** About every 100–200 ms while the stick is deflected: dir = stick
   rotated by the main view (`*(0xA32EB8)`, or `CPo::get_VIEW(ally)`, which
   falls back to it); call **`Step_MoveOrder` through `RUN_TO`'s native 0x2FBED0**
   (ally, target 0, pos = ally pos + dir·k, slack). Walk or run picked by stick
   magnitude (`WALK_TO`/`RUN_TO`). Stick released: `CANCEL_ORDER` or an idle order.
5. **Act.** Buttons → Nix: `0xBDA60` firebird, `0xB8FB4` ground vine (position),
   `0xBD9C4` teleport stomp, `0xBD908` magic. Kuo: `0x6C87C`. Zeke: `0x6EBD4`.
   Target = `CNpcSkill.ENEMY_TARGET` (0x1EF8A0) or the nearest enemy in the stick
   direction. Release → the matching `END_*`.
6. **Camera and leash.** Set `SECONDARY_FOCUS` on the active camera volume to the
   ally (H5). Clamp the move target to N m from Cole, and teleport the ally back
   past M m.
7. Calling guest code from native: functions with an .opd (the script wrappers)
   through `harness_guest_caller`. The direct-only natives (0x2FBED0, 0xBDA60…) by
   filling `ppu_context` and calling `func_<addr>` (vector argument in v2, as
   `RUN_TO` does).

Cheap side spike (B): give the ally `joyid = 1` (`CPo::vf79` 0x290054, slot 0 of
J1) and push a stick-driven generic skill (`CBalancedRunSkill`, Create 0x6BE300)
as its root. If the NPC rig has the animations, the friend gets real analog
locomotion through the engine's own path. Most likely the skill's animation fields
(`RUN_ANIMATION` +0x14, `IDLE_ANIMATION` +0x10…) come from script data we don't
have yet. One day to try once the hook is in.

**Effort (rough):** traces 1–2 game sessions; movement-only prototype 3–5 days;
attacks 3–5 days; camera, leash and death handling 1–2 weeks. That is about
2–3 weeks to "playable in missions that have an ally". Free-roam ally is unknown
(core.psarc data RE), weeks or a no-go.

**No-go if:**
- a tactic or mission script re-issues orders every frame and turning the
  tactics off leaves the ally broken (frozen, or snapped back by the strategy):
  orders would fight;
- the `FORCE_*` natives need state only the script VM sets up (they crash or do
  nothing when called from native);
- the allies are present only in scripted set-pieces whose script drives
  them the whole time, so there is no window of free control;
- ally assets cannot stay resident outside their missions (free-roam co-op
  only; prototype still fine inside missions);
- pad 1 already has a consumer in the game (H1 false): then the brain must read the
  host pad directly and J1 must stay unbound.

## 7. Traces to run (ready to paste)

Same setup as `rpi/game-re` section 5. All hooks below are indirect-call targets
(0 direct callers, checked), except `CPo::vf79`, `set_PLAYABLE`,
`CView::set_joyid` and `set_STRATEGY`, which have 1–2 direct callers too.

**Run 1: input and bindings (free roam, second pad connected on port 1).**

```
R=$PWD/work/runs/re
export PS3RECOMP_RE_NAMES=$R/names.txt PS3RECOMP_RE_LOG=$R/coop-1.log PS3RECOMP_RE_DUMP=0x40
export PS3RECOMP_RE_TRACE='0x290054,0x28F92C,0x43E250,0x6BCEE4,0x6BE264,0x6BE300,0x4DB6A4'
export PS3RECOMP_RE_WATCH='0xA058F0:8,0xA05F80:8,0xA058EC:4,0xA05F7C:4,0xA05ED0:4,0xA06560:4,0xA05ED0>0x510:4,0xA065DC:4'
work/play.sh
```

Watches: J0/J1 port + present (+0x20:8), J0/J1 button bits (+0x1C), J0/J1 input
focus (+0x600), Cole's `CURRENT_HIT_POINTS` through J0's focus (binding +0x510 =
CPo+0x7C0), J1's view (+0x67C).
In game: load the save, press buttons on pad 1 only (note times), open and close
the pause and power menus on pad 0, take a hit.

| H | Confirmed when | Killed when |
|---|---|---|
| H1 pad 1 unused | J1 +0x1C changes with pad-1 presses, J1 +0x600 stays 0, the game ignores pad 1 | J1 +0x600 non-zero, or pad 1 does something |
| joyid binding | `CPo::vf79` logs r4=0 on Cole when the hero is set up; J0 +0x600 = Cole+0x2B0 | Cole gets input another way (J0 +0x600 stays 0) |
| hero root skill | `CHeroGunSkill::Create` and `SET_ROOT_SKILL` at load | — |

**Run 2: an ally mission (Kuo or Nix; Zeke if easier). Fight with the ally around.**

```
export PS3RECOMP_RE_LOG=$R/coop-2.log PS3RECOMP_RE_DUMP=0x40 PS3RECOMP_RE_MAX=400
export PS3RECOMP_RE_TRACE='0x6BD01C,0x1DE128,0x6AA404,0x6AA458,0x6AA4AC,0x6AA500,0x6A9D74,0x6AA554,0x4E085C,0x4E080C,0x4D9280,0x4E07BC,0x4E5AF0,0x4E131C,0x4E330C,0x4E3334,0x4DEC5C,0x4DEDCC,0x4DE568,0xB87E4,0x13E8C,0x147FC,0x69DA8,0x688BC,0x6B6A24,0x4DAE6C,0x6ABD84'
work/play.sh
```

| H | Confirmed when | Killed when |
|---|---|---|
| H3 ally = class | `set_STRATEGY` on the ally's CNpc with a strategy whose vptr is 0x8D9D60 / 0x8D4F98; the matching tactic Creates | other strategy classes for the allies |
| strategy update slot | one of 0xB87E4 / 0x13E8C / 0x147FC / 0x69DA8 / 0x688BC fires every frame (the RE_MAX cap is hit fast) | none frequent: the update is a non-virtual call, so hook `CNpcSkill::vf5` instead |
| scripts force ally attacks | `FORCE_*` calls at set-pieces (r3 = strategy or tactic, argv dump gives the target) | never called: the `FORCE_*` path is untested in shipping, which is riskier |
| orders through RUN_TO | `RUN_TO`/`MOVE_TO` from script on the ally | allies move only through tactics (then copy what the tactic calls, found from 0x2FBD24 callers) |
| H5 secondary focus in use | `set_SECONDARY_FOCUS` at boss or ally beats | never: the field may be unused, so test it by writing it |
| H6 ally death | `set_CURRENT_HIT_POINTS`/`DIE_IMMEDIATE` on the ally, and what follows | — |

After run 2, the ally's CNpc address from the `set_STRATEGY` line gives the fields
to watch in a third run (orders, HP at +0x7C0, strategy at +0x1FC8). Allocations
can move between runs, so take the address again each run.

## 8. Not done / next

- Mission and character scripts in `core.psarc` (ally skill trees, tunables, how
  missions spawn and drive allies). This is the biggest unknown and decides
  free-roam co-op.
- The `Step_MoveOrder` 0x2FBD24 argument list in full (kinds other than 11, flags).
- `JoyStickInView` 0x18A824 output and the stick fields of the joystick object,
  needed if the brain reads J1 rather than the host pad.
- A second `CHero` on joystick 1 (the plumbing supports it: per-object joyid,
  input focus, per-joystick views, the hero looked up by joystick 0). It would be
  a Cole clone, not an ally, and needs the hero's script set-up. Parked.
