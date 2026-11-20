# Recipe merge and occlusion variant hints

`nx2-precompile merge` grows a precompile recipe (`*.nx2recipe`, `src/video_core/nx2_recipe.h`) with
what recorded play needed and the recipe lacks, and records which occlusion variant kinds play
built for each recipe record. The output is a new recipe; the input is never changed.

```
nx2-precompile merge --recipe "OoT v131072 (cache v54).nx2recipe" --game GAME.nup \
    --cache A/vulkan-frizti.fork-v54.bin --cache B/vulkan-frizti.fork-v54.bin \
    --occl A/vulkan-frizti-occl.fork-v54.bin --out merged.nx2recipe [--threads N]
```

`--cache` and `--occl` repeat; at least one of them is needed. The game must be the recipe's title
and version (its programs give the fingerprints; nothing of their code goes into the recipe), with
every package the recipe was made from: for OoT a `.nup` holding the base and the v131072 update
(a `.nup` with the update alone lacks 47 of the recipe's programs; `recipe programs not in this
game` in the output says so, and those records are then not compared). Build with
`-DENABLE_DEBUG_TOOLS=ON`, target `nx2-precompile`. Each run prints what it added; the recipe's
`SRC1` section keeps a line per merge.

## Base pipelines (`--cache`)

Every pipeline of each disk cache (`SerializePipeline` records: environments with their code, then
the 1056-byte graphics key or the 24-byte compute key) is put in recipe terms the way `MakeRecipe`
does it (`FactsOf` in `nx2_recipe.cpp`, shared with `MakeRecipe`): each environment's program by
its shader hash (the lowest fingerprint for programs lowering to the same code), its facts (the
environment without code and program header, tables put in key order), with the same withholding
rules. An entry whose program is not in the game, or whose facts would be withheld, is left out
(`not expressible`).

An entry is added when no recipe record has the same **key and facts**:

- key: the six shader hashes and the bytes of the state the key compares
  (`FixedPipelineState::Size`), without the driver fields and, with logic op off, without the logic
  operation and its dynamic-state flag (the preparer's `RecordedKeyForThisDevice` and
  `CanonicalizeKeyState` make those equal across devices);
- facts: per shader, the program's code hash, the fact-set flags and the facts in key order (the
  runtime's tables are hash maps, so their order means nothing).

A cache pipeline whose key the recipe has, under other facts, is added only when no record of
that key would serve its draw: the fast path takes a record when the live state answers the
record's questions as recorded (`FileEnvironment::AnswersHoldFor`), and the merge runs those
checks with the cache entry's recorded answers standing for the live state (`AnswersHold` in
`nx2_recipe.cpp`). Texture handles may differ where the textures' types and pixel format traits do
not, as at runtime; an answer the entry does not hold counts as different, and format traits are
compared in full (the runtime compares only those its descriptors use), so the check errs towards
adding. Without it, most cache entries would be "new": every session reads other bindless handles,
so its facts never equal the recipe's byte for byte (OoT: 9162 of 10606 entries are answered that
way). Such an added record is one more record of the key, as compute records already have one per
storage pointer state. Repeats across caches are added once.

Added render states are written in the recipe's **device form** (the dynamic-state flags and driver
fields most of its graphics records have): driver fields are the recipe's (the preparer puts its
own there), and a state that differs only by a static logic op with logic op off is turned into the
recipe's form, which every device's preparer reads. A state that differs in other dynamic state is
left as recorded (counted `device_specific`): devices like the recorder use it, others skip it as
`other device`. Nothing else of the recording device goes in.

The recipe's own bytes are kept exactly (checked: its body is written back first and compared);
additions follow in input order (caches in command-line order, entries in file order), new programs
and fact sets after the recipe's, existing fact sets and states reused. The same inputs give the
same file. As `MakeRecipe`, the merge refuses to write a recipe holding any 32 bytes of the game's
programs.

## Variant hints (`--occl`)

The occlusion file (`vulkan-frizti-occl.fork-v<N>.bin`, `FRITCHII_OCCL_FLAG_PERSIST`) holds, per
variant built, its kind and its parent's key (magic `occl_v1`, `occl_v2` or `occl_v3`: all three start a
variant record with these; v3 is v2's layout, bumped by the census_use fix). Each variant's parent key is compared with the merged
recipe's records as above (key only) and sets bit `kind` of every matching record's mask:

| bit | kind |
| --- | --- |
| 0 | lean sibling (no pixel program) |
| 1 | host flag program |
| 2 | colour (guest pixel program with the flag store) |
| 3 | pre-pass |
| 4 | no-colour (host flag program, colour writes off) |
| 5 | pre-pass, no colour |

Refusal records are left out: their memo hash holds the build revision and the parent's descriptor
signature, which are this build's and this device's, not the guest's. Masks from a recipe that had
hints are kept and ORed; records added later start at 0.

## Format (backwards compatible)

The recipe format stays 1. After the records come optional sections up to the end of the body:
`u32 tag, u32 size, payload`. Readers made before them stop after the records (`Assemble` reads
sequentially and ignores the rest; `ReadHeader` reads only the header), so a merged recipe
assembles in older builds, without hints.

- `HNT1`: `u32 records` (equal to the record count, else the section is ignored), `u32 sources`
  (occlusion files learned from), then one `u8` mask per recipe record.
- `SRC1`: provenance text, one line per merge: input sizes and content hashes, counts added. No
  paths, no times.

Unknown sections are kept as they are by a later merge.

## What the preparer reads

The hints stay in the recipe; nothing beside the precompiled file changes. (Writing them beside it
from `EnsurePrecompiledFile` would change `nx2_precompile.cpp`, which the Preparing shaders stamp
hashes: every player would prepare again. `nx2_recipe.*` is not hashed.)

The preparer (or whatever reads the precompiled file) gets them so:

1. the recipe the file was assembled from: `Nx2Precompile::FindRecipe(title_id, title_dir,
   cache_version)`, the same one `EnsurePrecompiledFile` used when the file's note
   (`<file>.recipe.txt`) reads `recipe <size> <content_hash:016x>` with that recipe's `size` and
   `content_hash`; read its bytes;
2. `Nx2Recipe::HintsForFile(recipe_bytes, file_record_count)`, `file_record_count` the u32 at
   offset 16 of the `.nx2prep` header: one mask per record of the file in **file order** (the
   records section's order, before `FRITCHII_PREP_ORDER` reordering). nullopt (no hints, or the
   assembly dropped records so indices no longer line up, or another recipe) means no hints: keep
   today's behaviour, all kinds;
3. for record `i`, build occlusion variant kind `k` only when `kinds[i] & (1 << k)`; `sources` is
   the number of occlusion files learned from. A record with mask 0 had no variant in those
   sessions (records the merge added from caches start at 0).

`Nx2Recipe::ReadVariantHints(recipe)` gives the masks per recipe record, and
`Assemble(..., &hints)` gives them per record of the file it writes, dropped records left out (the
tool's `assemble` prints how many records have hints).

`PipelineCache::PreparePrecompiled` does this with `FRITCHII_PREP_OCCL_VARIANTS=1`
(`PrepOcclHints` in `vk_pipeline_cache.cpp`): the kinds it builds for record `i` are
`OcclPrepKinds(...) & kinds[i]`, `i` the record's index in the file whatever order
`FRITCHII_PREP_ORDER` takes them in. Its log says where the hints came from (or why there are
none) and, per kind, how many variants were built and how many the hints left out. Neither the
prepared stamp nor the kept file's stamp names the hints: a new recipe with other hints prepares
again only when the precompiled file it assembles changes (or with Prepare Shaders Now).
