# Game RE foundation — research (2026-10-18)

Goal: make game-logic mods easy (first one: a map showing every blast shard;
later map improvements, maybe co-op) by recovering names and structure from
the EBOOT itself, then pinning down the shard and map systems.

Everything below came from static analysis of `disc/PS3_GAME/USRDIR/EBOOT.elf`
(BCUS98125 01.00). Nothing was run in the game. Items marked **H** are
hypotheses with their evidence; section 5 is the one game run that confirms
or kills them.

## 0. Tools and outputs

| Path | Tracked | What |
|---|---|---|
| `work/tools/re/gen_symbols.py` | yes | builds the symbol index from the ELF, the loader's function list and the lifter's function set; ~4 s, no dependencies |
| `work/tools/re/who.py` | yes | lookup CLI over the index (below) |
| `work/tools/re/manual_names.json` | yes | hand-checked names (section 4), applied over everything |
| `work/tools/re/shard_check.py` | yes | checks a trace log against the shard hypotheses, writes a shard CSV |
| `work/port/re_trace.c` | yes | env-gated trace hooks + memory watcher (section 5) |
| `work/runs/re/symbols.json` | **no** | addr → name, class, tags, evidence; plus every class (id, parent, size, Create, ctor, vtables, script members) |
| `work/runs/re/index.json` | **no** | call graph, string xrefs, data reads/writes, vtables, name tables |
| `work/runs/re/names.txt`, `re_names.h` | **no** | `who.py --names` / `--header`: "addr name" list and a comment header to read beside the lifted code |

`.gitignore` now admits `work/tools/re/` and `work/port/re_trace.c`; the index
files stay under `work/runs/` (ignored) because they hold the game's strings.

```
work/tools/re/gen_symbols.py                  # rebuild the index
work/tools/re/who.py 0x96054                  # function: names, evidence, callers, callees, strings, globals
work/tools/re/who.py -d 0x95724               # annotated disassembly (calls named, TOC/pool loads resolved)
work/tools/re/who.py CBlastShard [-v]         # class: id, parents, children, size, Create/ctor, vtable, members
work/tools/re/who.py -s 'shard|ping'          # regex over names, strings, classes and script members
work/tools/re/who.py -x 0x9F7620+0x700        # who reads/writes/takes the address of a data range
work/tools/re/who.py --disp 0x338,0x33c -s Shard   # who touches a field offset (any base)
work/tools/re/who.py -t shard                 # functions carrying a tag
```

## 1. What the binary gives (method)

**No symbols, no RTTI.** There are no mangled `N...E` strings and no typeinfo;
vtables have no RTTI slot. The C++ RTTI route is closed. What replaces it is
better: the engine's own **type registry**, built for its script language.

**Addressing.** SNC never materialises a data address with `lis/addi`
(no `lis` immediate in the binary is an address). Every address comes from the
TOC (r2 = 0x973528): a TOC slot holds either the address itself or a *section
anchor* biased by 0x8000 (`lwz r30, -0x7ED4(r2)` → 0x9431F8; then
`lwz r9, -0x7FF8(r30)` reads the pool word at 0x93B200, which holds the
address of a global, 0x9F7620). The pools live in the section at
0x939320–0x94DD28. The tracker in `gen_symbols.py` follows TOC → anchor → pool
→ global, keeps an anchor valid across the whole function when the register is
defined once (prologue load, epilogue restores ignored), and records for every
function: calls, tail calls, virtual calls by slot (`lwz vptr,0(obj)` /
`lwz slot,K(vptr)` / `lwz fn,0(slot)` / `bctrl`), strings, global reads and
writes, constants stored, return values.

**Function units.** The loader lists 13,472 functions (those with an .opd
descriptor); the lifter emits 32,578 `func_<addr>` (every bl target and every
address something can jump to). Analysis uses loader functions + bl targets +
lifter entries that start right after a `blr`/`bctr`/`b`; the 398 remaining
lifter entries are mid-function and are named `<unit>+0xNN`.

**The type registry (BCUS98125 01.00 addresses, checked against the strings at
load).** Four tables indexed by type id, 553 types (0 = CBasic … 552 = CPolycut):

| Table | Address | Entry |
|---|---|---|
| enum names | 0x9806AC | `BLAST_SHARD` |
| class names | 0x981780 | `CBlastShard` |
| data-struct names | 0x982040 | `SBlastShardData` |
| script members | 0x92F3FC | `{u32 count, u32 list}`, list = pointers to 0x34-byte member records |

