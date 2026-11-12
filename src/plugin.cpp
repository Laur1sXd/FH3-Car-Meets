#include "fh3mod.h"
#include "FH3CarMeets.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "embedded_payload.h"
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

struct Override {
    std::string path;
    std::string bytes;
};

struct Label {
    uint32_t key;
    std::wstring text;
};

struct alignas(16) Position {
    float x, y, z, w;
};

struct Matrix {
    Position right, up, forward, origin;
};

struct LocalPoint {
    float x, z;
};

struct ParkingSlot {
    LocalPoint car;
    float yaw;
    LocalPoint camera, panel;
};
static_assert(sizeof(ParkingSlot) == 7 * sizeof(float));

struct Site {
    Position origin;
    float cosine, sine;
    uint32_t id;

    Position world(LocalPoint local) const {
        return {origin.x + cosine * local.x + sine * local.z, origin.y,
                origin.z - sine * local.x + cosine * local.z, 1.0f};
    }
};

using GetTransform = Matrix* (*)(void*, Matrix*, void*);
using ComposeTransform = void (*)(void*, Matrix*, double*, float);
using CurrentScene = void* (*)();
using GroundCast = const void* (*)(void*, Position*, const Position*, unsigned char, float, float);
using ApplyHandbrake = void (*)(void*);
using SlotVisibility = void (*)(void*, bool);
using PlaceCarOnGround = void (*)(void*, const Position*, const Position*);
using PrepareProceduralModels = void (*)(std::byte*, int);

struct Grounding {
    std::byte* game = nullptr;
    GetTransform original_car = nullptr;
    GetTransform original_camera = nullptr;
    ComposeTransform original_cinematic = nullptr;
    ApplyHandbrake original_handbrake = nullptr;
    PrepareProceduralModels original_prepare_models = nullptr;
    const FH3ModApi* api = nullptr;
    FH3Mod* mod = nullptr;
    std::vector<Site> sites;
    std::array<ParkingSlot, 12> slots{};
    std::array<std::atomic<unsigned>, 32> logged_slots{};
    std::array<std::atomic<unsigned>, 32> logged_parking{};
    std::atomic<uint32_t> logged_entries = 0;
    std::atomic<uint32_t> logged_overviews = 0;
    std::atomic<uint32_t> logged_vegetation = 0;
    std::atomic<uint32_t> logged_spectators = 0;
};

Grounding grounding;
LocalPoint aligned_bay(size_t index);
const Site* new_meet_at(const Position& point, bool check_height = true);

struct ParkedMember {
    const std::byte* participant{};
    const std::byte* car{};
    const void* presentation{};
    uint32_t meet{};
    uint32_t slot{};
    uint64_t identity{};
};
SRWLOCK parked_lock = SRWLOCK_INIT;
std::array<ParkedMember, FH3_CAR_MEETS_SLOTS> parked_members{};
uint64_t parked_sequence{};
const uint64_t parked_seed = GetTickCount64() ^ reinterpret_cast<uintptr_t>(&parked_members);

template<class T> bool safe_read(const void* address, T& out) {
    SIZE_T bytes = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), address, &out, sizeof(out), &bytes) && bytes == sizeof(out);
}

const void* car_presentation(const std::byte* car) {
    const std::byte* presentation = nullptr;
    const std::byte* type = nullptr;
    if (!safe_read(car + 0x73a0, presentation) || !presentation || !safe_read(presentation, type)) return nullptr;
    if (type == grounding.game + 0x468b058 || type == grounding.game + 0x468b9b8) {
        if (!safe_read(presentation + 8, presentation) || !presentation || !safe_read(presentation, type)) return nullptr;
    }
    return type == grounding.game + 0x468e848 || type == grounding.game + 0x46902e0 ? presentation : nullptr;
}

void remember_parked_car(void* component) {
    const auto* slot = static_cast<const std::byte*>(component);
    uint32_t index = 0, state = 0;
    const std::byte* participant = nullptr;
    const std::byte* car = nullptr;
    if (!safe_read(slot + 0xf0, index) || index >= parked_members.size() ||
        !safe_read(slot + 0xc8, participant) || !participant ||
        !safe_read(participant + 0x50, state) || state != 3 ||
        !safe_read(participant + 0x58, car) || !car) return;
    const auto* presentation = static_cast<const std::byte*>(car_presentation(car));
    Position point{};
    void* physics = nullptr;
    if (!safe_read(car + 0x71e8, physics) || !physics) return;
    const auto* table = *reinterpret_cast<void***>(physics);
    const auto get_position = reinterpret_cast<const Position* (*)(void*, Position*)>(table[0x308 / 8]);
    point = *get_position(physics, &point);
    const auto* site = new_meet_at(point);
    if (!site) return;
    AcquireSRWLockExclusive(&parked_lock);
    auto& member = parked_members[index];
    if (member.participant != participant || member.car != car ||
        member.presentation != presentation || member.meet != site->id) {
        member = {participant, car, presentation, site->id, index, ++parked_sequence};
    }
    ReleaseSRWLockExclusive(&parked_lock);
}

