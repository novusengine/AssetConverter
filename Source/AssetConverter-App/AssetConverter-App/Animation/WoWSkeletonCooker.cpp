#include "WoWSkeletonCooker.h"
#include <FileFormat/Shared.h>
#include <xxhash/xxhash64.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace
{
    namespace A = FileFormat::Animation;
    u64 Hash(const std::string& value) { return XXHash64::hash(value.data(), value.size(), 0); }
    struct Mapping { const char* semantic; i32 key; u32 crc; };
    // Source identifiers are only used here, never written as runtime selection keys.
    constexpr Mapping MAPPING[] = {
        {"hips",5,0},{"spine",4,0},{"spine1",-1,310207871},{"spine2",-2,0},
        {"neck",-1,2149808493},{"head",6,0},
        {"leftshoulder",2,0},{"leftarm",0,0},{"leftforearm",81,0},{"lefthand",65,0},
        {"rightshoulder",3,0},{"rightarm",1,0},{"rightforearm",80,0},{"righthand",64,0},
        {"leftupleg",-1,3947676041},{"leftleg",-1,1475065404},{"leftfoot",59,0},{"lefttoebase",-1,1094736301},
        {"rightupleg",-1,289668330},{"rightleg",-1,2917436255},{"rightfoot",60,0},{"righttoebase",-1,3142542542}
    };
}