- **Class info** `{+0 parent info, +4 id, +8 last id of subtree, +C ?, +10 Create(alloc), +14 Destroy(obj)}`.
  Ids look handed out depth first (+8 >= +4, leaves have +4 == +8), so
  IS_DERIVED_FROM would be a range test on +4/+8.
  **Vtable slot 4 returns the class info** (the `cid` getter calls slot 4 and
  reads +4), which names 562 vtables directly. Create allocates (`li r4,<size>`
  → allocator 0xAE198, or 0x48BA90 for tactics) and calls the constructor,
  which gives object sizes and ctors (393 of 436 ctors agree with the vtable
  they store; the rest are shared base ctors). Secondary vtables (interfaces
  inside an object) are recognised from their `addi r3,r3,-K; b method`
  thunks: 747 more, named like `CBlastShard@0x2BC`.
- **Member record** (0x34 bytes): `+0 symbol id, +4 value type, +8 kind bits,
  +C method, +10 field offset, +14 setter, +1C getter, +24 change callback,
  +2C name, +30 signature`. Kind bits: 0x1 method, 0x2 field (offset valid),
  0x4 getter, 0x10 setter, 0x100 callback; 0x8 seems to mark tunables
  (all `kind 0xA` members are plain data like `DISTANT_SHARD_PING_DURATION`).
  A record is owned by the class with the shortest list holding it (inherited
  members repeat in every subclass list). 3,237 records; they name 2,248
  functions as `Class::get_X / set_X / X / on_X` and give **field offsets for
  script-visible fields** — the most useful thing for mods.

**Result.** 12,703 of 32,578 lifted functions named: 5,799 vtable slots of a
named class, 2,527 slots of unnamed vtables (`vt_<addr>::vfN`), 2,248 script
members, 464 Create/Destroy, ~436 constructors, 317 enum-to-string helpers,
274 named after a unique string, 274 imports (NID) + manual, 398 entries.
Slot names come from the base-most class holding that function, so a virtual
keeps one name across subclasses. Assert strings (`file.cpp:line`) exist but
few are referenced from code (~1.3 k strings are), so string tags are sparse.

**Known limits.** No names for non-virtual helpers that neither the registry
nor a string reaches (most of the 19.9 k unnamed). Vtable slot *meanings* are
numbers only; the ones read so far: #3 virtual destructor (Destroy calls it),
#4 GetClassInfo, #9 set-up after load (the shard manager registers its
singleton there), #17/#18 enter/leave world (shard, manager, dead drop, crumbs,
step all enable/disable their ping target there), #19 tear-down (clears the
singleton), #21 per-update work (H).

## 2. Blast shards

Classes (from the registry):

| Class | id | parent chain | size | Create | ctor | vtable |
|---|---|---|---|---|---|---|
| CBlastShard | 43 | CCollectibleObject ← CSo ← CXoSolve ← CXo ← CLo ← CSpawnable ← CBasic | 0x300 | 0x6C05C4 | 0x96054 | 0x8D8C88 (79 slots) |
| CCollectibleObject | 41 | CSo … | 0x2C0 | 0x6C06FC | 0x95FD8 | 0x934100 |
| CBlastShardManager | 287 | CLo ← CSpawnable ← CBasic | 0x350 | 0x6C44B8 | 0x96670 | 0x8D8E48 (26 slots) |
| CBlastShardMarker | 205 | CXo … | 0xA0 | 0x6C77C8 | 0x469ADC | 0x933118 |

Collectible categories (name table 0x984578): 0 Plain, 1 DeadDrop,
**2 BlastShard**, 3 Mine, 4 SystemicMission, 5 DeadDropBird.

**H1 — the world's shard list is the manager's data table, and it holds
positions.** `CBlastShardManager::get_NUM_TOTAL` (0x69CDB0) returns
`*(*(mgr+0x14)+0x48)`; `get_NUM_WORLD_COLLECTED` (0x9557C) walks
`*(mgr+0x14)+0x44` in 32-byte steps and asks `ShardEntry_IsCollected`
(0x95548) of each, which `lvx`-loads the entry's first 16 bytes as a vector
and turns x, y, z into the shard id. So mgr+0x14 points at the loaded
SBlastShardManagerData (H: from the level data, not built at run time), whose
{+0x44 array, +0x48 count} lists every shard, position first.
Manager singleton: **global 0x9BFED0** (set in vf9 0x98118, cleared in vf19
0x96978). Evidence: the three functions above; `who.py --disp 0x44,0x48 -s
BlastShard` finds no other user.

