// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/nx2_recipe.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include "common/lowend_sim.h"
#include "common/cityhash.h"
#include "common/zstd_compression.h"
#include "shader_recompiler/frontend/ampere/instruction.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ampere/program_override.h"
#include "shader_recompiler/byte_addressed_image.h"
#include "shader_recompiler/program_header.h"

namespace VideoCommon::Nx2Recipe {
namespace {

namespace X = FileSys::Nx2ShaderExtract;
using Shader::Stage;
using Shader::Ampere::Instruction;

constexpr u64 NativeHashTag = 0x4f554e4345000002ULL; // shader_environment.cpp NativeShaderHash
constexpr size_t SphSize = sizeof(Shader::ProgramHeader);
constexpr size_t NativeHeaderSize = 128;
constexpr size_t EnvHeaderSize = 76;    // GenericEnvironment::Serialize up to the code
constexpr size_t GraphicsKeySize = 1056;
constexpr size_t KeyHashesSize = 48;    // six u64 shader hashes at the start of a graphics key
constexpr size_t StateSize = GraphicsKeySize - KeyHashesSize;
constexpr size_t ComputeKeySize = 24;
constexpr u32 Format = 1;
constexpr std::string_view RecipeMagic{"NX2RCP01"};
constexpr std::string_view PrecompiledMagic{"NX2PREP1"};

using Bytes = std::span<const u8>;

template <typename T>
T Load(Bytes bytes, size_t offset) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        throw std::runtime_error(fmt::format("truncated input at {:#x}", offset));
    }
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

struct Writer {
    std::vector<u8> bytes;
    template <typename T>
    void Put(const T& value) {
        const auto* p = reinterpret_cast<const u8*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof(T));
    }
    void Put(Bytes data) {
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    void Put(const std::vector<u8>& data) {
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    void Put(const char* data, size_t n) {
        bytes.insert(bytes.end(), reinterpret_cast<const u8*>(data),
                     reinterpret_cast<const u8*>(data) + n);
    }
    void Zeros(size_t n) {
        bytes.insert(bytes.end(), n, u8{0});
    }
};

struct Reader {
    Bytes bytes;
    size_t at = 0;
    template <typename T>
    T Get() {
        const T value = Load<T>(bytes, at);
        at += sizeof(T);
        return value;
    }
    Bytes Take(size_t n) {
        if (n > bytes.size() - at) {
            throw std::runtime_error("truncated input");
        }
        const auto part = bytes.subspan(at, n);
        at += n;
        return part;
    }
};

template <typename F>
void ParallelFor(size_t count, unsigned threads, F&& body) {
    const unsigned n = threads ? threads : std::max(1u, Common::LowEndSim::LogicalProcessors());
    std::atomic<size_t> next{0};
    std::vector<std::jthread> pool;
    for (unsigned t = 0; t < n; ++t) {
        pool.emplace_back([&] {
            for (size_t i; (i = next.fetch_add(1)) < count;) {
                body(i);
            }
        });
    }
}

/// Where a serialized environment's parts are (GenericEnvironment::Serialize).
struct EnvParts {
    Stage stage{};
    size_t code_end{};   ///< the code is [EnvHeaderSize, code_end)
    size_t tables_end{}; ///< then the fact tables
    size_t end{};        ///< then compute's workgroup and shared size, or the program header and
                         ///< what follows it (geometry pass-through mask, mesh registers)
};

EnvParts Parse(Bytes env, bool has_code) {
    EnvParts parts;
    const u64 code_size = has_code ? Load<u64>(env, 0) : 0;
    const u64 types = Load<u64>(env, 8), formats = Load<u64>(env, 16), cbufs = Load<u64>(env, 24),
              pointers = Load<u64>(env, 32), replacements = Load<u64>(env, 40);
    parts.stage = static_cast<Stage>(Load<u32>(env, 72));
    parts.code_end = EnvHeaderSize + code_size;
    parts.tables_end = parts.code_end + types * 8 + formats * 8 + cbufs * 12 + pointers * 9 +
                       replacements * 12;
    parts.end = env.size();
    if (code_size % 8 != 0 || parts.tables_end > parts.end) {
        throw std::runtime_error("bad shader environment");
    }
    return parts;
}

/// The pass-through geometry header a mesh program's code area holds (control +0x68 bit 7, at the
/// offset +0x64 gives), as a mesh draw's environment records it.
std::optional<std::array<u8, SphSize>> MeshHeader(const X::Program& mesh) {
    const auto& control = mesh.control;
    if (control.size() <= 0x68 || !(control[0x68] & 0x80)) {
        return std::nullopt;
    }
    const u32 at = Load<u32>(control, 0x64);
    if (u64{at} + SphSize > mesh.code.size()) {
        return std::nullopt;
    }
    std::array<u8, SphSize> header;
    std::memcpy(header.data(), mesh.code.data() + at, SphSize);
    return header;
}

// Fact set flags. Bit 0 is not used: a recipe never carries a program header.
constexpr u8 MeshHeaderFromCode = 2; ///< a mesh environment's geometry header, taken from the code
constexpr size_t PassthroughMaskSize = 32; // GenericEnvironment gp_passthrough_mask
constexpr size_t MeshRegistersSize = 12;   // GenericEnvironment mesh_registers
constexpr size_t ComputeTailSize = 16;     // workgroup size and shared memory size
constexpr size_t CodeWindow = 32;

bool Dense(Bytes window) {
    return std::count_if(window.begin(), window.end(), [](u8 b) { return b != 0; }) >=
           static_cast<std::ptrdiff_t>(window.size() / 2);
}

u64 WindowHash(const u8* data) {
    return Common::CityHash64(reinterpret_cast<const char*>(data), CodeWindow);
}

/// Whether `facts` hold any 32 bytes of `code` (at an 8-byte step of the code, and dense: at least
/// half its bytes not zero, as instructions are, where mostly zero header fields can match the
/// facts' small numbers by chance).
bool HoldsCode(Bytes facts, Bytes code) {
    std::unordered_set<u64> windows;
    for (size_t at = 0; at + CodeWindow <= code.size(); at += 8) {
        if (Dense(code.subspan(at, CodeWindow))) {
            windows.insert(WindowHash(code.data() + at));
        }
    }
    for (size_t at = 0; at + CodeWindow <= facts.size(); at += 4) {
        if (windows.contains(WindowHash(facts.data() + at))) {
            return true;
        }
    }
    return false;
}

bool AllZero(Bytes bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](u8 b) { return b == 0; });
}

struct Program {
    const X::Program* source{};
    std::optional<Lowered> lowered;
    std::string error;
};

enum class FactsStatus { Ok, NoProgram, OtherCode, Withheld };

/// A recorded environment (GenericEnvironment::Serialize, code included) in recipe terms: its
/// program, found by the shader hash, and its facts, the environment without its code and program
/// header. Anything else that could be the game's (a program header other than the code's, a mesh
/// geometry header not derivable from the code, a layout this does not know, facts holding some
/// of the program's code) withholds it.
struct Facts {
    FactsStatus status{FactsStatus::Ok};
    s64 program = -1;
    u64 hash{};
    u8 flags{};
    Stage stage{};
    std::vector<u8> facts;
    std::string why; ///< why it is withheld
};

Facts FactsOf(Bytes blob, const std::vector<Program>& programs,
              const std::unordered_map<u64, u32>& by_hash) {
    Facts out;
    const auto parts = Parse(blob, true);
    out.stage = parts.stage;
    std::vector<u64> code((parts.code_end - EnvHeaderSize) / 8);
    std::memcpy(code.data(), blob.data() + EnvHeaderSize, code.size() * 8);
    out.hash = NativeShaderHash(code);
    const auto found = by_hash.find(out.hash);
    if (found == by_hash.end()) {
        out.status = FactsStatus::NoProgram;
        return out;
    }
    if (programs[found->second].lowered->code != code) {
        out.status = FactsStatus::OtherCode;
        return out;
    }
    const auto withhold = [&](std::string_view why) {
        out.status = FactsStatus::Withheld;
        out.why = why;
        return out;
    };
    const auto& source = *programs[found->second].source;
    Writer facts;
    facts.Put(blob.subspan(0, EnvHeaderSize));
    facts.Put(blob.subspan(parts.code_end, parts.tables_end - parts.code_end));
    auto tail = blob.subspan(parts.tables_end);
    u8 flags = 0;
    if (parts.stage == Stage::Compute) {
        if (tail.size() != ComputeTailSize) {
            return withhold("unknown compute layout");
        }
    } else {
        const bool task_mesh = parts.stage == Stage::Task || parts.stage == Stage::Mesh;
        const size_t extra = parts.stage == Stage::Geometry ? PassthroughMaskSize
                             : task_mesh                    ? MeshRegistersSize + SphSize
                                                            : 0;
        if (tail.size() != SphSize + extra) {
            return withhold("unknown graphics layout");
        }
        if (std::memcmp(tail.data(), code.data(), SphSize) != 0) {
            return withhold("its program header is not the code's");
        }
        tail = tail.subspan(SphSize);
        if (task_mesh) {
            const auto recorded = tail.last(SphSize);
            const auto derived = parts.stage == Stage::Mesh ? MeshHeader(source) : std::nullopt;
            if (derived && std::memcmp(derived->data(), recorded.data(), SphSize) == 0) {
                tail = tail.first(tail.size() - SphSize);
                flags |= MeshHeaderFromCode;
            } else if (!AllZero(recorded)) {
                return withhold("its mesh geometry header is not derivable from the code");
            }
        }
    }
    facts.Put(tail);
    if (HoldsCode(facts.bytes, source.code) || HoldsCode(facts.bytes, source.control) ||
        HoldsCode(facts.bytes,
                  Bytes{reinterpret_cast<const u8*>(code.data()), code.size() * sizeof(u64)})) {
        return withhold("its facts hold some of the program's code");
    }
    out.program = found->second;
    out.flags = flags;
    out.facts = std::move(facts.bytes);
    return out;
}