int parked_appearance(const void* presentation, const float position[3], FH3MeetAppearance* out) {
    if (!presentation || !position || !out || out->size < sizeof(*out) ||
        !std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2])) return 0;
    const auto* site = new_meet_at({position[0], position[1], position[2], 1});
    if (!site) return 0;
    ParkedMember member{};
    AcquireSRWLockShared(&parked_lock);
    for (const auto& candidate : parked_members) {
        if (candidate.car && candidate.presentation == presentation) { member = candidate; break; }
    }
    if (!member.car) {
        for (const auto& candidate : parked_members) {
            if (candidate.car && car_presentation(candidate.car) == presentation) { member = candidate; break; }
        }
    }
    ReleaseSRWLockShared(&parked_lock);
    uint32_t state = 0;
    const std::byte* car = nullptr;
    if (!member.car || !safe_read(member.participant + 0x50, state) || state != 3 ||
        !safe_read(member.participant + 0x58, car) || car != member.car ||
        car_presentation(car) != presentation) return 0;
    if (member.presentation != presentation) {
        AcquireSRWLockExclusive(&parked_lock);
        auto& cached = parked_members[member.slot];
        if (cached.identity == member.identity) cached.presentation = presentation;
        ReleaseSRWLockExclusive(&parked_lock);
    }
    if (site->id != member.meet) return 0;
    const auto bay = site->world(aligned_bay(member.slot));
    const float dx = position[0] - bay.x, dz = position[2] - bay.z;
    if (dx * dx + dz * dz > 2.5f * 2.5f || std::abs(position[1] - bay.y) > 12) return 0;

    // Stable per occupied bay: changing the selected car never rerolls colors.
    uint64_t hash = parked_seed ^ (member.identity * 0x9e3779b97f4a7c15ull) ^
                    (static_cast<uint64_t>(member.meet) << 32) ^ member.slot;
    hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ull;
    hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebull;
    hash ^= hash >> 31;
    const float hue = static_cast<float>(hash % 36000) / 6000.0f;
    const int sector = static_cast<int>(hue);
    const float fraction = hue - sector;
    constexpr float low = 0.12f;
    const float rising = low + fraction * (1 - low), falling = 1 - fraction * (1 - low);
    constexpr size_t channels[6][3]{{0,1,2},{1,0,2},{1,2,0},{2,1,0},{2,0,1},{0,2,1}};
    const float values[3]{1, sector % 2 ? falling : rising, low};
    *out = {sizeof(*out), member.meet, member.slot, 0, member.identity, {0,0,0}};
    for (size_t i = 0; i < 3; ++i) out->rgb[channels[sector][i]] = values[i];
    return 1;
}

const FH3CarMeetsV1 parked_interface{sizeof(FH3CarMeetsV1), parked_appearance};

LocalPoint aligned_bay(size_t index) {
    const auto& first = grounding.slots.front().car;
    const auto& last = grounding.slots.back().car;
    const float fraction = static_cast<float>(index) / (grounding.slots.size() - 1);
    return {std::lerp(first.x, last.x, fraction), first.z};
}

void* call_slot(void* object, size_t offset) {
    if (!object) return nullptr;
    const auto* table = *reinterpret_cast<void***>(object);
    const auto method = reinterpret_cast<void* (*)(void*)>(table[offset / sizeof(void*)]);
    return method(object);
}

const Site* new_meet_at(const Position& point, bool check_height) {
    for (const auto& site : grounding.sites) {
        const float dx = point.x - site.origin.x;
        const float dz = point.z - site.origin.z;
        if (dx * dx + dz * dz < 145.0f * 145.0f &&
            (!check_height || std::abs(point.y - site.origin.y) < 32.0f)) {
            return &site;
        }
    }
    return nullptr;
}

void* current_collision() {
    if (!*reinterpret_cast<void* const*>(grounding.game + 0x4e5bb48)) return nullptr;
    const auto current_scene = reinterpret_cast<CurrentScene>(grounding.game + 0x93e4d0);
    auto* scene = static_cast<std::byte*>(current_scene());
    return scene ? call_slot(call_slot(scene + 0x10, 0x30), 0x170) : nullptr;
}

