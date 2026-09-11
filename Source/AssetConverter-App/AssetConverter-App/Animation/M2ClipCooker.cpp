#include "M2ClipCooker.h"
#include <FileFormat/Shared.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <xxhash/xxhash64.h>

namespace
{
    namespace A = FileFormat::Animation;
    template <typename T> const T& At(const std::shared_ptr<Bytebuffer>& bytes, const M2::M2Array<T>& array, u32 index)
    {
        if (index >= array.size || array.offset > bytes->writtenData || array.size > (bytes->writtenData - array.offset) / sizeof(T)) throw std::runtime_error("Source animation array out of bounds");
        return *array.GetElement(bytes, index);
    }
    vec3 Decode(vec3 value) { return value; }
    quat Decode(M2::M2CompressedQuaternion value)
    {
        const quat q = value.ToQuat();
        if (glm::length2(q) < 0.000001f) throw std::runtime_error("Invalid source rotation");
        return glm::normalize(q);
    }
    vec3 Interpolate(vec3 first, vec3 second, f32 alpha) { return glm::mix(first, second, alpha); }
    quat Interpolate(quat first, quat second, f32 alpha) { return glm::normalize(glm::slerp(first, glm::dot(first, second) < 0 ? -second : second, alpha)); }

    template <typename T, typename V> V Track(const std::shared_ptr<Bytebuffer>& bytes, const M2::Layout& model, const M2::M2Track<T>& track, u32 sequence, double time, V fallback)
    {
        if (!track.timestamps.size || !track.values.size) return fallback;
        if (track.interpolationType > 1) throw std::runtime_error("Unsupported source interpolation (only step and linear accepted)");
        u32 slot = sequence;
        if (track.globalSequence >= 0)
        {
            const u32 duration = At(bytes, model.md21.globalLoops, track.globalSequence);
            slot = 0;
            time = duration ? std::fmod(time, static_cast<double>(duration)) : 0;
        }
        if (slot >= track.timestamps.size || slot >= track.values.size) return fallback;
        const auto& times = At(bytes, track.timestamps, slot);
        const auto& values = At(bytes, track.values, slot);
        if (!times.size && !values.size) return fallback;
        if (!times.size || times.size != values.size) throw std::runtime_error("Source track timestamp/value count mismatch");
        u32 low = 0, high = times.size;
        while (low < high)
        {
            const u32 middle = low + (high - low) / 2;
            if (At(bytes, times, middle) <= time) low = middle + 1; else high = middle;
        }
        const u32 first = low ? low - 1 : 0;
        if (!track.interpolationType || first + 1 >= times.size || time <= At(bytes, times, first)) return Decode(At(bytes, values, first));
        const u32 t0 = At(bytes, times, first), t1 = At(bytes, times, first + 1);
        if (t1 <= t0) throw std::runtime_error("Non-monotonic source timestamps");
        const f32 alpha = static_cast<f32>((time - t0) / (t1 - t0));
        return Interpolate(Decode(At(bytes, values, first)), Decode(At(bytes, values, first + 1)), alpha);
    }
    A::LocalTransform Decompose(const mat4x4& matrix)
    {
        A::LocalTransform result;
        result.translation = vec3(matrix[3]);
        result.scale = vec3(glm::length(vec3(matrix[0])), glm::length(vec3(matrix[1])), glm::length(vec3(matrix[2])));
        if (glm::any(glm::lessThan(result.scale, vec3(0.000001f)))) throw std::runtime_error("Singular source joint transform");
        const mat3x3 rotation(vec3(matrix[0]) / result.scale.x, vec3(matrix[1]) / result.scale.y, vec3(matrix[2]) / result.scale.z);
        result.rotation = glm::normalize(glm::quat_cast(rotation));
        return result;
    }
}

