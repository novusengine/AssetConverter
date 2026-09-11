#pragma once
#include "M2SkeletonCooker.h"

// Samples source animation only during cooking; runtime clips contain uniform local TRS.
class M2ClipCooker
{
public:
    struct Result
    {
        FileFormat::Animation::AnimationClipAsset asset;
        FileFormat::Animation::AnimationClipData data;
        FileFormat::Animation::Bounds bounds;
    };
    static std::vector<FileFormat::Animation::LocalTransform> Sample(const std::shared_ptr<Bytebuffer>& bytes, const M2::Layout& model, const M2SkeletonCooker::Result& skeleton, u32 sequence, double milliseconds);
    static Result Cook(const std::shared_ptr<Bytebuffer>& bytes, const M2::Layout& model, const M2SkeletonCooker::Result& skeleton, u32 sequence, u64 skeletonAssetID, u16 rate, std::ostream& report, bool preservePeriod = false);
};