std::vector<u8> Compress(Bytes body) {
    Writer out;
    out.Put(RecipeMagic.data(), RecipeMagic.size());
    out.Put(static_cast<u32>(body.size()));
    out.Put(u32{0});
    out.Put(Common::Compression::CompressDataZSTD(body.data(), body.size(), 19));
    return std::move(out.bytes);
}

std::vector<u8> Decompress(Bytes recipe) {
    if (recipe.size() < 16 || std::memcmp(recipe.data(), RecipeMagic.data(), 8) != 0) {
        throw std::runtime_error("not a precompile recipe");
    }
    const u32 size = Load<u32>(recipe, 8);
    auto body = Common::Compression::DecompressDataZSTD(recipe.subspan(16));
    if (body.size() != size) {
        throw std::runtime_error("damaged precompile recipe");
    }
    return body;
}

std::vector<CodeMatch> FindCodeInBody(Bytes body, const X::Result& extracted, unsigned threads) {
    std::unordered_map<u64, size_t> windows;
    for (size_t at = 0; at + CodeWindow <= body.size(); ++at) {
        if (Dense(body.subspan(at, CodeWindow))) {
            windows.try_emplace(WindowHash(body.data() + at), at);
        }
    }
    std::vector<std::vector<CodeMatch>> found(extracted.programs.size());
    ParallelFor(extracted.programs.size(), threads, [&](size_t i) {
        const auto& p = extracted.programs[i];
        for (const Bytes part : {Bytes{p.control}, Bytes{p.code}}) {
            for (size_t at = 0; at + CodeWindow <= part.size(); at += 4) {
                const auto it = windows.find(WindowHash(part.data() + at));
                if (it != windows.end() &&
                    std::memcmp(body.data() + it->second, part.data() + at, CodeWindow) == 0 &&
                    Dense(part.subspan(at, CodeWindow))) {
                    found[i].push_back({p.fingerprint, at, it->second});
                }
            }
        }
    });
    std::vector<CodeMatch> matches;
    for (auto& f : found) {
        matches.insert(matches.end(), f.begin(), f.end());
    }
    return matches;
}

// Sections after the records (nx2_recipe.h).
constexpr u32 SectionHints = 0x31544E48;      // "HNT1"
constexpr u32 SectionProvenance = 0x31435253; // "SRC1"

struct Sections {
    std::optional<VariantHints> hints;
    std::string provenance;
    std::vector<std::pair<u32, std::vector<u8>>> other; ///< unknown sections, kept as they are
};

/// The sections from `r` (just after the records) to the end. Hints for another record count are
/// dropped: they could only be read against the wrong records.
Sections ReadSections(Reader& r, u32 record_count) {
    Sections out;
    while (r.bytes.size() - r.at >= 8) {
        const u32 tag = r.Get<u32>();
        const u32 size = r.Get<u32>();
        const auto payload = r.Take(size);
        if (tag == SectionHints) {
            if (payload.size() < 8) {
                continue;
            }
            const u32 count = Load<u32>(payload, 0);
            if (count != record_count || payload.size() != 8 + size_t{count}) {
                continue;
            }
            VariantHints hints;
            hints.sources = Load<u32>(payload, 4);
            hints.kinds.assign(payload.begin() + 8, payload.end());
            out.hints = std::move(hints);
        } else if (tag == SectionProvenance) {
            out.provenance.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
        } else {
            out.other.emplace_back(tag, std::vector<u8>(payload.begin(), payload.end()));
        }
    }
    return out;
}

/// A recipe body, parsed.
struct RecipeData {
    Header header;
    struct Prog {
        u64 fingerprint{};
        u8 stage{};
    };
    struct FactSet {
        u32 program{};
        u8 flags{};
        std::vector<u8> bytes;
    };
    struct Rec {
        u8 kind{};
        std::vector<u32> envs;
        u32 state{};
        std::array<u8, 6> slots{};
        std::array<u64, 6> xors{};
    };
    std::vector<Prog> programs;
    std::vector<FactSet> facts;
    std::vector<std::vector<u8>> states;
    std::vector<Rec> records;
    size_t records_end{}; ///< where the records end in the body (the sections follow)
    Sections sections;
};

RecipeData ParseBody(Bytes body) {
    RecipeData d;
    Reader r{body};
    if (r.Get<u32>() != Format) {
        throw std::runtime_error("precompile recipe of another format");
    }
    d.header.title_id = r.Get<u64>();
    d.header.game_version = r.Get<u32>();
    d.header.cache_version = r.Get<u32>();
    d.programs.resize(r.Get<u32>());
    for (auto& p : d.programs) {
        p.fingerprint = r.Get<u64>();
        p.stage = r.Get<u8>();
        r.Take(3);
    }
    d.facts.resize(r.Get<u32>());
    for (auto& f : d.facts) {
        f.program = r.Get<u32>();
        f.flags = r.Get<u8>();
        r.Take(3);
        const auto bytes = r.Take(r.Get<u32>());
        f.bytes.assign(bytes.begin(), bytes.end());
    }
    d.states.resize(r.Get<u32>());
    for (auto& s : d.states) {
        const auto bytes = r.Take(StateSize);
        s.assign(bytes.begin(), bytes.end());
    }
    d.records.resize(r.Get<u32>());
    for (auto& rec : d.records) {
        rec.kind = r.Get<u8>();
        rec.envs.resize(r.Get<u8>());
        r.Take(2);
        for (u32& id : rec.envs) {
            id = r.Get<u32>();
        }
        if (rec.kind == 0) {
            rec.state = r.Get<u32>();
            rec.slots = r.Get<std::array<u8, 6>>();
            const bool has_xor = r.Get<u8>() != 0;
            r.Take(1);
            if (has_xor) {
                rec.xors = r.Get<std::array<u64, 6>>();
            }
        }
    }
    d.records_end = r.at;
    d.sections = ReadSections(r, static_cast<u32>(d.records.size()));
    return d;
}

/// The body as MakeRecipe writes it, then the sections.
std::vector<u8> WriteBody(const RecipeData& d, bool sections) {
    Writer body;
    body.Put(Format);
    body.Put(d.header.title_id);
    body.Put(d.header.game_version);
    body.Put(d.header.cache_version);
    body.Put(static_cast<u32>(d.programs.size()));
    for (const auto& p : d.programs) {
        body.Put(p.fingerprint);
        body.Put(p.stage);
        body.Zeros(3);
    }
    body.Put(static_cast<u32>(d.facts.size()));
    for (const auto& f : d.facts) {
        body.Put(f.program);
        body.Put(f.flags);
        body.Zeros(3);
        body.Put(static_cast<u32>(f.bytes.size()));
        body.Put(f.bytes);
    }
    body.Put(static_cast<u32>(d.states.size()));
    for (const auto& s : d.states) {
        body.Put(s);
    }
    body.Put(static_cast<u32>(d.records.size()));
    for (const auto& rec : d.records) {
        body.Put(rec.kind);
        body.Put(static_cast<u8>(rec.envs.size()));
        body.Zeros(2);
        for (const u32 id : rec.envs) {
            body.Put(id);
        }
        if (rec.kind == 0) {
            body.Put(rec.state);
            body.Put(rec.slots);
            const bool has_xor =
                std::any_of(rec.xors.begin(), rec.xors.end(), [](u64 x) { return x != 0; });
            body.Put(static_cast<u8>(has_xor));
            body.Zeros(1);
            if (has_xor) {
                body.Put(rec.xors);
            }
        }
    }
    if (!sections) {
        return std::move(body.bytes);
    }
    const auto put_section = [&](u32 tag, Bytes payload) {
        body.Put(tag);
        body.Put(static_cast<u32>(payload.size()));
        body.Put(payload);
    };
    if (const auto& hints = d.sections.hints) {
        Writer payload;
        payload.Put(static_cast<u32>(hints->kinds.size()));
        payload.Put(hints->sources);
        payload.Put(hints->kinds);
        put_section(SectionHints, payload.bytes);
    }
    if (!d.sections.provenance.empty()) {
        put_section(SectionProvenance,
                    Bytes{reinterpret_cast<const u8*>(d.sections.provenance.data()),
                          d.sections.provenance.size()});
    }
    for (const auto& [tag, payload] : d.sections.other) {
        put_section(tag, payload);
    }
    return std::move(body.bytes);
}