bool surface_height(void* collision, const Position& point, float& y) {
    if (!collision) return false;
    const auto cast = reinterpret_cast<GroundCast>(grounding.game + 0x1d3e3e0);
    Position surface{};
    for (const float range : {12.0f, 32.0f}) {
        if (cast(collision, &surface, &point, 8, range, 0.01f) &&
            std::isfinite(surface.y) && std::abs(surface.y - point.y) <= range) {
            y = surface.y;
            return true;
        }
    }
    return false;
}

std::string_view game_string(const std::byte* storage) {
    const auto size = *reinterpret_cast<const size_t*>(storage + 16);
    const auto capacity = *reinterpret_cast<const size_t*>(storage + 24);
    if (size == 0 || size > 128 || size > capacity) return {};
    const auto* data = capacity < 16 ? reinterpret_cast<const char*>(storage)
                                     : *reinterpret_cast<const char* const*>(storage);
    return data ? std::string_view(data, size) : std::string_view{};
}

bool in_vegetation_clearance(const Site& site, const Position& point) {
    const auto dx = point.x - site.origin.x;
    const auto dz = point.z - site.origin.z;
    const auto x = dx * site.cosine - dz * site.sine;
    const auto z = dx * site.sine + dz * site.cosine;
    // Covers the bays, the low lenses, the arrival replay, and the spectator groups.
    const float approach_edge = site.id == 104 ? -20.0f : -8.0f;
    return std::abs(point.y - site.origin.y) < 12.0f &&
           x >= -41 && x <= 41 && z >= approach_edge && z <= 32;
}

struct ModelInstance {
    uint16_t x, y, z, scale;
    std::byte other[24];
};
static_assert(sizeof(ModelInstance) == 0x20);

void clear_meet_vegetation(std::byte* data) {
    const auto* bounds = reinterpret_cast<const Position*>(data + 0xd0);
    const Site* nearby = nullptr;
    for (const auto& site : grounding.sites) {
        constexpr float reach = 60;
        if (bounds[0].x <= site.origin.x + reach && bounds[1].x >= site.origin.x - reach &&
            bounds[0].z <= site.origin.z + reach && bounds[1].z >= site.origin.z - reach) {
            nearby = &site;
            break;
        }
    }
    if (!nearby) return;

    const auto category = game_string(data);
    const bool soft_vegetation = category == "GRASSTEMPLATE" || category == "AUSTRALIA_BUSHES";
    if (!soft_vegetation && category != "AUSTRALIA_TREES") return;

    unsigned hidden = 0;
    // FH3 keeps external lookup tables with the original group/instance
    // ordinals. Never compact these vectors or rewrite their draw ranges:
    // doing so leaves stale references in the streaming scenery query.
    // Packed instances have a native zero-scale visibility test. Ordinary
    // models and procedural generators use a different representation.
    const auto* first = *reinterpret_cast<const std::byte* const*>(data + 0x88);
    const auto* end = *reinterpret_cast<const std::byte* const*>(data + 0x90);
    if (!first || end < first || (end - first) % 0x70 != 0 || end - first > 0x70 * 4096) return;
    for (const auto* group = first; group != end; group += 0x70) {
        auto* instances = *reinterpret_cast<ModelInstance* const*>(group);
        const auto* last = *reinterpret_cast<const ModelInstance* const*>(group + 8);
        if (!instances || last < instances || last - instances > 65536) continue;
        for (auto* instance = instances; instance != last; ++instance) {
            const Position point{
                std::lerp(bounds[0].x, bounds[1].x, instance->x / 65535.0f),
                std::lerp(bounds[0].y, bounds[1].y, instance->y / 65535.0f),
                std::lerp(bounds[0].z, bounds[1].z, instance->z / 65535.0f), 1.0f,
            };
            if (instance->scale && in_vegetation_clearance(*nearby, point)) {
                instance->scale = 0;
                ++hidden;
            }
        }
    }
    const auto mask = uint32_t{1} << (nearby->id - 101);
    if (hidden && !(grounding.logged_vegetation.fetch_or(mask, std::memory_order_relaxed) & mask)) {
        char line[100];
        std::snprintf(line, sizeof(line), "Meet %u packed plants hidden in parking and camera footprint.",
                      nearby->id);
        grounding.api->log(grounding.mod, line);
    }
}

void prepare_meet_scenery(std::byte* data, int quality) {
    grounding.original_prepare_models(data, quality);
    // Procedural generation fills the packed vectors. Apply the native hidden
    // scale after generation, before the caller builds render resources.
    clear_meet_vegetation(data);
}