std::vector<FileFormat::Animation::LocalTransform> M2ClipCooker::Sample(const std::shared_ptr<Bytebuffer>& bytes, const M2::Layout& model, const M2SkeletonCooker::Result& skeleton, u32 sequence, double milliseconds)
{
    const u32 count = model.md21.bones.size;
    std::vector<mat4x4> skin(count), original(count), cooked(skeleton.data.joints.size());
    const mat3x3 basis(CoordinateSpaces::ModelPosToNovus(vec3(1,0,0)), CoordinateSpaces::ModelPosToNovus(vec3(0,1,0)), CoordinateSpaces::ModelPosToNovus(vec3(0,0,1)));
    for (u32 i = 0; i < count; ++i)
    {
        const auto& bone = At(bytes, model.md21.bones, i);
        const vec3 translation = CoordinateSpaces::ModelPosToNovus(Track(bytes, model, bone.translation, sequence, milliseconds, vec3(0)));
        const quat sourceRotation = Track(bytes, model, bone.rotation, sequence, milliseconds, quat(1,0,0,0));
        const quat rotation = glm::normalize(glm::quat_cast(basis * glm::mat3_cast(sourceRotation) * glm::transpose(basis)));
        vec3 scale = CoordinateSpaces::ModelScaleToNovus(Track(bytes, model, bone.scale, sequence, milliseconds, vec3(1)));
        // Source visibility tracks collapse facial/helper bones to zero. Keep an
        // invertible neutral pose with a bounded residual, measured by skinning tests.
        for (u32 axis = 0; axis < 3; ++axis) if (std::abs(scale[axis]) < 0.00001f) scale[axis] = std::signbit(scale[axis]) ? -0.00001f : 0.00001f;
        mat4x4 parent = bone.parentBone < 0 ? mat4x4(1) : skin[bone.parentBone];
        if (bone.flags.IgnoreParentTranslate || bone.flags.IgnoreParentRotation || bone.flags.IgnoreParentScale)
        {
            auto parts = Decompose(parent);
            if (bone.flags.IgnoreParentTranslate) parts.translation = vec3(0);
            if (bone.flags.IgnoreParentRotation) parts.rotation = quat(1,0,0,0);
            if (bone.flags.IgnoreParentScale) parts.scale = vec3(1);
            parent = glm::translate(mat4x4(1), parts.translation) * glm::mat4_cast(parts.rotation) * glm::scale(mat4x4(1), parts.scale);
        }
        // Spherical leaf quads retain view-independent centers and scales here;
        // their camera-facing expansion happens per view in the renderer.
        if (bone.flags.CylindricialBillboardLockX || bone.flags.CylindricialBillboardLockY || bone.flags.CylindricialBillboardLockZ) throw std::runtime_error("Axis-locked billboard requires a neutral procedural rule");
        const vec3 pivot = skeleton.sourcePivots[i];
        original[i] = parent * glm::translate(mat4x4(1), pivot + translation) * glm::mat4_cast(rotation) * glm::scale(mat4x4(1), scale);
        skin[i] = original[i] * glm::translate(mat4x4(1), -pivot);
    }
    std::vector<A::LocalTransform> result(skeleton.data.joints.size());
    for (u32 i = 0; i < result.size(); ++i)
    {
        const auto& joint = skeleton.data.joints[i];
        const mat4x4 parent = joint.parentJointIndex < 0 ? mat4x4(1) : cooked[joint.parentJointIndex];
        cooked[i] = skeleton.cookedToSource[i] < 0 ? parent * glm::translate(mat4x4(1), joint.restTransform.translation) : original[skeleton.cookedToSource[i]];
        try { result[i] = Decompose(glm::inverse(parent) * cooked[i]); }
        catch (const std::exception& error) { throw std::runtime_error(std::string(error.what()) + " cooked=" + std::to_string(i) + " source=" + std::to_string(skeleton.cookedToSource[i]) + " timeMS=" + std::to_string(milliseconds)); }
    }
    return result;
}