// Render states (FixedPipelineState, video_core/renderer_vulkan/fixed_pipeline_state.h: 1008
// bytes, little endian). The offsets this needs, checked against the layout there.
constexpr size_t StateDriverOffset = 16;  // u32 driver_id, u32 driver_version
constexpr size_t StateDynamicOffset = 72; // DynamicState::raw1
constexpr u32 Raw1Eds = 1U << 0;
constexpr u32 Raw1LogicOpDynamic = 1U << 2; // extended_dynamic_state_2_logic_op
constexpr u32 Raw1Eds3Blend = 1U << 3;
constexpr u32 Raw1DynamicVertexInput = 1U << 5;
constexpr u32 Raw1Xfb = 1U << 6;
/// The device's dynamic state the key records: extended dynamic state 1, 2, 2 logic op, 3 blend,
/// 3 enables, dynamic vertex input, dynamic colour write enable.
constexpr u32 Raw1DeviceBits = 0x3FU | (1U << 20);
constexpr u32 DynamicLogicOpMask = 0xFU << 6;
constexpr u32 DynamicLogicOpEnable = 1U << 10;

/// The bytes of a state a key compares and hashes (FixedPipelineState::Size).
size_t StateUsedSize(Bytes state) {
    const u32 raw1 = Load<u32>(state, 0);
    if (raw1 & Raw1Xfb) {
        return StateSize;
    }
    if ((raw1 & Raw1DynamicVertexInput) && (raw1 & Raw1Eds3Blend)) {
        return 80; // offsetof(attachments)
    }
    if (raw1 & Raw1DynamicVertexInput) {
        return 112; // offsetof(attributes)
    }
    if (raw1 & Raw1Eds) {
        return 368; // offsetof(vertex_strides)
    }
    return 432; // offsetof(xfb_state)
}

/// A graphics key without what only tells devices apart: the driver fields (the preparer sets
/// this device's) and, with logic op off, the logic operation and whether it is dynamic (keyed
/// on drivers with a static logic op, 0 on drivers with a dynamic one; the preparer's
/// RecordedKeyForThisDevice and CanonicalizeKeyState make them equal). The six shader hashes
/// and the used bytes of the state.
std::string NeutralKey(Bytes hashes, Bytes state) {
    std::string key(reinterpret_cast<const char*>(hashes.data()), KeyHashesSize);
    std::vector<u8> s(state.begin(), state.begin() + StateSize);
    std::memset(s.data() + StateDriverOffset, 0, 8);
    u32 raw1 = Load<u32>(s, 0);
    u32 dynamic = Load<u32>(s, StateDynamicOffset);
    if (!(dynamic & DynamicLogicOpEnable)) {
        raw1 &= ~Raw1LogicOpDynamic;
        dynamic &= ~DynamicLogicOpMask;
    }
    std::memcpy(s.data(), &raw1, 4);
    std::memcpy(s.data() + StateDynamicOffset, &dynamic, 4);
    key.append(reinterpret_cast<const char*>(s.data()), StateUsedSize(s));
    return key;
}

/// Facts with each table in key order (the runtime's tables are hash maps: their order says
/// nothing), so equal facts compare equal and the recipe does not depend on a session's order.
std::vector<u8> SortedFacts(Bytes facts) {
    const auto parts = Parse(facts, false);
    std::vector<u8> out(facts.begin(), facts.end());
    const std::array<std::pair<u64, size_t>, 5> tables{{
        {Load<u64>(facts, 8), 8},   // texture types: u32 key, u32 type
        {Load<u64>(facts, 16), 8},  // pixel formats: u32 key, u32 format
        {Load<u64>(facts, 24), 12}, // constant buffer values: u64 key, u32 value
        {Load<u64>(facts, 32), 9},  // storage pointer types: u64 key, u8 type
        {Load<u64>(facts, 40), 12}, // constant buffer replacements: u64 key, u32 value
    }};
    size_t at = EnvHeaderSize;
    for (size_t t = 0; t < tables.size(); ++t) {
        const auto [count, size] = tables[t];
        const bool wide = t >= 2;
        std::vector<size_t> order(count);
        for (size_t i = 0; i < count; ++i) {
            order[i] = at + i * size;
        }
        const auto key = [&](size_t offset) {
            return wide ? Load<u64>(facts, offset) : u64{Load<u32>(facts, offset)};
        };
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return key(a) < key(b); });
        for (size_t i = 0; i < count; ++i) {
            std::memcpy(out.data() + at + i * size, facts.data() + order[i], size);
        }
        at += count * size;
    }
    if (at != parts.tables_end) {
        throw std::runtime_error("bad facts");
    }
    return out;
}

/// The size of a serialized environment (GenericEnvironment::Serialize) at the start of `bytes`.
size_t EnvironmentSize(Bytes bytes) {
    const u64 code = Load<u64>(bytes, 0);
    const u64 tables = Load<u64>(bytes, 8) * 8 + Load<u64>(bytes, 16) * 8 +
                       Load<u64>(bytes, 24) * 12 + Load<u64>(bytes, 32) * 9 +
                       Load<u64>(bytes, 40) * 12;
    const auto stage = static_cast<Stage>(Load<u32>(bytes, 72));
    u64 tail = SphSize;
    if (stage == Stage::Compute) {
        tail = ComputeTailSize;
    } else if (stage == Stage::Geometry) {
        tail += PassthroughMaskSize;
    } else if (stage == Stage::Task || stage == Stage::Mesh) {
        tail += MeshRegistersSize + SphSize;
    }
    if (code > bytes.size() || tables > bytes.size() ||
        EnvHeaderSize + code + tables + tail > bytes.size()) {
        throw std::runtime_error("truncated environment");
    }
    return EnvHeaderSize + code + tables + tail;
}

// The occlusion file (vk_pipeline_cache.cpp, FRITCHII_OCCL_FLAG_PERSIST).
constexpr std::string_view OcclMagic{"occl_v2\0", 8};
constexpr u32 OcclRecordMarker = 0x5243434F; // "OCCR"
constexpr u8 OcclRecordVariant = 1;
constexpr u8 OcclRecordRefused = 2;
constexpr std::string_view CacheMagic{"yuzucach"};

/// A fact set's answers (facts without code: Parse(facts, false)).
struct FactView {
    u32 local_memory{}, texture_bound{}, start{}, viewport_transform{};
    Stage stage{};
    std::unordered_map<u32, u32> types, formats;
    std::unordered_map<u64, u32> cbufs;
    std::unordered_map<u64, u8> pointers;
    u64 replacements{};
    std::array<u32, 4> compute_tail{};
};

FactView ViewOf(Bytes facts) {
    const auto parts = Parse(facts, false);
    FactView v;
    v.local_memory = Load<u32>(facts, 48);
    v.texture_bound = Load<u32>(facts, 52);
    v.start = Load<u32>(facts, 56);
    v.viewport_transform = Load<u32>(facts, 68);
    v.stage = parts.stage;
    size_t at = EnvHeaderSize;
    for (u64 i = Load<u64>(facts, 8); i > 0; --i, at += 8) {
        v.types.emplace(Load<u32>(facts, at), Load<u32>(facts, at + 4));
    }
    for (u64 i = Load<u64>(facts, 16); i > 0; --i, at += 8) {
        v.formats.emplace(Load<u32>(facts, at), Load<u32>(facts, at + 4));
    }
    for (u64 i = Load<u64>(facts, 24); i > 0; --i, at += 12) {
        v.cbufs.emplace(Load<u64>(facts, at), Load<u32>(facts, at + 8));
    }
    for (u64 i = Load<u64>(facts, 32); i > 0; --i, at += 9) {
        v.pointers.emplace(Load<u64>(facts, at), Load<u8>(facts, at + 8));
    }
    v.replacements = Load<u64>(facts, 40);
    if (parts.stage == Stage::Compute && facts.size() >= parts.tables_end + ComputeTailSize) {
        v.compute_tail = Load<std::array<u32, 4>>(facts, parts.tables_end);
    }
    return v;
}