**H2 — shard id = hash of its position.** `ShardIdFromPos` (0x952AC):
`id = int(x) ^ int(y)*2089 ^ int(z)*17939` (fctiwz, low 32 bits). Also used by
`CollectibleComp_InitIdFromPos` (0x962CC, dead drops / mines take their id the
same way) and `Collectibles_CollectShardAtPos` (0x985AC, reached from the
script method `COLLECT_MISSION_SHARDS_FROM_POS`).

**H3 — collected state is a sorted {category, id} vector in the persistent
player block at 0x9F7620 (+0x600 pointer, +0x604 count).**
`Collectibles_CountCategory` (0x94F48; `NUM_COLLECTED` is `li r3,2; b` to it)
counts entries with category 2. `Collectibles_IsCollected(cat,id)` (0x954D4)
binary-searches it (0x5DDEF8 with comparator descriptor 0x951AA8).
`Collectibles_SetCollected(?,cat,id,flag)` (0x982E8) inserts sorted
(0x60AFF0), calls `Persist_MarkDirty(0x9F7620)` (0x122C44, 36 callers; those read so
far are setters of fields in that block) and for categories other than 2 posts a message to the
object at 0xA24B00. The same block holds karma (+0x20), heavy-magic points
(+0x38), kill points (+0x3C) — the `CMissionPersist` getters read them — so
this is the block the save serialises (save path: 0x12A14C / 0x123410 /
0x123454 call the cellSaveData wrappers 0x12430C, 0x1243CC, 0x129B00,
0x129E28; not traced further).