M2ClipCooker::Result M2ClipCooker::Cook(const std::shared_ptr<Bytebuffer>& bytes, const M2::Layout& model, const M2SkeletonCooker::Result& skeleton, u32 sequence, u64 skeletonAssetID, u16 rate, std::ostream& report, bool preservePeriod)
{
    if (rate == 0)
    {
        auto deform = [&](const std::vector<A::LocalTransform>& pose)
        {
            std::vector<mat4x4> matrices(pose.size());
            for (u32 i = 0; i < pose.size(); ++i)
            {
                const auto& value = pose[i];
                const mat4x4 local = glm::translate(mat4x4(1), value.translation) * glm::mat4_cast(value.rotation) * glm::scale(mat4x4(1), value.scale);
                const i32 parent = skeleton.data.joints[i].parentJointIndex;
                matrices[i] = parent < 0 ? local : matrices[parent] * local;
            }
            std::vector<vec3> positions(model.md21.vertices.size, vec3(0));
            for (u32 i = 0; i < positions.size(); ++i)
            {
                const auto& vertex = At(bytes, model.md21.vertices, i);
                const vec3 position = CoordinateSpaces::ModelPosToNovus(vertex.position);
                for (u32 influence = 0; influence < 4; ++influence)
                {
                    if (!vertex.boneWeights[influence]) continue;
                    const u32 joint = skeleton.sourceToCooked[vertex.boneIndices[influence]];
                    positions[i] += vec3(matrices[joint] * vec4(position - skeleton.cookedPivots[joint], 1)) * (vertex.boneWeights[influence] / 255.0f);
                }
            }
            return positions;
        };
        for (const u16 candidateRate : {30, 60, 120, 240, 480, 1000})
        {
            // Independent loops must keep their exact period. Rounding a period
            // changes phase on every repetition, even when one cycle looks right.
            if (preservePeriod && (static_cast<u64>(At(bytes, model.md21.sequences, sequence).duration) * candidateRate) % 1000 != 0) continue;
            Result candidate = Cook(bytes, model, skeleton, sequence, skeletonAssetID, candidateRate, report);
            f32 maximumError = 0;
            vec3 lower(std::numeric_limits<f32>::max()), upper(std::numeric_limits<f32>::lowest());
            auto includeBounds = [&](const auto& positions) { for (const vec3& position : positions) { lower = glm::min(lower, position); upper = glm::max(upper, position); } };
            u32 worstVertex = 0;
            double worstTime = 0;
            f32 maximumStepError = 0;
            u32 stepIntervals = 0;
            const u32 samples = candidate.asset.sampleCount;
            const auto& source = At(bytes, model.md21.sequences, sequence);
            for (u32 interval = 0; interval + 1 < samples; ++interval)
            {
                const double beginMS = source.duration * static_cast<double>(interval) / (samples - 1);
                const double endMS = source.duration * static_cast<double>(interval + 1) / (samples - 1);
                auto crossesStep = [&](const auto& track)
                {
                    if (track.interpolationType != 0 || !track.timestamps.size || !track.values.size) return false;
                    const u32 slot = track.globalSequence >= 0 ? 0 : sequence;
                    if (slot >= track.timestamps.size || slot >= track.values.size) return false;
                    const auto& times = At(bytes, track.timestamps, slot);
                    const auto& values = At(bytes, track.values, slot);
                    if (times.size != values.size) throw std::runtime_error("Step track count mismatch");
                    const u32 period = track.globalSequence < 0 ? 0 : At(bytes, model.md21.globalLoops, track.globalSequence);
                    for (u32 key = 1; key < times.size; ++key)
                    {
                        const auto& first = At(bytes, values, key - 1);
                        const auto& second = At(bytes, values, key);
                        if (std::memcmp(&first, &second, sizeof(first)) == 0) continue;
                        double keyTime = At(bytes, times, key);
                        if (period) keyTime += std::floor(beginMS / period) * period;
                        if (keyTime > beginMS && keyTime <= endMS) return true;
                    }
                    return false;
                };
                bool step = false;
                for (u32 boneIndex = 0; boneIndex < model.md21.bones.size && !step; ++boneIndex)
                {
                    const auto& bone = At(bytes, model.md21.bones, boneIndex);
                    step = crossesStep(bone.translation) || crossesStep(bone.rotation) || crossesStep(bone.scale);
                }
                if (step) ++stepIntervals;
                for (const f32 alpha : {0.25f, 0.5f, 0.75f})
                {
                    const double time = source.duration * (interval + static_cast<double>(alpha)) / (samples - 1);
                    const auto exact = deform(Sample(bytes, model, skeleton, sequence, time));
                    std::vector<A::LocalTransform> interpolated(skeleton.data.joints.size());
                    for (const auto& track : candidate.data.tracks)
                    {
                        auto& value = interpolated[track.sourceJointIndex];
                        value.translation = glm::mix(candidate.data.translationSamples[track.translationOffset + interval], candidate.data.translationSamples[track.translationOffset + interval + 1], alpha);
                        value.rotation = Interpolate(candidate.data.rotationSamples[track.rotationOffset + interval], candidate.data.rotationSamples[track.rotationOffset + interval + 1], alpha);
                        value.scale = glm::mix(candidate.data.scaleSamples[track.scaleOffset + interval], candidate.data.scaleSamples[track.scaleOffset + interval + 1], alpha);
                    }
                    const auto sampled = deform(interpolated);
                    includeBounds(exact);
                    includeBounds(sampled);
                    for (u32 vertex = 0; vertex < exact.size(); ++vertex)
                    {
                        const f32 error = glm::length(exact[vertex] - sampled[vertex]);
                        if (step) { maximumStepError = std::max(maximumStepError, error); continue; }
                        if (error > maximumError) { maximumError = error; worstVertex = vertex; worstTime = time; }
                    }
                }
                if (maximumError > 0.0015f) break;
            }
            report << "rate-test\t" << sequence << '\t' << candidateRate << '\t' << maximumError << '\t' << (maximumError <= 0.0015f ? "accepted" : "retry") << '\n';
            report << "rate-peak\t" << sequence << '\t' << candidateRate << '\t' << worstVertex << '\t' << worstTime << '\n';
            report << "step-window\t" << sequence << '\t' << candidateRate << '\t' << stepIntervals << '\t' << maximumStepError << '\t' << source.duration / static_cast<double>(samples - 1) << '\n';
            // Uniform interpolated clips cannot reproduce an instantaneous step.
            // Bound its smoothing window to one 120 Hz interval and retain the
            // measured residual; smooth source motion still uses the 2 mm gate.
            // Leave headroom for independent samples between the cooker probes.
            if (maximumError <= 0.0015f && (!stepIntervals || candidateRate >= 120))
            {
                const size_t originalBytes = candidate.asset.GetSerializedSize(candidate.data);
                includeBounds(deform(Sample(bytes, model, skeleton, sequence, 0)));
                includeBounds(deform(Sample(bytes, model, skeleton, sequence, source.duration)));
                candidate.bounds.center = (lower + upper) * 0.5f;
                candidate.bounds.extents = (upper - lower) * 0.5f + vec3(0.01f);
                candidate.bounds.sphereRadius = glm::length(candidate.bounds.extents);
                std::vector<vec3> translations, scales;
                std::vector<quat> rotations;
                auto compact = [](const auto& input, auto& output, u32& offset, u32& count, auto& fallback, u32& flags, u32 flag)
                {
                    fallback = input[offset];
                    bool constant = true;
                    for (u32 sample = 1; sample < count; ++sample)
                        if (glm::length(input[offset + sample] - fallback) > 0.0000001f) { constant = false; break; }
                    if (constant) { offset = 0; count = 0; flags &= ~flag; return; }
                    const u32 destination = static_cast<u32>(output.size());
                    output.insert(output.end(), input.begin() + offset, input.begin() + offset + count);
                    offset = destination;
                };
                for (auto& track : candidate.data.tracks)
                {
                    compact(candidate.data.translationSamples, translations, track.translationOffset, track.numTranslations, track.defaultTranslation, track.flags, A::AnimationTrackFlags_HasTranslationSamples);
                    compact(candidate.data.rotationSamples, rotations, track.rotationOffset, track.numRotations, track.defaultRotation, track.flags, A::AnimationTrackFlags_HasRotationSamples);
                    compact(candidate.data.scaleSamples, scales, track.scaleOffset, track.numScales, track.defaultScale, track.flags, A::AnimationTrackFlags_HasScaleSamples);
                }
                candidate.data.translationSamples = std::move(translations);
                candidate.data.rotationSamples = std::move(rotations);
                candidate.data.scaleSamples = std::move(scales);
                report << "constant-channels\t" << sequence << '\t' << originalBytes << '\t' << candidate.asset.GetSerializedSize(candidate.data) << '\n';
                return candidate;
            }
        }
        throw std::runtime_error("Uniform sample-rate candidates exceed 2 mm deformation tolerance");
    }
    Result result;
    const auto& source = At(bytes, model.md21.sequences, sequence);
    u32 resolved = sequence;
    for (u32 hops = 0; At(bytes, model.md21.sequences, resolved).flags.IsAlias; ++hops)
    {
        if (hops >= model.md21.sequences.size) throw std::runtime_error("Source animation alias cycle");
        resolved = At(bytes, model.md21.sequences, resolved).nextAliasID;
    }
    if (!At(bytes, model.md21.sequences, resolved).flags.HasEmbeddedAnimationData) throw std::runtime_error("External source animation requires an explicit payload resolver");
    if (!source.duration || !rate) throw std::runtime_error("Invalid source clip duration/rate");
    result.asset.sourcePoseDomainAssetID = skeletonAssetID;
    result.asset.sourcePoseDomainType = A::PoseDomainType::Skeleton;
    result.asset.flags = A::AnimationClipFlags_Looping;
    result.asset.sampleRateHz = rate;
    result.asset.sampleCount = std::max(2u, static_cast<u32>(std::llround(source.duration * rate / 1000.0)) + 1);
    result.asset.durationMicroseconds = static_cast<u32>(std::llround((result.asset.sampleCount - 1) * 1000000.0 / rate));
    std::vector<std::vector<A::LocalTransform>> frames;
    for (u32 frame = 0; frame < result.asset.sampleCount; ++frame) frames.push_back(Sample(bytes, model, skeleton, resolved, source.duration * static_cast<double>(frame) / (result.asset.sampleCount - 1)));
    for (u32 joint = 0; joint < skeleton.data.joints.size(); ++joint)
    {
        A::AnimationTrack track;
        track.sourceJointIndex = joint;
        track.flags = A::AnimationTrackFlags_HasTranslationSamples | A::AnimationTrackFlags_HasRotationSamples | A::AnimationTrackFlags_HasScaleSamples;
        track.translationOffset = static_cast<u32>(result.data.translationSamples.size());
        track.rotationOffset = static_cast<u32>(result.data.rotationSamples.size());
        track.scaleOffset = static_cast<u32>(result.data.scaleSamples.size());
        track.numTranslations = track.numRotations = track.numScales = result.asset.sampleCount;
        for (const auto& frame : frames)
        {
            result.data.translationSamples.push_back(frame[joint].translation);
            result.data.rotationSamples.push_back(frame[joint].rotation);
            result.data.scaleSamples.push_back(frame[joint].scale);
        }
        result.data.tracks.push_back(track);
    }
    report << "sampled\t" << sequence << '\t' << resolved << '\t' << source.id << '\t' << source.variationID << '\t' << source.duration << '\t' << rate << '\t' << result.asset.sampleCount << '\n';
    auto hash = [](const std::string& value) { return XXHash64::hash(value.data(), value.size(), 0); };
    for (u32 index = 0; index < model.md21.events.size; ++index)
    {
        const auto& event = At(bytes, model.md21.events, index);
        if (resolved >= event.enabled.timestamps.size) continue;
        const auto& times = At(bytes, event.enabled.timestamps, resolved);
        if (!times.size) continue;
        char code[5] = {};
        std::memcpy(code, &event.identifier, 4);
        const bool contact = code[0] == '$' && std::string("BFRSW").find(code[1]) != std::string::npos && (code[2] == 'L' || code[2] == 'R') && code[3] >= '0' && code[3] <= '9';
        if (!contact || event.enabled.globalSequence >= 0)
        {
            report << "unsupported-event\t" << sequence << '\t' << code << '\t' << times.size << "\tno legacy gameplay dispatch\n";
            continue;
        }
        const bool left = code[2] == 'L';
        const u64 semantic = hash(left ? "humanoid/leftfoot" : "humanoid/rightfoot");
        for (u32 key = 0; key < times.size; ++key)
        {
            const u32 sourceTime = At(bytes, times, key);
            if (sourceTime > source.duration) throw std::runtime_error("Source footstep exceeds sequence duration");
            const u32 time = static_cast<u32>(std::llround(sourceTime * static_cast<double>(result.asset.durationMicroseconds) / source.duration));
            const u64 marker = hash(left ? "animation-sync-marker/leftfoot" : "animation-sync-marker/rightfoot");
            if (std::any_of(result.data.synchronizationMarkers.begin(), result.data.synchronizationMarkers.end(), [=](const auto& value) { return value.semanticID == marker && value.timeMicroseconds == time; }))
            {
                report << "merged-contact-event\t" << sequence << '\t' << code << '\t' << sourceTime << "\tone neutral contact per foot/time\n";
                continue;
            }
            A::AnimationEvent output;
            output.timeMicroseconds = time;
            output.eventTypeID = static_cast<u32>(hash("animation-event/footstep"));
            output.payloadTypeID = static_cast<u32>(hash("animation-payload/joint-semantic.v1"));
            output.payloadOffset = static_cast<u32>(result.data.eventPayloadBytes.size());
            output.payloadSize = sizeof(semantic);
            const auto* bytes = reinterpret_cast<const u8*>(&semantic);
            result.data.eventPayloadBytes.insert(result.data.eventPayloadBytes.end(), bytes, bytes + sizeof(semantic));
            result.data.events.push_back(output);
            result.data.synchronizationMarkers.push_back({marker, time, 0});
        }
    }
    std::stable_sort(result.data.events.begin(), result.data.events.end(), [](const auto& first, const auto& second) { return first.timeMicroseconds < second.timeMicroseconds; });
    std::stable_sort(result.data.synchronizationMarkers.begin(), result.data.synchronizationMarkers.end(), [](const auto& first, const auto& second) { return first.timeMicroseconds < second.timeMicroseconds; });
    return result;
}