/// The traits of a pixel format AnswersHoldFor compares (shader_environment.cpp
/// PixelFormatTraits): integer, R16 float, byte-addressed 32-bit colour, snorm. All of them here,
/// where the runtime compares integer always and the others only for descriptors that use them.
u32 FormatTraits(u32 value) {
    using F = Shader::TexturePixelFormat;
    const auto format = static_cast<F>(value);
    bool integer = false;
    switch (format) {
    case F::A8B8G8R8_SINT:
    case F::A8B8G8R8_UINT:
    case F::A2B10G10R10_UINT:
    case F::R8_SINT:
    case F::R8_UINT:
    case F::R16G16B16A16_SINT:
    case F::R16G16B16A16_UINT:
    case F::R32G32B32A32_UINT:
    case F::R32G32B32A32_SINT:
    case F::R32G32_SINT:
    case F::R16_UINT:
    case F::R16_SINT:
    case F::R16G16_UINT:
    case F::R16G16_SINT:
    case F::R8G8_SINT:
    case F::R8G8_UINT:
    case F::R32G32_UINT:
    case F::R32_UINT:
    case F::R32_SINT:
        integer = true;
        break;
    default:
        break;
    }
    const bool snorm = format == F::A8B8G8R8_SNORM || format == F::R8G8_SNORM ||
                       format == F::R8_SNORM || format == F::R16G16B16A16_SNORM ||
                       format == F::R16G16_SNORM || format == F::R16_SNORM;
    return (integer ? 1U : 0U) | (format == F::R16_FLOAT ? 2U : 0U) |
           (Shader::IsByteAddressed32BitColorFormat(format) ? 4U : 0U) | (snorm ? 8U : 0U);
}

/// Whether a draw whose translation gave `live` would take a record with `recorded`: the checks
/// of FileEnvironment::AnswersHoldFor, with `live`'s recorded answers standing for the GPU state
/// (an answer it does not hold counts as different). Texture handles may differ where the
/// textures' types and formats do not.
bool AnswersHold(const FactView& recorded, const FactView& live) {
    if (recorded.stage != live.stage || recorded.start != live.start ||
        recorded.local_memory != live.local_memory ||
        recorded.texture_bound != live.texture_bound || recorded.replacements != 0 ||
        live.replacements != 0 || recorded.compute_tail != live.compute_tail ||
        (recorded.stage == Stage::VertexB &&
         recorded.viewport_transform != live.viewport_transform)) {
        return false;
    }
    const auto is_pointer_word = [&](u64 key) {
        const u64 index = key >> 32;
        const u32 offset = static_cast<u32>(key);
        return recorded.pointers.contains(key) ||
               (offset >= 4 && recorded.pointers.contains((index << 32) | (offset - 4)));
    };
    std::unordered_set<u32> handles_read;
    for (const auto& [key, value] : recorded.cbufs) {
        const auto live_value = live.cbufs.find(key);
        if (live_value == live.cbufs.end()) {
            return false;
        }
        const auto type = recorded.types.find(value);
        const auto format = recorded.formats.find(value);
        if (type != recorded.types.end() || format != recorded.formats.end()) {
            if (type != recorded.types.end()) {
                const auto live_type = live.types.find(live_value->second);
                if (live_type == live.types.end() || live_type->second != type->second) {
                    return false;
                }
            }
            if (format != recorded.formats.end()) {
                const auto live_format = live.formats.find(live_value->second);
                if (live_format == live.formats.end() ||
                    FormatTraits(live_format->second) != FormatTraits(format->second)) {
                    return false;
                }
            }
            handles_read.insert(value);
            continue;
        }
        if (live_value->second != value && !is_pointer_word(key)) {
            return false;
        }
    }
    for (const auto& [handle, type] : recorded.types) {
        if (!handles_read.contains(handle)) {
            const auto it = live.types.find(handle);
            if (it == live.types.end() || it->second != type) {
                return false;
            }
        }
    }
    for (const auto& [handle, format] : recorded.formats) {
        if (!handles_read.contains(handle)) {
            const auto it = live.formats.find(handle);
            if (it == live.formats.end() || FormatTraits(it->second) != FormatTraits(format)) {
                return false;
            }
        }
    }
    for (const auto& [key, mapped] : recorded.pointers) {
        const auto it = live.pointers.find(key);
        if (it == live.pointers.end() || (it->second != 0) != (mapped != 0)) {
            return false;
        }
    }
    return true;
}

} // Anonymous namespace

std::vector<CodeMatch> FindGameCode(std::span<const u8> recipe, const X::Result& programs,
                                    unsigned threads) {
    return FindCodeInBody(Decompress(recipe), programs, threads);
}

template <typename T>
static void PutRaw(Writer& w, const T* data, size_t n) {
    w.Put(Bytes{reinterpret_cast<const u8*>(data), n});
}

u64 NativeShaderHash(std::span<const u64> code) {
    return Common::CityHash64(reinterpret_cast<const char*>(code.data()),
                              code.size() * sizeof(u64)) ^
           NativeHashTag;
}

Shader::Stage StageOf(X::Stage stage) noexcept {
    switch (stage) {
    case X::Stage::Vertex:
        return Stage::VertexB;
    case X::Stage::TessControl:
        return Stage::TessellationControl;
    case X::Stage::TessEval:
        return Stage::TessellationEval;
    case X::Stage::Geometry:
        return Stage::Geometry;
    case X::Stage::Fragment:
        return Stage::Fragment;
    case X::Stage::Compute:
        return Stage::Compute;
    case X::Stage::Task:
        return Stage::Task;
    case X::Stage::Mesh:
        return Stage::Mesh;
    }
    return Stage::VertexB;
}

std::optional<Lowered> Lower(Stage stage, std::span<const u8> control, std::span<const u8> code_area,
                             std::string& error) {
    const bool compute = stage == Stage::Compute;
    Shader::ProgramHeader sph{};
    size_t start = 0;
    size_t available = 0;
    if (compute) {
        if (control.size() < 0x3c) {
            error = "short control section";
            return std::nullopt;
        }
        available = std::min<size_t>(Load<u32>(control, 56), code_area.size());
    } else {
        if (code_area.size() <= NativeHeaderSize) {
            error = "short code area";
            return std::nullopt;
        }
        std::memcpy(&sph, code_area.data(), SphSize);
        start = NativeHeaderSize;
        available = code_area.size() - NativeHeaderSize;
    }
    std::vector<Instruction> native;
    Shader::Ampere::ProgramCaptureState capture;
    bool complete = false;
    const size_t count = available / sizeof(Instruction);
    for (u32 index = 0; index < Shader::Ampere::ProgramCaptureState::MaximumInstructions; ++index) {
        if (index >= count) {
            break; // the runtime reads on past the code area; the game file holds only the area
        }
        Instruction instruction;
        std::memcpy(&instruction, code_area.data() + start + index * sizeof(Instruction),
                    sizeof(Instruction));
        native.push_back(instruction);
        const auto status = capture.Observe(instruction, index);
        if (status == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
            error = "branch outside capture bounds";
            return std::nullopt;
        }
        if (status == Shader::Ampere::ProgramCaptureState::Result::Complete) {
            complete = true;
            break;
        }
    }
    if (!complete) {
        error = "capture incomplete inside the code area";
        return std::nullopt;
    }
    std::vector<u32> traps;
    for (u32 i = 0; i < native.size(); ++i) {
        if (native[i].Bits(0, 12) == 0x95c) {
            traps.push_back(i * sizeof(Instruction));
        }
    }
    if (!traps.empty()) {
        error = "trap guards"; // checked against live constants; never serialized
        return std::nullopt;
    }
    const auto lowered = Shader::Ampere::LowerToMaxwell(
        native, {.geometry = false,
                 .tensor = true,
                 .native_code_address = 0x100000000ULL + start,
                 .verified_traps = traps,
                 .tessellation_stage = Shader::Ampere::TessellationStage::None});
    if (!lowered) {
        error = fmt::format("lowering failed at {:#x}: {}", lowered.error_offset, lowered.error);
        return std::nullopt;
    }
    Lowered result;
    result.native_key = Shader::Ampere::NativeProgramKey(native);
    result.texture_bound = lowered.required_texture_buffer;
    auto& code = result.code;
    code.resize(SphSize / sizeof(u64));
    std::memcpy(code.data(), &sph, SphSize);
    for (const u64 instruction : lowered.instructions) {
        if ((code.size() * sizeof(u64)) % 32 == 0) {
            code.push_back(0);
        }
        code.push_back(instruction);
    }
    code.push_back(0);
    Shader::Ampere::AppendNativeAddressMap(code, lowered.source_instruction_offsets);
    result.hash = NativeShaderHash(code);
    return result;
}

