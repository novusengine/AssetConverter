#pragma once

#include <Base/Types.h>

#include <filesystem>
#include <string>
#include <vector>

namespace NavMesh
{
    struct SeamValidationResult
    {
        u32 adjacentPairs = 0;
        u32 validatedPairs = 0;
        u32 skippedPairs = 0;
        u32 failedPairs = 0;
    };

    SeamValidationResult ValidateSeams(const std::filesystem::path& outputDirectory, const std::string& mapName, const std::vector<u32>& tileIDs);
}