float camera_height_offset(void* collision, Position point, float offset) {
    float surface = 0.0f;
    if (surface_height(collision, point, surface)) {
        constexpr float lens_clearance = 0.38f;
        const float clearance_offset = surface + lens_clearance - point.y;
        if (clearance_offset > offset) return clearance_offset;
    }
    return offset;
}

int recording_slot(const std::byte* object) {
    // The game's MSVC string holds the recording basename at offset 0x600.
    auto file = game_string(object + 0x600);
    const auto slash = file.find_last_of("/\\");
    if (slash != std::string_view::npos) file.remove_prefix(slash + 1);
    for (const std::string_view prefix : {"CarMeet_Entrance_", "CarMeet_Exit_", "CarMeet_Intro_"}) {
        if (file.size() >= prefix.size() + 6 && _strnicmp(file.data(), prefix.data(), prefix.size()) == 0) {
            const auto a = file[prefix.size()];
            const auto b = file[prefix.size() + 1];
            if (a < '0' || a > '1' || b < '0' || b > '9') return -1;
            const int slot = (a - '0') * 10 + b - '0' - 1;
            return slot >= 0 && slot < 12 ? slot : -1;
        }
    }
    return -1;
}

Matrix* ground_car_transform(void* object, Matrix* output, void* extra) {
    Matrix* result = grounding.original_car(object, output, extra);
    const auto* table = *reinterpret_cast<const std::byte**>(object);
    if (table != grounding.game + 0x4631330 && table != grounding.game + 0x4712038) return result;
    const int slot = recording_slot(static_cast<const std::byte*>(object));
    if (slot < 0) return result;
    auto& origin = result->origin;
    const auto* site = new_meet_at(origin);
    if (!site) return result;

    const auto original_bay = site->world(grounding.slots[slot].car);
    const auto parking = site->world(aligned_bay(slot));
    origin.x += parking.x - original_bay.x;
    origin.z += parking.z - original_bay.z;

    // The Rainforest apron has a short approach from the road. Keep FH3's
    // arrival motion and attached camera within that approach and finish at
    // the unchanged parking anchor, rather than driving across the hamlet.
    if (site->id == 104 &&
        game_string(static_cast<const std::byte*>(object) + 0x600).find("Entrance") != std::string_view::npos) {
        const auto bay = parking;
        origin.x = bay.x + (origin.x - bay.x) * 0.2f;
        origin.z = bay.z + (origin.z - bay.z) * 0.2f;
    }

    // The replay retains the vertical offset of its starting transform.
    // Sample its parking position so it ends at the correct height.
    float ground = 0.0f;
    if (surface_height(current_collision(), parking, ground)) {
        origin.y += ground - parking.y;
        const unsigned mask = 1u << slot;
        if (!(grounding.logged_slots[site->id - 101].fetch_or(mask, std::memory_order_relaxed) & mask)) {
            char line[128];
            std::snprintf(line, sizeof(line), "Meet %u slot %d at %.2f, %.2f (surface %.2f).",
                          site->id, slot + 1, parking.x, parking.z, ground);
            grounding.api->log(grounding.mod, line);
        }
    }
    return result;
}

void log_camera(const Site& site, std::atomic<uint32_t>& logged, const char* view) {
    const auto mask = uint32_t{1} << (site.id - 101);
    if (!(logged.fetch_or(mask, std::memory_order_relaxed) & mask)) {
        char line[96];
        std::snprintf(line, sizeof(line), "Meet %u %s camera aligned.", site.id, view);
        grounding.api->log(grounding.mod, line);
    }
}

void log_parking_surface(const Site& site, void* collision) {
    unsigned samples = 0;
    float low = INFINITY;
    float high = -INFINITY;
    for (size_t index = 0; index < grounding.slots.size(); ++index) {
        const auto bay = aligned_bay(index);
        // Sample the full wheelbase in each bay as well as its center.
        for (const float x : {-1.0f, 0.0f, 1.0f}) {
            for (const float z : {-1.8f, 0.0f, 1.8f}) {
                float height = 0.0f;
                const auto point = site.world({bay.x + x, bay.z + z});
                if (!surface_height(collision, point, height)) continue;
                low = std::min(low, height);
                high = std::max(high, height);
                ++samples;
            }
        }
    }
    char line[140];
    if (samples) {
        std::snprintf(line, sizeof(line), "Meet %u parking survey %u/108 points, %.2f to %.2f m (span %.2f m).",
                      site.id, samples, low, high, high - low);
    } else {
        std::snprintf(line, sizeof(line), "Meet %u parking survey found no loaded surface.", site.id);
    }
    grounding.api->log(grounding.mod, line);
}

