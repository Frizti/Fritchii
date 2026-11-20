// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Precompile recipes: a game's precompiled pipeline file (tools/nx2-precompile) without the game's
// code, so it can be shared. A recipe holds, for every pipeline the file prepares, its render state
// and, for each of its shaders, the program's fingerprint (FileSys::Nx2ShaderExtract) and the facts
// its translation asked for (constant buffer values, texture types, storage buffer pointers, ...):
// the shader environment of GenericEnvironment::Serialize without its code and program header.
// Assembling it with the programs extracted from a player's own copy of the game puts the code back,
// lowered as GenericEnvironment::LowerNativeCode lowers it, and gives that player's precompiled file.
//
// Recipe layout: "NX2RCP01", u32 body size, u32 0, then the body as one zstd frame:
//   u32 format (1), u64 title id, u32 game version, u32 cache version of the facts
//   u32 n, then n programs: u64 fingerprint, u8 stage (Shader::Stage), 3 bytes 0
//   u32 n, then n fact sets: u32 program, u8 flags (2: a mesh environment's geometry header is the
//     one in the code; no other flag is valid), 3 bytes 0, u32 size, the facts
//
// A recipe carries none of the game's code or program headers: an environment whose program header
// is not its code's, whose mesh geometry header is neither the code's nor zero, whose layout is
// unknown, or whose facts hold any 32 bytes of its program's code is withheld, with its records.
//   u32 n, then n render states: the graphics key without its six shader hashes (1008 bytes)
//   u32 n, then n records: u8 kind (0 graphics, 1 compute), u8 shader count, u16 0, u32 fact set
//     per shader; graphics: u32 render state, u8 shader of each of the six key slots (0xff none),
//     u8 has_xor, u8 0, then if has_xor six u64 that the slots' hashes are xored with (task and
//     mesh keys add their mesh state)
//   then, optionally, sections up to the end: u32 tag, u32 size, the payload. Readers before them
//   stop after the records, so a recipe with sections still assembles there (format stays 1).
//     "HNT1" variant hints: u32 records (must equal the record count), u32 sources (occlusion
//       files learned from), then one u8 per record: bit k = occlusion variant kind k used
//     "SRC1" provenance, text: what made the recipe (nx2-precompile merge), no times or paths

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/nx2_shader_extract.h"
#include "shader_recompiler/stage.h"