WoWSkeletonCooker::Result WoWSkeletonCooker::Cook(const std::shared_ptr<Bytebuffer>& bytes, const M2::Layout& model, const std::vector<FileFormat::Animation::RigFamilyJoint>& family, std::ostream& report)
{
    Result result;
    result.asset.rigFamilyAssetID = Hash("animation/bipedal_humanoid.rigfamily");
    const u32 count = model.md21.bones.size;
    if (!count || count >= 254) throw std::runtime_error("Source skeleton exceeds packed joint range");
    result.sourceToCooked.resize(count, 255);
    result.sourcePivots.resize(count);
    std::vector<bool> deforms(count, false);
    for (u32 i = 0; i < model.md21.vertices.size; ++i)
    {
        const auto& vertex = *model.md21.vertices.GetElement(bytes, i);
        for (u32 influence = 0; influence < 4; ++influence)
            if (vertex.boneWeights[influence])
            {
                if (vertex.boneIndices[influence] >= count) throw std::runtime_error("Source skin index out of range");
                deforms[vertex.boneIndices[influence]] = true;
            }
    }
    for (u32 i = 0; i < count; ++i)
    {
        const auto& bone = *model.md21.bones.GetElement(bytes, i);
        if (bone.parentBone >= static_cast<i32>(i) || bone.parentBone < -1) throw std::runtime_error("Source hierarchy not topological");
        result.sourcePivots[i] = CoordinateSpaces::ModelPosToNovus(bone.pivot);
        if (bone.flags.SphericalBillboard)
        {
            for (u32 child = i + 1; child < count; ++child) if (model.md21.bones.GetElement(bytes, child)->parentBone == static_cast<i32>(i)) throw std::runtime_error("Camera-facing joint must be a leaf");
            for (u32 vertexIndex = 0; vertexIndex < model.md21.vertices.size; ++vertexIndex)
            {
                const auto& vertex = *model.md21.vertices.GetElement(bytes, vertexIndex);
                for (u32 influence = 0; influence < 4; ++influence) if (vertex.boneIndices[influence] == i && vertex.boneWeights[influence] != 0 && vertex.boneWeights[influence] != 255) throw std::runtime_error("Camera-facing vertices require rigid skinning");
            }
        }
    }
    std::vector<i32> familyToCooked(family.size(), -1);
    std::vector<quat> familyRotations(family.size());
    auto append = [&](i32 source, i32 parent, i32 familyIndex, u64 semantic, vec3 pivot)
    {
        A::SkeletonJoint joint;
        joint.semanticID = semantic;
        joint.parentJointIndex = parent;
        joint.familyJointIndex = familyIndex;
        joint.hierarchyDepth = parent < 0 ? 0 : result.data.joints[parent].hierarchyDepth + 1;
        joint.flags = source >= 0 && deforms[source] ? A::SkeletonJointFlags_Deformation : A::SkeletonJointFlags_Helper;
        if (familyIndex < 0) joint.flags |= A::SkeletonJointFlags_Extension;
        if (source >= 0 && model.md21.bones.GetElement(bytes, source)->flags.SphericalBillboard) joint.flags |= A::SkeletonJointFlags_CameraFacing;
        joint.restTransform.translation = pivot - (parent < 0 ? vec3(0) : result.cookedPivots[parent]);
        joint.inverseBindTransform = Math::AffineMatrix::TransformMatrix(-pivot, quat(1, 0, 0, 0), vec3(1));
        const u32 cooked = static_cast<u32>(result.data.joints.size());
        result.data.joints.push_back(joint);
        result.cookedPivots.push_back(pivot);
        result.cookedToSource.push_back(source);
        if (source >= 0) result.sourceToCooked[source] = static_cast<u8>(cooked);
        report << "mapping\t" << cooked << '\t' << source << '\t' << familyIndex << '\t' << parent << '\t' << semantic << '\n';
        return cooked;
    };
    for (u32 f = 0; f < family.size(); ++f)
    {
        const auto& fj = family[f];
        familyRotations[f] = glm::normalize((fj.parentJointIndex < 0 ? quat(1,0,0,0) : familyRotations[fj.parentJointIndex]) * fj.canonicalTransform.rotation);
        const Mapping* mapping = nullptr;
        for (const auto& candidate : MAPPING) if (Hash(std::string("humanoid/") + candidate.semantic) == fj.semanticID) mapping = &candidate;
        if (!mapping)
        {
            if (fj.flags & A::RigFamilyJointFlags_Required) throw std::runtime_error("Required family semantic not mapped");
            continue;
        }
        i32 source = -1;
        for (u32 i = 0; i < count; ++i)
        {
            const auto& bone = *model.md21.bones.GetElement(bytes, i);
            // The alternate thigh names use the same pelvis -> thigh -> calf chain.
            const bool alternateThigh = (mapping->crc == 3947676041u && bone.boneNameCRC == 1756834604u) || (mapping->crc == 289668330u && bone.boneNameCRC == 2461537871u);
            if ((mapping->key >= 0 && bone.keyBoneID == mapping->key) || (mapping->key == -1 && (bone.boneNameCRC == mapping->crc || alternateThigh)))
            {
                if (source >= 0) throw std::runtime_error("Ambiguous source joint mapping");
                source = i;
            }
        }
        if (source < 0 && mapping->key != -2) throw std::runtime_error(std::string("Missing source joint: ") + mapping->semantic);
        const i32 parent = fj.parentJointIndex < 0 ? -1 : familyToCooked[fj.parentJointIndex];
        if (fj.parentJointIndex >= 0 && parent < 0) throw std::runtime_error("Missing mapped parent");
        vec3 pivot = source >= 0 ? result.sourcePivots[source] : result.cookedPivots[parent];
        // The extra chest joint carries no deformation. Native sampling derives it from its parent.
        if (source < 0) pivot += vec3(0, 0.08f, 0);
        const u32 cooked = append(source, parent, f, fj.semanticID, pivot);
        familyToCooked[f] = cooked;
        A::SkeletonFamilyBinding binding;
        binding.skeletonJointIndex = cooked;
        binding.familyJointIndex = f;
        binding.skeletonToFamily.rotation = glm::inverse(familyRotations[f]);
        binding.familyToSkeleton.rotation = familyRotations[f];
        const f32 concreteLength = glm::length(result.data.joints[cooked].restTransform.translation);
        const f32 familyLength = glm::length(fj.canonicalTransform.translation);
        const f32 ratio = concreteLength > 0.000001f && familyLength > 0.000001f ? familyLength / concreteLength : 1.0f;
        binding.skeletonToFamily.scale = vec3(ratio);
        binding.familyToSkeleton.scale = vec3(1.0f / ratio);
        result.data.familyBindings.push_back(binding);
    }
    // Coincident pivots do not imply matching source/family units. Borrow the
    // nearest usable child calibration, or the parent's, for zero-length joints.
    for (auto& binding : result.data.familyBindings)
    {
        if (glm::length(result.data.joints[binding.skeletonJointIndex].restTransform.translation) > 0.000001f) continue;
        const A::SkeletonFamilyBinding* neighbor = nullptr;
        for (const auto& candidate : result.data.familyBindings)
        {
            if (family[candidate.familyJointIndex].parentJointIndex != static_cast<i32>(binding.familyJointIndex)) continue;
            if (glm::length(result.data.joints[candidate.skeletonJointIndex].restTransform.translation) <= 0.000001f) continue;
            neighbor = &candidate;
            break;
        }
        if (!neighbor)
            for (const auto& candidate : result.data.familyBindings)
                if (static_cast<i32>(candidate.familyJointIndex) == family[binding.familyJointIndex].parentJointIndex) { neighbor = &candidate; break; }
        if (neighbor)
        {
            binding.skeletonToFamily.scale = neighbor->skeletonToFamily.scale;
            binding.familyToSkeleton.scale = neighbor->familyToSkeleton.scale;
            report << "coincident-pivot-calibration\t" << binding.skeletonJointIndex << '\t' << neighbor->skeletonJointIndex << '\t' << binding.skeletonToFamily.scale.x << '\n';
        }
    }
    // Infer A/T-pose offsets along the arms only. A curved spine, hunched neck or
    // bent hock is bind anatomy, not a reference-pose rotation to straighten.
    std::vector<mat4x4> familyModels(family.size());
    std::vector<quat> referenceRotations(family.size());
    for (u32 f = 0; f < family.size(); ++f)
    {
        const auto& joint = family[f];
        const auto& rest = joint.canonicalTransform;
        const mat4x4 local = glm::translate(mat4x4(1), rest.translation) * glm::mat4_cast(rest.rotation) * glm::scale(mat4x4(1), rest.scale);
        familyModels[f] = joint.parentJointIndex < 0 ? local : familyModels[joint.parentJointIndex] * local;
    }
    for (u32 f = 0; f < family.size(); ++f)
    {
        const i32 parent = family[f].parentJointIndex;
        bool arm = false;
        for (const char* name : {"leftshoulder", "leftarm", "leftforearm", "lefthand", "rightshoulder", "rightarm", "rightforearm", "righthand"})
            arm |= family[f].semanticID == Hash(std::string("humanoid/") + name);
        const quat inherited = parent < 0 ? quat(1,0,0,0) : referenceRotations[parent] * glm::inverse(familyRotations[parent]);
        referenceRotations[f] = arm ? glm::normalize(inherited * familyRotations[f]) : familyRotations[f];
        if (familyToCooked[f] < 0) continue;
        for (u32 child = f + 1; arm && child < family.size(); ++child)
        {
            if (family[child].parentJointIndex != static_cast<i32>(f) || familyToCooked[child] < 0) continue;
            const vec3 canonicalDirection = vec3(familyModels[child][3]) - vec3(familyModels[f][3]);
            const vec3 concreteDirection = result.cookedPivots[familyToCooked[child]] - result.cookedPivots[familyToCooked[f]];
            if (glm::length2(canonicalDirection) < 0.000001f || glm::length2(concreteDirection) < 0.000001f) continue;
            referenceRotations[f] = glm::normalize(glm::rotation(glm::normalize(canonicalDirection), glm::normalize(concreteDirection)) * familyRotations[f]);
            break;
        }
        auto& binding = *std::find_if(result.data.familyBindings.begin(), result.data.familyBindings.end(), [f](const auto& value) { return value.familyJointIndex == f; });
        const quat referenceLocal = glm::inverse(parent < 0 ? quat(1,0,0,0) : referenceRotations[parent]) * referenceRotations[f];
        binding.familyReferenceRotation = glm::normalize(glm::inverse(family[f].canonicalTransform.rotation) * referenceLocal);
        binding.skeletonToFamily.rotation = glm::inverse(referenceRotations[f]);
        binding.familyToSkeleton.rotation = referenceRotations[f];
    }
    // Preserve remaining source joints as extensions. Source roots become children
    // of the neutral root; sampled model-space transforms preserve their motion.
    for (u32 i = 0; i < count; ++i)
    {
        if (result.sourceToCooked[i] != 255) continue;
        const auto& bone = *model.md21.bones.GetElement(bytes, i);
        const i32 parent = bone.parentBone < 0 ? 0 : result.sourceToCooked[bone.parentBone];
        if (parent == 255) throw std::runtime_error("Unresolved extension parent");
        append(i, parent, -1, Hash("extension/" + std::to_string(bone.boneNameCRC) + "/" + std::to_string(i)), result.sourcePivots[i]);
    }
    u32 maximumDepth = 0;
    for (const auto& joint : result.data.joints) maximumDepth = std::max(maximumDepth, static_cast<u32>(joint.hierarchyDepth));
    for (u32 depth = 0; depth <= maximumDepth; ++depth)
    {
        A::HierarchyDepthRange range;
        range.jointIndexOffset = static_cast<u32>(result.data.hierarchyDepthJointIndices.size());
        for (u32 i = 0; i < result.data.joints.size(); ++i) if (result.data.joints[i].hierarchyDepth == depth) result.data.hierarchyDepthJointIndices.push_back(i);
        range.numJointIndices = static_cast<u32>(result.data.hierarchyDepthJointIndices.size()) - range.jointIndexOffset;
        result.data.hierarchyDepthRanges.push_back(range);
    }
    for (u32 i = 0; i < model.md21.attachments.size; ++i)
    {
        const auto& source = *model.md21.attachments.GetElement(bytes, i);
        if (source.bone >= count) throw std::runtime_error("Attachment joint out of range");
        A::SkeletonAttachment attachment;
        const bool selected = source.id < model.md21.attachmentLooukpList.size && *model.md21.attachmentLooukpList.GetElement(bytes, source.id) == i;
        // The lookup selects the public socket when raw records reuse an ID.
        // Preserve non-selected records under distinct auxiliary semantics.
        const std::string semantic = selected ? "attachment/" + std::to_string(source.id) : "attachment/auxiliary/" + std::to_string(i);
        attachment.semanticID = Hash(semantic);
        report << "attachment\t" << i << '\t' << source.id << '\t' << semantic << '\n';
        attachment.jointIndex = result.sourceToCooked[source.bone];
        attachment.localTransform.translation = CoordinateSpaces::ModelPosToNovus(source.position) - result.sourcePivots[source.bone];
        result.data.attachments.push_back(attachment);
    }
    return result;
}