Position align_ui_point(Position& point, LocalPoint ParkingSlot::* member) {
    const auto* site = new_meet_at(point);
    if (!site) return {};

    for (size_t index = 0; index < grounding.slots.size(); ++index) {
        const auto& slot = grounding.slots[index];
        const auto expected = site->world(slot.*member);
        if (std::abs(point.x - expected.x) > 0.10f || std::abs(point.z - expected.z) > 0.10f ||
            std::abs(point.y - expected.y) > 3.0f) continue;
        const auto original = site->world(slot.car);
        const auto parking = site->world(aligned_bay(index));
        const float dx = parking.x - original.x;
        const float dz = parking.z - original.z;
        point.x += dx;
        point.z += dz;
        auto* collision = current_collision();
        float ground = parking.y;
        const bool grounded = surface_height(collision, parking, ground);
        float height_offset = ground - parking.y;
        if (member == &ParkingSlot::camera) {
            const auto cleared_offset = camera_height_offset(collision, point, height_offset);
            if (grounded || cleared_offset > height_offset) {
                log_camera(*site, grounding.logged_overviews, "overview");
            }
            height_offset = cleared_offset;
        }
        point.y += height_offset;
        return {dx, height_offset, dz, 0.0f};
    }
    return {};
}

Matrix* align_overview_camera(void* object, Matrix* output, void* extra) {
    auto* result = grounding.original_camera(object, output, extra);
    align_ui_point(result->origin, &ParkingSlot::camera);
    return result;
}

void align_cinematic(void* object, Matrix* output, double* precise_origin, float time) {
    grounding.original_cinematic(object, output, precise_origin, time);
    const auto* table = *reinterpret_cast<const std::byte**>(object);
    auto& point = output->origin;
    if (table == grounding.game + 0x4711cf0) {
        const auto offset = align_ui_point(point, &ParkingSlot::panel);
        precise_origin[0] += offset.x;
        precise_origin[1] += offset.y;
        precise_origin[2] += offset.z;
    } else if (table == grounding.game + 0x470ffa8) {
        const auto* site = new_meet_at(point);
        if (!site) return;
        // The Airfield intro camera and target share the stock slot-one anchor.
        const auto& player_slot = grounding.slots.front();
        const auto parking = site->world(player_slot.car);
        constexpr float anchor_height = 0.0483832f;
        if (std::abs(point.x - parking.x) > 0.10f || std::abs(point.z - parking.z) > 0.10f ||
            std::abs(point.y - parking.y - anchor_height) > 0.005f) return;
        auto* collision = current_collision();
        float ground = parking.y;
        const bool grounded = surface_height(collision, parking, ground);
        const float parking_offset = ground - parking.y;
        // The stock entry lens sweeps from its first keyframe to the parked view.
        auto first_lens = site->world({player_slot.car.x - 3.22332f, player_slot.car.z - 5.04948f});
        auto last_lens = site->world(player_slot.camera);
        first_lens.y = last_lens.y = point.y + 0.51662f;
        const auto start_offset = camera_height_offset(collision, first_lens, parking_offset);
        const auto height_offset = camera_height_offset(collision, last_lens, start_offset);
        // Child cameras and targets use the precise world origin as well as the matrix.
        point.y += height_offset;
        precise_origin[1] += height_offset;
        if (grounded || height_offset > parking_offset) {
            const auto mask = uint32_t{1} << (site->id - 101);
            const bool first_entry = !(grounding.logged_entries.load(std::memory_order_relaxed) & mask);
            log_camera(*site, grounding.logged_entries, "Airfield entry");
            if (first_entry) log_parking_surface(*site, collision);
        }
    } else {
        const auto* site = new_meet_at(point);
        if (!site) return;
        // These are the root anchors shared by FH3's four native spectator pairs.
        constexpr LocalPoint crowd_points[]{
            {-21.3346f, 8.29159f}, {-9.95022f, 10.9063f},
            {-0.474383f, 7.50513f}, {22.1113f, 7.32451f},
        };
        for (const auto local : crowd_points) {
            const auto anchor = site->world(local);
            if (std::abs(point.x - anchor.x) > 0.02f || std::abs(point.z - anchor.z) > 0.02f ||
                std::abs(point.y - anchor.y) > 0.02f) continue;
            float surface = 0.0f;
            if (surface_height(current_collision(), anchor, surface)) {
                const float offset = surface - anchor.y;
                point.y += offset;
                precise_origin[1] += offset;
                const auto mask = uint32_t{1} << (site->id - 101);
                if (!(grounding.logged_spectators.fetch_or(mask, std::memory_order_relaxed) & mask)) {
                    grounding.api->log(grounding.mod, "Native Car Meet spectators aligned to the parking surface.");
                }
            }
            return;
        }
    }
}