std::optional<Header> ReadHeader(std::span<const u8> recipe) {
    try {
        const auto body = Decompress(recipe);
        Reader r{body};
        if (r.Get<u32>() != Format) {
            return std::nullopt;
        }
        Header header;
        header.title_id = r.Get<u64>();
        header.game_version = r.Get<u32>();
        header.cache_version = r.Get<u32>();
        return header;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<u8> MakeRecipe(std::span<const u8> precompiled, const X::Result& extracted,
                           const Header& header, Report& report, unsigned threads) {
    if (precompiled.size() < 24 ||
        std::memcmp(precompiled.data(), PrecompiledMagic.data(), 8) != 0) {
        throw std::runtime_error("not a precompiled pipeline file");
    }
    const u32 env_count = Load<u32>(precompiled, 12);
    const u32 record_count = Load<u32>(precompiled, 16);

    // Every extracted program lowered once: the shader hash finds an environment's program.
    std::vector<Program> programs(extracted.programs.size());
    ParallelFor(programs.size(), threads, [&](size_t i) {
        const auto& p = extracted.programs[i];
        programs[i].source = &p;
        programs[i].lowered = Lower(StageOf(p.stage), p.control, p.code, programs[i].error);
    });
    std::unordered_map<u64, u32> by_hash;
    for (u32 i = 0; i < programs.size(); ++i) {
        if (programs[i].lowered) {
            // Programs lowering to the same code: the lowest fingerprint, so the recipe does not
            // depend on the order the extraction found them in.
            const auto [it, added] = by_hash.try_emplace(programs[i].lowered->hash, i);
            if (!added && programs[i].source->fingerprint < programs[it->second].source->fingerprint) {
                it->second = i;
            }
        }
    }

    // Environments: their program and their facts.
    struct Env {
        s64 program = -1;
        u64 hash{};
        u8 flags{};
        std::vector<u8> facts;
        Stage stage{};
    };
    std::vector<Env> envs(env_count);
    size_t offset = 24;
    std::vector<std::pair<u64, u64>> index(env_count);
    for (auto& [at, size] : index) {
        at = Load<u64>(precompiled, offset);
        size = Load<u64>(precompiled, offset + 8);
        offset += 16;
    }
    for (u32 e = 0; e < env_count; ++e) {
        auto& env = envs[e];
        auto made = FactsOf(precompiled.subspan(index[e].first, index[e].second), programs,
                            by_hash);
        env.stage = made.stage;
        env.hash = made.hash;
        switch (made.status) {
        case FactsStatus::NoProgram:
            report.errors.push_back(fmt::format("environment {}: no program of the game has its "
                                                "code ({:016x})", e, env.hash));
            continue;
        case FactsStatus::OtherCode:
            report.errors.push_back(fmt::format("environment {}: same hash, other code", e));
            continue;
        case FactsStatus::Withheld:
            ++report.environments_withheld;
            report.errors.push_back(fmt::format("environment {} ({:016x}) withheld: {}", e,
                                                env.hash, made.why));
            continue;
        case FactsStatus::Ok:
            break;
        }
        env.program = made.program;
        env.flags = made.flags;
        env.facts = std::move(made.facts);
    }

    // Records.
    struct Record {
        u8 kind{};
        std::vector<u32> envs;
        u32 state{};
        std::array<u8, 6> slots{};
        std::array<u64, 6> xors{};
    };
    std::vector<Record> records;
    std::map<std::vector<u8>, u32> states;
    for (u32 i = 0; i < record_count; ++i) {
        Record record;
        record.kind = Load<u8>(precompiled, offset);
        const u8 count = Load<u8>(precompiled, offset + 1);
        offset += 4;
        bool ok = true;
        for (u8 k = 0; k < count; ++k) {
            const u32 id = Load<u32>(precompiled, offset);
            offset += 4;
            ok &= id < env_count && envs[id].program >= 0;
            record.envs.push_back(id);
        }
        if (record.kind == 1) {
            offset += ComputeKeySize;
        } else {
            const auto key = precompiled.subspan(offset, GraphicsKeySize);
            offset += GraphicsKeySize;
            record.slots.fill(0xff);
            size_t next = 0;
            for (size_t slot = 0; slot < 6 && ok; ++slot) {
                const u64 hash = Load<u64>(key, slot * 8);
                if (hash == 0) {
                    continue;
                }
                if (next >= record.envs.size()) {
                    ok = false;
                    break;
                }
                record.slots[slot] = static_cast<u8>(next);
                record.xors[slot] = hash ^ envs[record.envs[next]].hash;
                ++next;
            }
            const std::vector<u8> state(key.begin() + KeyHashesSize, key.end());
            record.state = states.try_emplace(state, static_cast<u32>(states.size())).first->second;
        }
        if (!ok) {
            ++report.records_dropped;
            continue;
        }
        records.push_back(std::move(record));
    }

    // Programs and fact sets that records use, renumbered.
    std::vector<s64> fact_of(env_count, -1);
    std::vector<s64> program_slot(programs.size(), -1);
    std::vector<u32> program_list, fact_list;
    for (auto& record : records) {
        for (u32& id : record.envs) {
            if (fact_of[id] < 0) {
                const u32 program = static_cast<u32>(envs[id].program);
                if (program_slot[program] < 0) {
                    program_slot[program] = static_cast<s64>(program_list.size());
                    program_list.push_back(program);
                }
                fact_of[id] = static_cast<s64>(fact_list.size());
                fact_list.push_back(id);
            }
            id = static_cast<u32>(fact_of[id]);
        }
    }

    Writer body;
    body.Put(Format);
    body.Put(header.title_id);
    body.Put(header.game_version);
    body.Put(header.cache_version);
    body.Put(static_cast<u32>(program_list.size()));
    for (const u32 p : program_list) {
        body.Put(programs[p].source->fingerprint);
        body.Put(static_cast<u8>(StageOf(programs[p].source->stage)));
        body.Zeros(3);
    }
    body.Put(static_cast<u32>(fact_list.size()));
    for (const u32 id : fact_list) {
        const auto& env = envs[id];
        body.Put(static_cast<u32>(program_slot[env.program]));
        body.Put(env.flags);
        body.Zeros(3);
        body.Put(static_cast<u32>(env.facts.size()));
        body.Put(env.facts);
    }
    std::vector<const std::vector<u8>*> state_list(states.size());
    for (const auto& [state, id] : states) {
        state_list[id] = &state;
    }
    body.Put(static_cast<u32>(state_list.size()));
    for (const auto* state : state_list) {
        body.Put(*state);
    }
    body.Put(static_cast<u32>(records.size()));
    for (const auto& record : records) {
        body.Put(record.kind);
        body.Put(static_cast<u8>(record.envs.size()));
        body.Zeros(2);
        for (const u32 id : record.envs) {
            body.Put(id);
        }
        if (record.kind == 0) {
            body.Put(record.state);
            body.Put(record.slots);
            const bool has_xor =
                std::any_of(record.xors.begin(), record.xors.end(), [](u64 x) { return x != 0; });
            body.Put(static_cast<u8>(has_xor));
            body.Zeros(1);
            if (has_xor) {
                body.Put(record.xors);
            }
        }
    }
    report.programs = static_cast<u32>(program_list.size());
    report.environments = static_cast<u32>(fact_list.size());
    report.records = static_cast<u32>(records.size());
    report.states = static_cast<u32>(state_list.size());

    // The last gate: no 32 bytes of any of the game's programs anywhere in the recipe.
    const auto matches = FindCodeInBody(body.bytes, extracted, threads);
    if (!matches.empty()) {
        throw std::runtime_error(fmt::format(
            "the recipe would hold {} pieces of the game's code (program {:016x} +{:#x}); not "
            "written",
            matches.size(), matches[0].fingerprint, matches[0].program_offset));
    }
    return Compress(body.bytes);
}

std::vector<u8> Assemble(std::span<const u8> recipe, const X::Result& extracted, u32 cache_version,
                         Report& report, unsigned threads, VariantHints* hints) {
    const auto body = Decompress(recipe);
    Reader r{body};
    if (r.Get<u32>() != Format) {
        throw std::runtime_error("precompile recipe of another format");
    }
    r.Get<u64>();
    r.Get<u32>();
    r.Get<u32>();

    std::unordered_map<u64, const X::Program*> by_fingerprint;
    for (const auto& p : extracted.programs) {
        by_fingerprint.try_emplace(p.fingerprint, &p);
    }
    const u32 program_count = r.Get<u32>();
    std::vector<Program> programs(program_count);
    std::vector<Stage> stages(program_count);
    for (u32 i = 0; i < program_count; ++i) {
        const u64 fingerprint = r.Get<u64>();
        stages[i] = static_cast<Stage>(r.Get<u8>());
        r.Take(3);
        const auto it = by_fingerprint.find(fingerprint);
        if (it == by_fingerprint.end()) {
            ++report.programs_missing;
            programs[i].error = fmt::format("{:016x} is not in the game", fingerprint);
        } else {
            programs[i].source = it->second;
        }
    }
    ParallelFor(program_count, threads, [&](size_t i) {
        if (programs[i].source) {
            programs[i].lowered =
                Lower(stages[i], programs[i].source->control, programs[i].source->code,
                      programs[i].error);
        }
    });
    for (const auto& p : programs) {
        report.programs_failed += p.source && !p.lowered ? 1 : 0;
    }

    // Environments: the facts with the program's code and header put back.
    const u32 fact_count = r.Get<u32>();
    struct Env {
        s64 program = -1;
        std::vector<u8> bytes;
        u64 hash{};
        u32 shared{};
        std::array<u32, 3> workgroup{};
    };
    std::vector<Env> envs(fact_count);
    for (u32 i = 0; i < fact_count; ++i) {
        const u32 program = r.Get<u32>();
        const u8 flags = r.Get<u8>();
        r.Take(3);
        const auto facts = r.Take(r.Get<u32>());
        if (program >= program_count || !programs[program].lowered ||
            (flags & ~MeshHeaderFromCode) != 0) {
            continue;
        }
        const auto& lowered = *programs[program].lowered;
        const auto parts = Parse(facts, false);
        Writer env;
        env.Put(static_cast<u64>(lowered.code.size() * sizeof(u64)));
        env.Put(facts.subspan(8, EnvHeaderSize - 8));
        PutRaw(env, lowered.code.data(), lowered.code.size() * sizeof(u64));
        env.Put(facts.subspan(EnvHeaderSize, parts.tables_end - EnvHeaderSize));
        const auto tail = facts.subspan(parts.tables_end);
        if (parts.stage == Stage::Compute) {
            envs[i].workgroup = Load<std::array<u32, 3>>(tail, 0);
            envs[i].shared = Load<u32>(tail, 12);
        } else {
            PutRaw(env, lowered.code.data(), SphSize);
        }
        env.Put(tail);
        if (flags & MeshHeaderFromCode) {
            const auto header = MeshHeader(*programs[program].source);
            if (!header) {
                continue;
            }
            env.Put(Bytes{header->data(), header->size()});
        }
        envs[i].program = program;
        envs[i].hash = lowered.hash;
        envs[i].bytes = std::move(env.bytes);
    }

    const u32 state_count = r.Get<u32>();
    std::vector<Bytes> states(state_count);
    for (auto& state : states) {
        state = r.Take(StateSize);
    }

    // Records, and the environments they use, renumbered.
    const u32 record_count = r.Get<u32>();
    Writer records;
    std::vector<s64> env_slot(fact_count, -1);
    std::vector<u32> env_list;
    u32 written = 0;
    std::vector<u32> written_from; // the recipe record of each record written
    for (u32 i = 0; i < record_count; ++i) {
        const u8 kind = r.Get<u8>();
        const u8 count = r.Get<u8>();
        r.Take(2);
        std::vector<u32> ids(count);
        bool ok = true;
        for (auto& id : ids) {
            id = r.Get<u32>();
            ok &= id < fact_count && envs[id].program >= 0;
        }
        u32 state = 0;
        std::array<u8, 6> slots{};
        std::array<u64, 6> xors{};
        if (kind == 0) {
            state = r.Get<u32>();
            slots = r.Get<std::array<u8, 6>>();
            const bool has_xor = r.Get<u8>() != 0;
            r.Take(1);
            if (has_xor) {
                xors = r.Get<std::array<u64, 6>>();
            }
            ok &= state < state_count;
        }
        if (!ok) {
            ++report.records_dropped;
            continue;
        }
        records.Put(kind);
        records.Put(count);
        records.Zeros(2);
        for (const u32 id : ids) {
            if (env_slot[id] < 0) {
                env_slot[id] = static_cast<s64>(env_list.size());
                env_list.push_back(id);
            }
            records.Put(static_cast<u32>(env_slot[id]));
        }
        if (kind == 1) {
            const auto& env = envs[ids[0]];
            records.Put(env.hash);
            records.Put(env.shared);
            records.Put(env.workgroup);
        } else {
            for (size_t slot = 0; slot < 6; ++slot) {
                records.Put(slots[slot] < ids.size() ? envs[ids[slots[slot]]].hash ^ xors[slot]
                                                     : u64{0});
            }
            records.Put(states[state]);
        }
        ++written;
        written_from.push_back(i);
    }
    if (hints) {
        // A recipe made before the sections ends here; a damaged section only loses the hints.
        *hints = {};
        Sections sections;
        try {
            sections = ReadSections(r, record_count);
        } catch (const std::exception&) {
        }
        if (sections.hints) {
            hints->sources = sections.hints->sources;
            hints->kinds.reserve(written_from.size());
            for (const u32 i : written_from) {
                hints->kinds.push_back(sections.hints->kinds[i]);
            }
        }
    }

    // NX2PREP1: header, environment index, records, environments.
    Writer out;
    out.Put(PrecompiledMagic.data(), PrecompiledMagic.size());
    out.Put(cache_version);
    out.Put(static_cast<u32>(env_list.size()));
    out.Put(written);
    out.Put(u32{0});
    u64 at = 24 + 16 * u64{env_list.size()} + records.bytes.size();
    for (const u32 id : env_list) {
        out.Put(at);
        out.Put(static_cast<u64>(envs[id].bytes.size()));
        at += envs[id].bytes.size();
    }
    out.Put(records.bytes);
    for (const u32 id : env_list) {
        out.Put(envs[id].bytes);
    }
    report.programs = program_count;
    report.environments = static_cast<u32>(env_list.size());
    report.records = written;
    report.states = state_count;
    for (const auto& p : programs) {
        if (!p.error.empty() && report.errors.size() < 64) {
            report.errors.push_back(p.error);
        }
    }
    return std::move(out.bytes);
}

std::optional<VariantHints> ReadVariantHints(std::span<const u8> recipe) {
    try {
        return ParseBody(Decompress(recipe)).sections.hints;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<VariantHints> HintsForFile(std::span<const u8> recipe, u32 file_record_count) {
    auto hints = ReadVariantHints(recipe);
    if (!hints || hints->sources == 0 || hints->kinds.size() != file_record_count) {
        return std::nullopt;
    }
    return hints;
}

std::vector<u8> Merge(std::span<const u8> recipe, const X::Result& extracted,
                      std::span<const MergeSource> caches, std::span<const MergeSource> occlusion,
                      MergeReport& report, unsigned threads) {
    const auto base_body = Decompress(recipe);
    RecipeData d = ParseBody(base_body);
    {
        // Written back, the recipe's own part must be the same bytes: what it holds is kept.
        const auto again = WriteBody(d, false);
        if (again.size() != d.records_end ||
            std::memcmp(again.data(), base_body.data(), again.size()) != 0) {
            throw std::runtime_error("the recipe does not read back as it was written");
        }
    }
    report.records_before = static_cast<u32>(d.records.size());
    report.programs_before = static_cast<u32>(d.programs.size());
    report.facts_before = static_cast<u32>(d.facts.size());
    report.states_before = static_cast<u32>(d.states.size());

    // Every extracted program lowered once, as MakeRecipe does: environments find their program
    // by the shader hash, the lowest fingerprint for programs lowering to the same code.
    std::vector<Program> programs(extracted.programs.size());
    ParallelFor(programs.size(), threads, [&](size_t i) {
        const auto& p = extracted.programs[i];
        programs[i].source = &p;
        programs[i].lowered = Lower(StageOf(p.stage), p.control, p.code, programs[i].error);
    });
    std::unordered_map<u64, u32> by_hash;
    std::unordered_map<u64, u32> by_fingerprint;
    for (u32 i = 0; i < programs.size(); ++i) {
        by_fingerprint.try_emplace(programs[i].source->fingerprint, i);
        if (programs[i].lowered) {
            const auto [it, added] = by_hash.try_emplace(programs[i].lowered->hash, i);
            if (!added &&
                programs[i].source->fingerprint < programs[it->second].source->fingerprint) {
                it->second = i;
            }
        }
    }

    // The recipe's programs: their shader hash in this game (0: not in it), lowered as Assemble
    // lowers them, for the stage the recipe names (which can be another than the extraction's).
    std::vector<u64> recipe_hash(d.programs.size(), 0);
    std::unordered_map<u64, u32> slot_of_hash;
    std::vector<u32> relowered; // recipe slots whose stage is not the extraction's
    for (u32 slot = 0; slot < d.programs.size(); ++slot) {
        const auto it = by_fingerprint.find(d.programs[slot].fingerprint);
        if (it != by_fingerprint.end() &&
            static_cast<u8>(StageOf(programs[it->second].source->stage)) != d.programs[slot].stage) {
            relowered.push_back(slot);
        }
    }
    const size_t extracted_count = programs.size();
    programs.resize(extracted_count + relowered.size());
    ParallelFor(relowered.size(), threads, [&](size_t i) {
        const auto& source = *programs[by_fingerprint.at(d.programs[relowered[i]].fingerprint)].source;
        auto& program = programs[extracted_count + i];
        program.source = &source;
        program.lowered = Lower(static_cast<Stage>(d.programs[relowered[i]].stage), source.control,
                                source.code, program.error);
    });
    for (size_t i = 0; i < relowered.size(); ++i) {
        const u32 index = static_cast<u32>(extracted_count + i);
        if (programs[index].lowered) {
            by_hash.try_emplace(programs[index].lowered->hash, index);
        }
    }
    for (u32 slot = 0, next_relowered = 0; slot < d.programs.size(); ++slot) {
        const auto it = by_fingerprint.find(d.programs[slot].fingerprint);
        u32 index = it == by_fingerprint.end() ? ~0U : it->second;
        if (next_relowered < relowered.size() && relowered[next_relowered] == slot) {
            index = static_cast<u32>(extracted_count + next_relowered++);
        }
        if (index == ~0U || !programs[index].lowered) {
            ++report.recipe_programs_missing;
            continue;
        }
        recipe_hash[slot] = programs[index].lowered->hash;
        slot_of_hash.try_emplace(recipe_hash[slot], slot);
    }

    // Fact set identities: the program's code (its hash), the flags, the facts in key order.
    std::unordered_map<std::string, u32> identity_ids;
    std::vector<s64> fact_of_identity; // the merged recipe's fact set of each identity
    const auto identity_of = [&](u64 hash, u8 flags, const std::vector<u8>& sorted) {
        std::string id(reinterpret_cast<const char*>(&hash), sizeof(hash));
        id.push_back(static_cast<char>(flags));
        id.append(reinterpret_cast<const char*>(sorted.data()), sorted.size());
        const auto [it, added] =
            identity_ids.try_emplace(std::move(id), static_cast<u32>(identity_ids.size()));
        if (added) {
            fact_of_identity.push_back(-1);
        }
        return it->second;
    };
    std::vector<s64> recipe_identity(d.facts.size(), -1);
    for (u32 i = 0; i < d.facts.size(); ++i) {
        const auto& f = d.facts[i];
        if (f.program >= d.programs.size() || recipe_hash[f.program] == 0) {
            continue;
        }
        const u32 id = identity_of(recipe_hash[f.program], f.flags, SortedFacts(f.bytes));
        if (fact_of_identity[id] < 0) {
            fact_of_identity[id] = i;
        }
        recipe_identity[i] = id;
    }

    // The records: by key and facts (what is in the recipe), by key (what hints attach to).
    const auto record_identity = [](char kind, const std::string& key, std::span<const u32> ids) {
        std::string id(1, kind);
        id += key;
        id.append(reinterpret_cast<const char*>(ids.data()), ids.size() * sizeof(u32));
        return id;
    };
    std::unordered_set<std::string> have;
    std::unordered_map<std::string, std::vector<u32>> by_key; // compute: compute_key
    const auto compute_key = [](u64 hash) {
        std::string key(1, 'c');
        key.append(reinterpret_cast<const char*>(&hash), sizeof(hash));
        return key;
    };
    std::map<std::pair<u32, u64>, u32> forms;
    for (u32 index = 0; index < d.records.size(); ++index) {
        const auto& rec = d.records[index];
        std::vector<u32> ids;
        bool ok = true;
        for (const u32 env : rec.envs) {
            ok = ok && env < d.facts.size() && recipe_identity[env] >= 0;
            if (ok) {
                ids.push_back(static_cast<u32>(recipe_identity[env]));
            }
        }
        if (!ok) {
            continue;
        }
        if (rec.kind == 1) {
            have.insert(record_identity('c', {}, ids));
            if (!rec.envs.empty()) {
                by_key[compute_key(recipe_hash[d.facts[rec.envs[0]].program])].push_back(index);
            }
            continue;
        }
        if (rec.state >= d.states.size()) {
            continue;
        }
        std::array<u64, 6> hashes{};
        for (size_t slot = 0; slot < 6 && ok; ++slot) {
            if (rec.slots[slot] == 0xff) {
                continue;
            }
            ok = rec.slots[slot] < rec.envs.size();
            if (ok) {
                hashes[slot] =
                    recipe_hash[d.facts[rec.envs[rec.slots[slot]]].program] ^ rec.xors[slot];
            }
        }
        if (!ok) {
            continue;
        }
        const Bytes state{d.states[rec.state]};
        auto key = NeutralKey(Bytes{reinterpret_cast<const u8*>(hashes.data()), KeyHashesSize},
                              state);
        have.insert(record_identity('g', key, ids));
        by_key[std::move(key)].push_back(index);
        ++forms[{Load<u32>(state, 0) & Raw1DeviceBits, Load<u64>(state, StateDriverOffset)}];
    }
    // The recipe's device form, the one most of its graphics records have: added states take its
    // driver fields (the preparer puts its own there) and, where only that differs, its logic op.
    u32 form_raw1 = 0;
    u64 form_driver = 0;
    u32 form_records = 0;
    for (const auto& [form, count] : forms) {
        if (count > form_records) {
            form_records = count;
            form_raw1 = form.first;
            form_driver = form.second;
        }
    }
    std::unordered_map<std::string, u32> state_ids;
    for (u32 i = 0; i < d.states.size(); ++i) {
        state_ids.try_emplace(
            std::string(reinterpret_cast<const char*>(d.states[i].data()), StateSize), i);
    }

    // Fact sets' answers, read when a key's records are compared.
    std::vector<std::unique_ptr<FactView>> views;
    const auto view_of = [&](u32 fact) -> const FactView& {
        if (views.size() <= fact) {
            views.resize(d.facts.size());
        }
        if (!views[fact]) {
            views[fact] = std::make_unique<FactView>(ViewOf(d.facts[fact].bytes));
        }
        return *views[fact];
    };

    // The caches' pipelines the recipe lacks, in file order.
    std::unordered_set<std::string> added;
    const auto note = [&report](std::string text) {
        if (report.errors.size() < 256) {
            report.errors.push_back(std::move(text));
        }
    };
    for (const auto& source : caches) {
        const Bytes bytes = source.bytes;
        if (bytes.size() < 12 || std::memcmp(bytes.data(), CacheMagic.data(), 8) != 0 ||
            Load<u32>(bytes, 8) != d.header.cache_version) {
            note(fmt::format("{}: not a pipeline cache of version {}", source.name,
                             d.header.cache_version));
            continue;
        }
        size_t at = 12;
        try {
            while (at < bytes.size()) {
                const u32 count = Load<u32>(bytes, at);
                at += 4;
                if (count == 0 || count > 6) {
                    throw std::runtime_error("bad record");
                }
                std::vector<Bytes> envs;
                for (u32 k = 0; k < count; ++k) {
                    const size_t size = EnvironmentSize(bytes.subspan(at));
                    envs.push_back(bytes.subspan(at, size));
                    at += size;
                }
                const bool compute = static_cast<Stage>(Load<u32>(envs[0], 72)) == Stage::Compute;
                const size_t key_size = compute ? ComputeKeySize : GraphicsKeySize;
                if (bytes.size() - at < key_size) {
                    throw std::runtime_error("truncated record");
                }
                const Bytes key = bytes.subspan(at, key_size);
                at += key_size;
                ++(compute ? report.cache_compute : report.cache_graphics);

                struct Pending {
                    u32 identity{};
                    u32 program{};
                    u8 flags{};
                    u64 hash{};
                    std::vector<u8> facts;
                };
                std::vector<Pending> pending;
                bool ok = !compute || count == 1;
                for (const Bytes env : envs) {
                    if (!ok) {
                        break;
                    }
                    auto made = FactsOf(env, programs, by_hash);
                    if (made.status != FactsStatus::Ok) {
                        note(fmt::format("{}: an entry at {:#x} left out: {} ({:016x})",
                                         source.name, at - key_size,
                                         made.status == FactsStatus::NoProgram
                                             ? std::string{"no program of the game has its code"}
                                         : made.status == FactsStatus::OtherCode
                                             ? std::string{"same hash, other code"}
                                             : made.why,
                                         made.hash));
                        ok = false;
                        break;
                    }
                    auto sorted = SortedFacts(made.facts);
                    const u32 id = identity_of(made.hash, made.flags, sorted);
                    pending.push_back({id, static_cast<u32>(made.program), made.flags, made.hash,
                                       std::move(sorted)});
                }
                if (!ok) {
                    ++report.not_expressible;
                    continue;
                }
                std::vector<u32> ids;
                for (const auto& p : pending) {
                    ids.push_back(p.identity);
                }
                const std::string neutral =
                    compute ? compute_key(pending[0].hash)
                            : NeutralKey(key.first(KeyHashesSize), key.subspan(KeyHashesSize));
                auto identity =
                    record_identity(compute ? 'c' : 'g', compute ? std::string{} : neutral, ids);
                if (have.contains(identity)) {
                    ++(added.contains(identity) ? report.duplicates : report.in_recipe);
                    continue;
                }
                // Other facts for a key the recipe has: added only when none of its records with
                // that key would serve the draw (texture handles differ between sessions where
                // the textures do not; such records would only prepare the same pipeline again).
                if (const auto same_key = by_key.find(neutral); same_key != by_key.end()) {
                    std::vector<FactView> live;
                    for (const auto& p : pending) {
                        live.push_back(ViewOf(p.facts));
                    }
                    const bool served = std::ranges::any_of(same_key->second, [&](u32 index) {
                        const auto& candidate = d.records[index];
                        if (candidate.envs.size() != live.size()) {
                            return false;
                        }
                        for (size_t i = 0; i < live.size(); ++i) {
                            if (!AnswersHold(view_of(candidate.envs[i]), live[i])) {
                                return false;
                            }
                        }
                        return true;
                    });
                    if (served) {
                        ++report.answered;
                        continue;
                    }
                }
                RecipeData::Rec rec;
                rec.kind = compute ? 1 : 0;
                if (!compute) {
                    rec.slots.fill(0xff);
                    size_t next = 0;
                    for (size_t slot = 0; slot < 6 && ok; ++slot) {
                        const u64 hash = Load<u64>(key, slot * 8);
                        if (hash == 0) {
                            continue;
                        }
                        ok = next < pending.size();
                        if (ok) {
                            rec.slots[slot] = static_cast<u8>(next);
                            rec.xors[slot] = hash ^ pending[next].hash;
                            ++next;
                        }
                    }
                    if (!ok || next != pending.size()) {
                        note(fmt::format("{}: an entry at {:#x} left out: its key and programs "
                                         "differ",
                                         source.name, at - key_size));
                        ++report.not_expressible;
                        continue;
                    }
                    std::vector<u8> state(key.begin() + KeyHashesSize, key.end());
                    std::memcpy(state.data() + StateDriverOffset, &form_driver, sizeof(u64));
                    u32 raw1 = Load<u32>(state, 0);
                    u32 dynamic = Load<u32>(state, StateDynamicOffset);
                    const u32 differ = (raw1 ^ form_raw1) & Raw1DeviceBits;
                    if (differ == Raw1LogicOpDynamic && !(dynamic & DynamicLogicOpEnable)) {
                        // Logic op off: the operation does not count, and the preparer turns
                        // the recipe's form into this device's (RecordedKeyForThisDevice).
                        raw1 = (raw1 & ~Raw1LogicOpDynamic) | (form_raw1 & Raw1LogicOpDynamic);
                        dynamic &= ~DynamicLogicOpMask;
                        std::memcpy(state.data(), &raw1, sizeof(raw1));
                        std::memcpy(state.data() + StateDynamicOffset, &dynamic, sizeof(dynamic));
                        ++report.converted_form;
                    } else if (differ != 0) {
                        // Other dynamic state: left as recorded, for devices like the one that
                        // recorded it (the preparer skips it elsewhere).
                        ++report.device_specific;
                    }
                    const auto [it, new_state] = state_ids.try_emplace(
                        std::string(reinterpret_cast<const char*>(state.data()), StateSize),
                        static_cast<u32>(d.states.size()));
                    if (new_state) {
                        d.states.push_back(std::move(state));
                    }
                    rec.state = it->second;
                }
                for (auto& p : pending) {
                    if (fact_of_identity[p.identity] < 0) {
                        u32 slot;
                        if (const auto it = slot_of_hash.find(p.hash); it != slot_of_hash.end()) {
                            slot = it->second;
                        } else {
                            slot = static_cast<u32>(d.programs.size());
                            const auto& program = *programs[p.program].source;
                            d.programs.push_back(
                                {program.fingerprint, static_cast<u8>(StageOf(program.stage))});
                            slot_of_hash.emplace(p.hash, slot);
                        }
                        fact_of_identity[p.identity] = static_cast<s64>(d.facts.size());
                        d.facts.push_back({slot, p.flags, std::move(p.facts)});
                    }
                    rec.envs.push_back(static_cast<u32>(fact_of_identity[p.identity]));
                }
                const u32 index = static_cast<u32>(d.records.size());
                auto& list = by_key[neutral];
                ++(list.empty() ? report.added_new_key : report.added_new_facts);
                list.push_back(index);
                ++(compute ? report.added_compute : report.added_graphics);
                have.insert(identity);
                added.insert(std::move(identity));
                d.records.push_back(std::move(rec));
            }
        } catch (const std::exception& e) {
            note(fmt::format("{}: {} at {:#x}; the rest is not read", source.name, e.what(), at));
        }
    }

    // Variant hints: the kinds each record's key was used with.
    VariantHints hints = d.sections.hints.value_or(VariantHints{});
    hints.kinds.resize(d.records.size(), 0);
    constexpr size_t OcclHeaderSize = 16;
    constexpr size_t OcclRecordHeaderSize = 17; // marker, type, payload size, payload hash
    for (const auto& source : occlusion) {
        const Bytes bytes = source.bytes;
        if (bytes.size() < OcclHeaderSize ||
            std::memcmp(bytes.data(), OcclMagic.data(), OcclMagic.size()) != 0 ||
            Load<u32>(bytes, 8) != d.header.cache_version ||
            Load<u32>(bytes, 12) != GraphicsKeySize) {
            note(fmt::format("{}: not an occlusion file of version {}", source.name,
                             d.header.cache_version));
            continue;
        }
        ++hints.sources;
        size_t at = OcclHeaderSize;
        while (bytes.size() - at >= OcclRecordHeaderSize) {
            const u32 marker = Load<u32>(bytes, at);
            const u8 type = Load<u8>(bytes, at + 4);
            const u32 size = Load<u32>(bytes, at + 5);
            const u64 hash = Load<u64>(bytes, at + 9);
            if (marker != OcclRecordMarker || size > bytes.size() - at - OcclRecordHeaderSize) {
                note(fmt::format("{}: a corrupt record at {:#x}; the rest is not read",
                                 source.name, at));
                break;
            }
            const Bytes payload = bytes.subspan(at + OcclRecordHeaderSize, size);
            if (Common::CityHash64(reinterpret_cast<const char*>(payload.data()), size) != hash) {
                note(fmt::format("{}: a corrupt record at {:#x}; the rest is not read",
                                 source.name, at));
                break;
            }
            at += OcclRecordHeaderSize + size;
            if (type == OcclRecordRefused) {
                // Its memo hash holds the build and the parent's descriptors: not the guest's.
                ++report.occl_refusals;
                continue;
            }
            if (type != OcclRecordVariant || payload.size() < 1 + 2 * GraphicsKeySize ||
                payload[0] >= VariantKindCount) {
                continue;
            }
            const u8 kind = payload[0];
            ++report.occl_variants;
            ++report.variants_by_kind[kind];
            const Bytes parent = payload.subspan(1, GraphicsKeySize);
            const auto it = by_key.find(
                NeutralKey(parent.first(KeyHashesSize), parent.subspan(KeyHashesSize)));
            if (it == by_key.end()) {
                ++report.occl_unmatched;
                continue;
            }
            for (const u32 index : it->second) {
                hints.kinds[index] = static_cast<u8>(hints.kinds[index] | (1U << kind));
            }
        }
    }
    if (hints.sources != 0) {
        for (const u8 mask : hints.kinds) {
            report.hinted_records += mask != 0 ? 1 : 0;
            for (u8 kind = 0; kind < VariantKindCount; ++kind) {
                report.hinted_by_kind[kind] += (mask >> kind) & 1;
            }
        }
        d.sections.hints = std::move(hints);
    }

    // What made it: the inputs by size and content hash, no paths or times.
    auto& text = d.sections.provenance;
    text += fmt::format("nx2-precompile merge 1: recipe body {} bytes {:016x} ({} records)",
                        base_body.size(),
                        Common::CityHash64(reinterpret_cast<const char*>(base_body.data()),
                                           base_body.size()),
                        report.records_before);
    for (const auto& source : caches) {
        text += fmt::format("; cache {} bytes {:016x}", source.bytes.size(),
                            Common::CityHash64(reinterpret_cast<const char*>(source.bytes.data()),
                                               source.bytes.size()));
    }
    for (const auto& source : occlusion) {
        text += fmt::format("; occlusion {} bytes {:016x}", source.bytes.size(),
                            Common::CityHash64(reinterpret_cast<const char*>(source.bytes.data()),
                                               source.bytes.size()));
    }
    text += fmt::format("; added {} graphics, {} compute\n", report.added_graphics,
                        report.added_compute);

    report.records_after = static_cast<u32>(d.records.size());
    report.programs_after = static_cast<u32>(d.programs.size());
    report.facts_after = static_cast<u32>(d.facts.size());
    report.states_after = static_cast<u32>(d.states.size());
    const auto body = WriteBody(d, true);
    // The last gate, as MakeRecipe's: no 32 bytes of any of the game's programs in the recipe.
    const auto matches = FindCodeInBody(body, extracted, threads);
    if (!matches.empty()) {
        throw std::runtime_error(fmt::format(
            "the merged recipe would hold {} pieces of the game's code (program {:016x} +{:#x}); "
            "not written",
            matches.size(), matches[0].fingerprint, matches[0].program_offset));
    }
    return Compress(body);
}

} // namespace VideoCommon::Nx2Recipe