**H4 — per-object state.** A collectible embeds a component at +0x2B0
`{vptr, +4 category, +8 id}`; `CCollectibleObject.COLLECTED` (get 0x6AB6A8,
set 0x6AB6D8) goes through it: `CollectibleComp_IsCollected` (0x95618) and
`CollectibleComp_SetCollected` (0x98704, which calls the store and then the
component's virtual #0, the "collected" notification). Script field
`CBlastShard.IS_COLLECTIBLE` is the byte at +0x2C1 (setter 0x95264).

**H5 — the pulse (sense) works through ping targets.** CBlastShard embeds an
`SPingTarget` at **+0x2BC** (ctor `SPingTarget_SPingTarget` 0x28A7BC; the
string "SPingTarget" is its debug name). Entering/leaving the world (vf17
0x9543C / vf18 0x953F4) calls `SPingTarget_SetEnabled(this+0x2BC, 1/0)`
(0x28A780), which links it into the **ping system at 0xA111E0**
(`PingSystem_Register` 0x28A634). The interface vtable `CBlastShard@0x2BC`
(0x8D8E28) has three thunks into the shard: slot 2 → vf76 (0x96AD0), slot 3 →
vf77, slot 4 → vf78 (0x60AE20, returns `CXo::get_pos`). **vf76 is "pinged"**:
it increments `mgr+0x334` (`BlastShardManager_CountPing`, 0x95234) and calls
0x49544(shard, 1993, 0, 0, 0, 0) (H: posts event 1993 to the shard). Other ping targets: dead drops (and their
manager: `CDeadDropManager.ENABLE_PING_RESPONSE` +0x74), crumbs
(`CCrumbs.REQUIRE_PING`), steps, UGC collectibles. `CHero.IS_PING_ENABLED` is
the hero's byte at +0x1F05.

**H6 — the "distant shard" map ping.** The manager's update, vf21 (0x97E44),
queries the ping system (0x28ADBC on 0xA111E0), reads and resets the ping
count at +0x334 and calls `BlastShardManager_ShowDistantPing` (0x95724),
which reads the script tunables `DISTANT_SHARD_PING_ICON` (+0x338, type 7 =
name) and `DISTANT_SHARD_PING_DURATION` (+0x33C, float) and puts a timed icon
on the map through `MapIcons_Add` (0x463198) on the map-icon registry. Exactly
what the pulse shows when no shard is close is not yet read (H: an icon in the
direction of the nearest uncollected entry of the H1 table).

## 3. Map and minimap

| Class | id | parent | size | notes |
|---|---|---|---|---|
| CWorldMap | 93 | CUno (UI) | 0x320 | the map screen: `SHOW_MAP` 0x4D9AAC, `PUSH_MAP` 0x4E1D7C, `POP_MAP` 0x4E1A64, `SET_DEFAULT_ICON`, `USER_WAYPOINT_ICON` |
| CMapDisplay | 94 | CUno | 0x360 | the drawn map, full screen or minimap: `IS_MINIMAP` (get 0x69C688), `VIEW_CENTER` +0x320, `VIEW_RADIUS` +0x330, `VIEW_HEADING` +0x334, `MAP_DISPLAY_KIND` +0x34C, `STICKY_RADIUS_PERCENT` +0x348 |
| CMapIconSet / CMapBorders | 217 / 218 | CXo | 0xF0 | icon art sets |
| CMapIconStub | 325 | CSpawnable | 0x1C | `MAP_ICON` objects placed in data |
| CMapIconRef | 519 | CBasic | — | a script handle to one icon: `SET_POS` 0x4E655C, `SET_COLOR`, `ALPHA`, `scale`, `RANGE_MIN`, `REMOVE_ICON` 0x4E6A40 |

**H7 — one global registry owns every map icon: 0xABDF40.** 30 functions take
its address. Its API: `MapIcons_Add` (0x463198, 12 callers: shard manager
distant ping, `CChopper.set_MAP_ICON`, mission code…) and `MapIcons_Remove`
(0x461B84, 17 callers; `CChopper.set_MAP_ICON` calls remove then add).
`CMissionMgr.ADD_MISSION_MAP_ICON` / `REMOVE_MISSION_MAP_ICON`,
`CUgcWorld.SET_MAP_ICON_FOR_CLASS`, `CNpc.MAP_ICON_OVERRIDE` (+0x1FDC),
`CNpc.CLAMP_TO_MINIMAP` (+0x1FE0), `CMissionLoc.CLAMP_ICON_TO_MINIMAP`,
`CMission.SHOW_RADAR_ALWAYS` are the other entry points. CMapDisplay
overrides only slots 2–4, so drawing goes through the UI/icon code around
0x45D000–0x467000 (not read yet). Argument layout of `MapIcons_Add` is the
first thing to get from the trace (section 5).

**What this means for the shard-map mod.** Everything it needs is reachable
from two globals: the shard table (H1) for positions, the collected vector
(H3) or `CollectibleComp_IsCollected` for state, and the icon registry (H7)
to draw. A native mod can, once per level load, add one icon per uncollected
entry (and remove it when the count at 0x9F7C24 grows), or a script-side mod
can drive `CMapIconRef`. Calling `MapIcons_Add` from our C needs its
arguments (trace) and a guest call from host code (the port already does
guest callbacks, `harness_guest_caller`).

## 4. Names applied by hand

`work/tools/re/manual_names.json` (all from reading the code above):
Collectibles_CountCategory 0x94F48, Collectibles_IsCollected 0x954D4,
Collectibles_SetCollected 0x982E8, Collectibles_CollectShardAtPos 0x985AC,
CollectibleComp_IsCollected 0x95618, CollectibleComp_SetCollected 0x98704,
CollectibleComp_InitIdFromPos 0x962CC, ShardIdFromPos 0x952AC,
ShardEntry_IsCollected 0x95548, BlastShardManager_CountPing 0x95234,
BlastShardManager_ShowDistantPing 0x95724, SPingTarget_SetEnabled 0x28A780,
SPingTarget_SPingTarget 0x28A7BC, PingSystem_Register 0x28A634,
MapIcons_Add 0x463198, MapIcons_Remove 0x461B84, Persist_MarkDirty 0x122C44.

## 5. In-game confirmation (one run)

**Hook mechanism and its limit.** `re_trace.c` registers wrappers with
`ppu_register_function`, the map the indirect-call dispatcher
(`ps3_indirect_call`) looks targets up in. So it sees calls through vtables,
script members and callbacks — and **not** direct `bl` calls between lifted
functions. The targets below are all reached indirectly (each has an .opd
descriptor and 0 direct callers, checked with the index), except
`get_NUM_COLLECTED` / `get_NUM_WORLD_COLLECTED` / vf21, which have one or two
direct callers too. The direct-only functions (`Collectibles_SetCollected`,
`MapIcons_Add`, `ShowDistantPing`, `SPingTarget_SetEnabled`) are covered by
watching the memory they change. (Hooking direct calls would need the lifter's
`--weak` output and a full re-lift; not worth it yet.)

**Build.** `re_trace.c` is in the executable's source list
(`work/port/CMakeLists.txt`) and `main.cpp` calls `re_trace_init()` after the
last registration (`lv2_init_syscalls`). The next build recompiles main.cpp
and re_trace.c and relinks; with no `PS3RECOMP_RE_*` variable set nothing
changes at run time. Checked with `-fsyntax-only` using the build's own
command lines (`ninja -t commands`), and the parser/watch/table logic with a
host harness against a fake guest memory; not built or run in the game.

**Run.**

```
R=$PWD/work/runs/re
export PS3RECOMP_RE_NAMES=$R/names.txt PS3RECOMP_RE_LOG=$R/trace-1.log PS3RECOMP_RE_DUMP=0x40
export PS3RECOMP_RE_TRACE='0x9543C,0x953F4,0x60B1EC,0x96B64,0x98118,0x96978,0x97E44,0x6AB6D8,0x6AB6A8,0x4D845C,0x4E5634,0x4E5590,0x4E556C,0x9510C,0x9557C,0x69CDB0,0x4D9AAC,0x4E1D7C,0x4E1A64,0x4E655C,0x4E6A40'
export PS3RECOMP_RE_WATCH='0x9BFED0,0x9BFED0>0x334,0x9BFED0>0x338:8,0x9F7C24,0xA111E0:16,0xABDF40:32'
export PS3RECOMP_RE_TABLE='0x9F7C20;0x9F7C24;8|0x9BFED0>0x14>0x44;0x9BFED0>0x14>0x48;32'
work/play.sh
work/tools/re/shard_check.py $R/trace-1.log --csv $R/shards.csv
```

(Quote the values: `>` and `;` are shell syntax.)

In the game: load a save in the open world; stand still ~10 s; pulse (sense)
once near a known shard and once far from any; collect one shard; open the
map, close it; quit. Note the time of each step.

| Hypothesis | Confirmed when the log shows | Killed when |
|---|---|---|
| H1 | `watch 009BFED0` turns non-zero at load; `table 009BFED0>0x14>0x44` dumps N entries, N = `get_NUM_TOTAL` ret, first three words plausible floats (world coordinates) | count 0 or garbage words |
| H2 + H3 | `shard_check.py`: every category-2 entry of the collected vector matches a table entry's hash; its count equals the menu's "Blast Shards: X of Y" X | ids do not match |
| H3 (write) | on pickup, `watch 009F7C24` +1 and the collected table re-dumps with one new `00000002 <id>` whose id is a table entry near the player | count unchanged on pickup |
| H4 | `set_COLLECTED` (0x6AB6D8) logged at pickup with r3 = shard, r4 = 1 — or not, which tells the pickup is native (then trace vf51 0x96B64 and the event path next) | — (either outcome is information) |
| H5 | near-shard pulse: `CBlastShard@0x2BC::vf2` (0x60B1EC) entries with r3 = shard+0x2BC; `watch 009BFED0>0x334` rises; vf17/vf18 at streaming in/out | no vf2 call on a pulse next to a shard |
| H6/H7 | far pulse: `>0x334` back to 0 after vf21 and the `0xABDF40` window changes (icon added), again ~`DISTANT_SHARD_PING_DURATION` s later (removed) | no change in the registry |
| map | `SHOW_MAP`/`PUSH_MAP`/`POP_MAP` on opening/closing the map | — |

`PS3RECOMP_RE_MAX` (default 200) caps each hook's log; vf21 runs every update
and will hit it, which is fine (its first calls are what matter).

## 6. Next

1. Run section 5, fold the answers into `manual_names.json` and this file.
2. Read `MapIcons_Add` (0x463198 → 0x462AF8) for its argument layout, and
   the icon draw loop in CMapDisplay's code region.
3. Shard map mod (its own RPI): at level load read the H1 table, add one icon
   per uncollected shard through `MapIcons_Add`, drop it when collected.
4. Name the generic CLo/CSo slots (#9/#17/#18/#19/#21 and the rest) from the
   base implementations, which would rename ~5 k `Class::vfN` at once.