bool align_slot_parking(void* component) {
    const auto* slot = static_cast<const std::byte*>(component);
    const auto index = *reinterpret_cast<const uint32_t*>(slot + 0xf0);
    // Slot zero is the player, whose entry shot controls the car until the handoff.
    if (index == 0 || index >= grounding.slots.size()) return false;
    const auto* participant = *reinterpret_cast<const std::byte* const*>(slot + 0xc8);
    if (!participant || *reinterpret_cast<const uint32_t*>(participant + 0x50) != 3) return false;
    auto* car = *reinterpret_cast<std::byte* const*>(participant + 0x58);
    if (!car) return false;
    auto* physics = *reinterpret_cast<void* const*>(car + 0x71e8);
    if (!physics) return false;
    const auto* table = *reinterpret_cast<void***>(physics);
    const auto get_position = reinterpret_cast<const Position* (*)(void*, Position*)>(table[0x308 / 8]);
    Position actual{};
    actual = *get_position(physics, &actual);
    const auto* site = new_meet_at(actual, false);
    if (!site) return false;

    auto parked = site->world(aligned_bay(index));
    float ground = 0.0f;
    if (surface_height(current_collision(), parked, ground)) {
        // All parked cars use the player bay's heading. FH3 settles each car's
        // wheels against the ground at its own bay.
        parked.y = ground;
        const float yaw = grounding.slots.front().yaw;
        const float c = std::cos(yaw), s = std::sin(yaw);
        const float right = site->cosine * c - site->sine * s;
        const float forward = site->sine * c + site->cosine * s;
        const std::array<Position, 3> axes{{
            {right, 0, -forward, 0}, {0, 1, 0, 0}, {forward, 0, right, 0},
        }};
        const auto place_car = reinterpret_cast<PlaceCarOnGround>(grounding.game + 0x18a7190);
        place_car(car, &parked, axes.data());
        const unsigned mask = 1u << index;
        if (!(grounding.logged_parking[site->id - 101].fetch_or(mask, std::memory_order_relaxed) & mask)) {
            Position settled{};
            settled = *get_position(physics, &settled);
            char line[160];
            std::snprintf(line, sizeof(line), "Meet %u slot %u parked in row (X/Z error %.2f, %.2f; settled height %.2f).",
                          site->id, index + 1, settled.x - parked.x, settled.z - parked.z, settled.y - ground);
            grounding.api->log(grounding.mod, line);
        }
    }
    return true;
}

void park_and_show_car(void* component) {
    const bool is_added_meet = align_slot_parking(component);
    grounding.original_handbrake(component);
    remember_parked_car(component);
    if (!is_added_meet) return;
    // These are the slot's native visibility controls and use its existing
    // lifetime, so leaving a meet releases the visibility override.
    reinterpret_cast<SlotVisibility>(grounding.game + 0x5b89f0)(component, true);
    reinterpret_cast<SlotVisibility>(grounding.game + 0x5c6f90)(component, true);
}

