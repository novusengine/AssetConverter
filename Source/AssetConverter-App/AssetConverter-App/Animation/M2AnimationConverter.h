#pragma once

#include <Base/Types.h>

struct Runtime;

// Exports character skeletons and animations without replacing the world's PACT manifests.
class M2AnimationConverter
{
public:
    static i32 ExportCharacterAnimations(Runtime* runtime, const char* outputDirectory, const char* characterName = nullptr);
};
