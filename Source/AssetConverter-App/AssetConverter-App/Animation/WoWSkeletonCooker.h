#pragma once
#include <FileFormat/Novus/Animation/Animation.h>
#include <FileFormat/Warcraft/M2/M2.h>
#include <Base/Memory/Bytebuffer.h>
#include <ostream>

// Rebuilds source hierarchy into a concrete neutral skeleton and retains cooker-only mappings.
class WoWSkeletonCooker
{
public:
    struct Result
    {
        FileFormat::Animation::SkeletonAsset asset;
        FileFormat::Animation::SkeletonData data;
        std::vector<u8> sourceToCooked;
        std::vector<i32> cookedToSource;
        std::vector<vec3> sourcePivots;
        std::vector<vec3> cookedPivots;
    };
    static Result Cook(const std::shared_ptr<Bytebuffer>& bytes, const M2::Layout& model, const std::vector<FileFormat::Animation::RigFamilyJoint>& family, std::ostream& report);
};