DWORD WINAPI install_grounding(void*) {
    const auto* api = grounding.api;
    auto* mod = grounding.mod;
    auto* base = static_cast<std::byte*>(api->wait_for_code(600000));
    constexpr unsigned char transform_prologue[]{0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x81};
    constexpr unsigned char compose_prologue[]{0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10};
    constexpr unsigned char camera_call[]{0xe8, 0xb6, 0x05, 0x5b, 0x01};
    constexpr unsigned char handbrake_prologue[]{0x48, 0x8b, 0x89, 0xc8, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9};
    constexpr unsigned char force_visible_prologue[]{0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56};
    constexpr unsigned char show_car_prologue[]{0x48, 0x8b, 0xc4, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56};
    constexpr unsigned char place_car_prologue[]{0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10};
    constexpr unsigned char cast_prologue[]{0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0xb8, 0x80, 0xfa, 0x00, 0x00};
    constexpr unsigned char scene_prologue[]{0x48, 0x8b, 0x0d, 0x71, 0xd6, 0x51, 0x04};
    if (!base ||
        std::memcmp(base + 0x224ace0, transform_prologue, sizeof(transform_prologue)) != 0 ||
        std::memcmp(base + 0x22443a0, compose_prologue, sizeof(compose_prologue)) != 0 ||
        std::memcmp(base + 0xc9a725, camera_call, sizeof(camera_call)) != 0 ||
        std::memcmp(base + 0x5b2170, handbrake_prologue, sizeof(handbrake_prologue)) != 0 ||
        std::memcmp(base + 0x5b89f0, force_visible_prologue, sizeof(force_visible_prologue)) != 0 ||
        std::memcmp(base + 0x5c6f90, show_car_prologue, sizeof(show_car_prologue)) != 0 ||
        std::memcmp(base + 0x18a7190, place_car_prologue, sizeof(place_car_prologue)) != 0 ||
        std::memcmp(base + 0x1d3e3e0, cast_prologue, sizeof(cast_prologue)) != 0 ||
        std::memcmp(base + 0x93e4d0, scene_prologue, sizeof(scene_prologue)) != 0) {
        api->log(mod, "Ground alignment is unavailable on this game build.");
        return 0;
    }
    grounding.game = base;
    void* original_car = nullptr;
    void* original_camera = nullptr;
    void* original_cinematic = nullptr;
    void* original_handbrake = nullptr;
    auto* car = api->hook_function(mod, "Car Meet surface alignment", base + 0x224ace0,
                                   reinterpret_cast<void*>(ground_car_transform), &original_car);
    auto* camera = api->hook_call(mod, "Car Meet overview camera alignment", base + 0xc9a725,
                                 reinterpret_cast<void*>(align_overview_camera), &original_camera);
    auto* cinematic = api->hook_function(mod, "Car Meet entry and label alignment", base + 0x22443a0,
                                         reinterpret_cast<void*>(align_cinematic), &original_cinematic);
    auto* handbrake = api->hook_function(mod, "Car Meet parking and visibility", base + 0x5b2170,
                                         reinterpret_cast<void*>(park_and_show_car), &original_handbrake);
    grounding.original_car = reinterpret_cast<GetTransform>(original_car);
    grounding.original_camera = reinterpret_cast<GetTransform>(original_camera);
    grounding.original_cinematic = reinterpret_cast<ComposeTransform>(original_cinematic);
    grounding.original_handbrake = reinterpret_cast<ApplyHandbrake>(original_handbrake);
    if (car && camera && cinematic && handbrake && api->enable_hooks(mod, nullptr)) {
        api->log(mod, "Ground, camera, and parked-car visibility hooks installed.");
    } else {
        api->log(mod, "Could not enable ground alignment. Check for a hook conflict.");
    }
    constexpr unsigned char prepare_models_prologue[]{0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4c, 0x24, 0x08};
    if (std::memcmp(base + 0x23314f0, prepare_models_prologue, sizeof(prepare_models_prologue)) == 0) {
        void* original_prepare_models = nullptr;
        auto* scenery = api->hook_function(mod, "Car Meet vegetation clearance", base + 0x23314f0,
                                          reinterpret_cast<void*>(prepare_meet_scenery), &original_prepare_models);
        grounding.original_prepare_models = reinterpret_cast<PrepareProceduralModels>(original_prepare_models);
        if (scenery && api->enable_hooks(mod, scenery)) {
            api->log(mod, "Car Meet vegetation clearance hook installed.");
            return 0;
        }
    }
    api->log(mod, "Vegetation clearance is unavailable on this game build or a hook conflicts.");
    return 0;
}

std::string read_bytes(std::istream& input, size_t count) {
    std::string bytes(count, '\0');
    if (!input.read(bytes.data(), static_cast<std::streamsize>(count))) {
        throw std::runtime_error("The install data is incomplete. Run setup again.");
    }
    return bytes;
}

uint32_t read_number(std::istream& input) {
    uint32_t value = 0;
    if (!input.read(reinterpret_cast<char*>(&value), sizeof(value))) {
        throw std::runtime_error("The install data is incomplete. Run setup again.");
    }
    return value;
}

std::wstring read_label(std::istream& input) {
    const auto count = read_number(input);
    if (count == 0 || count > 128) {
        throw std::runtime_error("The install data contains an invalid map label.");
    }
    const auto bytes = read_bytes(input, count);
    const int length = static_cast<int>(count);
    const int wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), length, nullptr, 0);
    if (wide_count == 0) {
        throw std::runtime_error("A map label is not valid UTF-8.");
    }
    std::wstring text(static_cast<size_t>(wide_count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), length, text.data(), wide_count);
    return text;
}

bool valid_path(const std::string& path) {
    if (path.find("..") != std::string::npos ||
        path.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_/.") != std::string::npos) {
        return false;
    }
    return path == "stateflow/car_meet.xml" ||
           path == "stateflow/car_meet_slot.xml" ||
           path == "EntityModel/Entities/Australia/cml_car_meets.xml" ||
           path == "EntityModel/Entities/Australia/entities_offline_freeroam.xml" ||
           path.starts_with("EntityModel/Entities/Australia/entities_route_offline_car_meet_cml_") ||
           path.starts_with("Cinematics/Global/CarMeet/cml_") ||
           path.starts_with("tracks/australia/trackroutes/route") ||
           path == "tracks/australia/triggerzones/tz_cml/car_meet_location_triggers.tz";
}

