#pragma once
#include <stdint.h>

// Optional FH3 Mod Platform interface, published by Car Meets. No DLL imports
// or game pointers are retained by a consumer. The provider owns membership;
// the lighting mod owns every light and its enabled state.
#define FH3_CAR_MEETS_INTERFACE "CarMeets.ParkedCars"
#define FH3_CAR_MEETS_INTERFACE_VERSION 1u
#define FH3_CAR_MEETS_SLOTS 12u

struct FH3MeetAppearance {
    uint32_t size;
    uint32_t meet;
    uint32_t slot;
    uint32_t reserved;
    uint64_t identity;
    float rgb[3];
};

struct FH3CarMeetsV1 {
    uint32_t size;
    // Call with a live car presentation and its world position. Returns 1
    // only for a currently occupied added-meet bay. Slot 0 is the player;
    // consumers must preserve that car's personal lighting (including none).
    // Repeat before rendering cached poses; zero means remove the meet glow.
    int (*lookup)(const void* presentation, const float position[3], FH3MeetAppearance* out);
};