namespace VideoCommon::Nx2Recipe {

/// A native program as the runtime lowers it: program header, lowered words with scheduling slots,
/// padding and the native address map (GenericEnvironment::LowerNativeCode).
struct Lowered {
    std::vector<u64> code;
    u32 texture_bound{~0U};
    u64 hash{};       ///< the shader hash pipeline keys use
    u64 native_key{};
};

[[nodiscard]] std::optional<Lowered> Lower(Shader::Stage stage, std::span<const u8> control,
                                           std::span<const u8> code, std::string& error);

[[nodiscard]] u64 NativeShaderHash(std::span<const u64> code);

[[nodiscard]] Shader::Stage StageOf(FileSys::Nx2ShaderExtract::Stage stage) noexcept;

struct Report {
    u32 programs{};          ///< distinct programs the recipe names
    u32 programs_missing{};  ///< of them not in the game (another version of it)
    u32 programs_failed{};   ///< found but not lowered
    u32 environments{};
    u32 environments_withheld{}; ///< left out of a recipe as they would carry game data
    u32 records{};
    u32 records_dropped{};   ///< records with a missing or failed program
    u32 states{};
    std::vector<std::string> errors;
};

struct Header {
    u64 title_id{};
    u32 game_version{};
    u32 cache_version{};
};

/// A recipe from a precompiled file and the programs it was built from.
[[nodiscard]] std::vector<u8> MakeRecipe(std::span<const u8> precompiled,
                                         const FileSys::Nx2ShaderExtract::Result& programs,
                                         const Header& header, Report& report,
                                         unsigned threads = 0);

/// The recipe's header, without reading the rest.
[[nodiscard]] std::optional<Header> ReadHeader(std::span<const u8> recipe);

/// 32 bytes of a game's program (dense: at least half not zero) found in a recipe.
struct CodeMatch {
    u64 fingerprint{};
    size_t program_offset{}; ///< into the program's control section, then its code
    size_t recipe_offset{};  ///< into the recipe's uncompressed body
};

/// Any of the game's code a recipe holds. MakeRecipe refuses to make one that holds some; this
/// checks a recipe from elsewhere against a copy of the game before it is shared.
[[nodiscard]] std::vector<CodeMatch> FindGameCode(std::span<const u8> recipe,
                                                  const FileSys::Nx2ShaderExtract::Result& programs,
                                                  unsigned threads = 0);

/// Occlusion variant hints (recipe section "HNT1", see the layout above): for each record, the
/// occlusion variant kinds play built for it (FRITCHII_OCCL_FLAG_PERSIST records): bit k set for
/// kind k, 0 the lean sibling, 1 the host flag program, 2 colour, 3 pre-pass, 4 no-colour,
/// 5 pre-pass no-colour. Kinds per guest record only: nothing of the device that recorded them.
struct VariantHints {
    u32 sources{};          ///< occlusion files the hints were learned from (0: no hints)
    std::vector<u8> kinds;  ///< one mask per record
};
inline constexpr u8 VariantKindCount = 6;

/// The recipe's hints, one mask per recipe record; nullopt when it has none.
[[nodiscard]] std::optional<VariantHints> ReadVariantHints(std::span<const u8> recipe);

/// A precompiled file of `cache_version` from a recipe and the programs of the player's game.
/// With `hints`, the recipe's variant hints for the file's records (record i of the file, the
/// records dropped left out), or none (sources 0, no kinds) when the recipe has none.
[[nodiscard]] std::vector<u8> Assemble(std::span<const u8> recipe,
                                       const FileSys::Nx2ShaderExtract::Result& programs,
                                       u32 cache_version, Report& report, unsigned threads = 0,
                                       VariantHints* hints = nullptr);

/// The recipe's hints for the precompiled file assembled from it, one mask per record of the
/// file in file order: the recipe's records map one to one when the assembly dropped none, which
/// `file_record_count` (the file's header, offset 16) shows. nullopt when the recipe has no hints
/// or records were dropped (the file's records are then not the recipe's by index).
[[nodiscard]] std::optional<VariantHints> HintsForFile(std::span<const u8> recipe,
                                                       u32 file_record_count);

struct MergeSource {
    std::string name;
    std::span<const u8> bytes;
};

struct MergeReport {
    u32 records_before{}, records_after{};
    u32 programs_before{}, programs_after{};
    u32 facts_before{}, facts_after{};
    u32 states_before{}, states_after{};
    u32 recipe_programs_missing{}; ///< recipe programs not in the game: their records not compared
    u32 cache_graphics{}, cache_compute{};
    u32 in_recipe{};          ///< the same key and facts as a recipe record
    u32 answered{};           ///< the same key, other facts a record's answers hold for
                              ///< (texture handles aside: FileEnvironment::AnswersHoldFor)
    u32 duplicates{};         ///< the same as one added from an earlier cache entry
    u32 added_graphics{}, added_compute{};
    u32 added_new_key{};      ///< graphics added with a key the recipe had no record of
    u32 added_new_facts{};    ///< added with a key the recipe had, under other facts
    u32 converted_form{};     ///< states turned into the recipe's device form (logic op)
    u32 device_specific{};    ///< states left in their device's form (other dynamic state)
    u32 not_expressible{};    ///< entries with a program not in the game or withheld facts
    u32 occl_variants{}, occl_refusals{}, occl_unmatched{};
    std::array<u32, VariantKindCount> variants_by_kind{};
    std::array<u32, VariantKindCount> hinted_by_kind{}; ///< records whose mask has the kind
    u32 hinted_records{};
    std::vector<std::string> errors;
};

/// The recipe with the pipelines of the disk caches (vulkan-frizti.fork-v<N>.bin) it lacks, in
/// its own terms (programs by fingerprint, facts, render states in the recipe's device form),
/// and variant hints from occlusion files (vulkan-frizti-occl.fork-v<N>.bin). A cache pipeline
/// is added when no recipe record has its key (device fields aside) with the same facts. The
/// recipe's own content is kept as it is; additions follow it in input order. Refusal records
/// are left out (their memo holds the build and the device). Deterministic for the same inputs.
[[nodiscard]] std::vector<u8> Merge(std::span<const u8> recipe,
                                    const FileSys::Nx2ShaderExtract::Result& programs,
                                    std::span<const MergeSource> caches,
                                    std::span<const MergeSource> occlusion, MergeReport& report,
                                    unsigned threads = 0);

} // namespace VideoCommon::Nx2Recipe