void apply_embedded_payload(const FH3ModApi* api, FH3Mod* mod) {
    std::istringstream input(std::string(
        reinterpret_cast<const char*>(car_meets_payload), sizeof(car_meets_payload)),
        std::ios::in | std::ios::binary);
    if (read_bytes(input, 4) != "CML9") {
        throw std::runtime_error("The embedded Car Meets data is invalid.");
    }

    const auto slot_data = read_bytes(input, sizeof(grounding.slots));
    std::memcpy(grounding.slots.data(), slot_data.data(), slot_data.size());
    const uint32_t sites = read_number(input);
    if (sites == 0 || sites > 32) {
        throw std::runtime_error("The install data contains an invalid number of locations.");
    }
    std::vector<Label> labels;
    std::vector<Site> locations;
    std::unordered_set<uint32_t> ids;
    std::string sql = "INSERT INTO CarMeets (Id, RoadTripLocationId) VALUES ";
    for (uint32_t index = 0; index < sites; ++index) {
        const uint32_t id = read_number(input);
        if (id < 101 || id > 132 || !ids.insert(id).second) {
            throw std::runtime_error("The install data contains an invalid meet ID.");
        }
        Position point{};
        const auto bytes = read_bytes(input, sizeof(point));
        std::memcpy(&point, bytes.data(), bytes.size());
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) ||
            !std::isfinite(point.w)) {
            throw std::runtime_error("The install data contains an invalid lineup position.");
        }
        const auto radians = point.w * std::numbers::pi_v<float> / 180.0f;
        point.w = 1.0f;
        locations.push_back({point, std::cos(radians), std::sin(radians), id});
        if (index != 0) sql += ",";
        sql += "(" + std::to_string(id) + ",9)";
        for (int label_index = 0; label_index < 2; ++label_index) {
            const uint32_t hash = read_number(input);
            labels.push_back({hash, read_label(input)});
        }
    }
    sql += ";";

    std::vector<Override> files;
    std::unordered_set<std::string> paths;
    const uint32_t file_count = read_number(input);
    if (file_count != 7 + 2 * sites) {
        throw std::runtime_error("The install data has the wrong file count.");
    }
    for (uint32_t index = 0; index < file_count; ++index) {
        const uint32_t path_length = read_number(input);
        const uint32_t content_length = read_number(input);
        if (path_length == 0 || path_length > 200 || content_length == 0 || content_length > 2 * 1024 * 1024) {
            throw std::runtime_error("The install data contains an invalid override.");
        }
        auto path = read_bytes(input, path_length);
        if (!valid_path(path) || !paths.insert(path).second) {
            throw std::runtime_error("The install data contains an invalid or duplicate path.");
        }
        files.push_back({std::move(path), read_bytes(input, content_length)});
    }
    if (input.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("The install data contains unexpected trailing bytes.");
    }

    for (const auto& file : files) {
        if (!api->set_file(mod, file.path.c_str(), file.bytes.data(), file.bytes.size())) {
            api->clear_file(mod, nullptr);
            throw std::runtime_error("Could not register " + file.path + ". Check the platform log for a file conflict.");
        }
    }
    for (const auto& label : labels) {
        api->add_string_key(mod, "CarMeets.str", label.key, label.text.c_str());
    }
    api->add_sql(mod, sql.c_str());
    grounding.api = api;
    grounding.mod = mod;
    grounding.sites = std::move(locations);
    if (api->publish_interface) {
        api->publish_interface(mod, FH3_CAR_MEETS_INTERFACE, FH3_CAR_MEETS_INTERFACE_VERSION, &parked_interface);
    }
    const auto thread = CreateThread(nullptr, 0, install_grounding, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else api->log(mod, "Could not start the ground alignment setup thread.");
    const auto message = "Registered " + std::to_string(sites) + " added offline Car Meets.";
    api->log(mod, message.c_str());
}

} // namespace

FH3MOD_EXPORT int fh3mod_init(const FH3ModApi* api) {
    if (!api || api->api_version < 10 ||
        api->size < offsetof(FH3ModApi, add_string_key) + sizeof(api->add_string_key)) {
        return 0;
    }
    constexpr FH3ModInfo info{
        sizeof(FH3ModInfo), "Car Meets", "1.0.0", "Laur1sXd/FH3-Car-Meets", "1.3.0", 1,
        "Adds offline meets across Australia using the native lineup and menus.", 1
    };
    auto* mod = api->register_mod(&info);
    if (!mod) return 0;
    try {
        apply_embedded_payload(api, mod);
    } catch (const std::exception& error) {
        api->log(mod, error.what());
    }
    return 0;
}
