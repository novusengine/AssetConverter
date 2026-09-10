#include "WoWAnimationConverter.h"
#include "WoWSkeletonCooker.h"
#include "WoWClipCooker.h"
#include "AssetConverter-App/Model/ModelV2Builder.h"
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include "AssetConverter-App/Runtime.h"
#include "AssetConverter-App/Casc/CascLoader.h"
#include "AssetConverter-App/Util/ServiceLocator.h"
#include <Base/Util/DebugHandler.h>
#include <FileFormat/Warcraft/M2/M2.h>
#include <FileFormat/Warcraft/Parsers/M2Parser.h>
#include <xxhash/xxhash64.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>

namespace
{
    namespace fs = std::filesystem;
    void SaveSource(const fs::path& path, const std::shared_ptr<Bytebuffer>& bytes, std::ofstream& inventory, u32 id)
    {
        if (!bytes || !bytes->writtenData) throw std::runtime_error("Missing source file " + std::to_string(id));
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes->GetDataPointer()), bytes->writtenData);
        if (!out) throw std::runtime_error("Could not save source fixture");
        inventory << "file\t" << id << '\t' << path.filename().string() << '\t' << bytes->writtenData << '\t' << std::hex << XXHash64::hash(bytes->GetDataPointer(), bytes->writtenData, 0) << std::dec << '\n';
    }
    template <typename T> u32 Bits(const T& flags) { u32 bits = 0; std::memcpy(&bits, &flags, sizeof(flags)); return bits; }
}

i32 WoWAnimationConverter::ExportCharacterAnimations(Runtime* runtime, const char* outputDirectory, const char* characterName)
{
    try
    {
        const fs::path output = fs::absolute(outputDirectory);
        fs::create_directories(output / "sources");
        runtime->paths.looseOutput = output / "assets";
        runtime->scheduler.Initialize(4);
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        const fs::path familyPath = fs::current_path() / "Data/Pact/data/staging/animation/bipedal_humanoid.rigfamily";
        std::ifstream familyFile(familyPath, std::ios::binary | std::ios::ate);
        if (!familyFile) throw std::runtime_error("Missing neutral RigFamily: " + familyPath.string());
        const auto familySize = familyFile.tellg();
        auto familyBuffer = std::make_shared<Bytebuffer>(nullptr, static_cast<size_t>(familySize));
        familyFile.seekg(0);
        familyFile.read(reinterpret_cast<char*>(familyBuffer->GetDataPointer()), familySize);
        familyBuffer->writtenData = static_cast<size_t>(familySize);
        FileFormat::Animation::RigFamilyAsset familyAsset;
        if (!familyFile || !FileFormat::Animation::RigFamilyAsset::Read(familyBuffer, familyAsset)) throw std::runtime_error("Invalid neutral RigFamily");
        const auto* familyBegin = reinterpret_cast<const FileFormat::Animation::RigFamilyJoint*>(familyBuffer->GetDataPointer() + familyAsset.jointsOffset);
        std::vector<FileFormat::Animation::RigFamilyJoint> family(familyBegin, familyBegin + familyAsset.numJoints);
        auto* casc = ServiceLocator::SetCascLoader(new CascLoader("listfile.csv", "enGB"));
        if (casc->Load() != CascLoader::Result::Success) throw std::runtime_error("Could not open Classic Era CASC");
        std::ofstream inventory(output / "inventory.tsv");
        inventory << "sourceRoot\t" << fs::current_path().generic_string() << "\nhash\txxhash64(seed=0)\n";
        const std::vector<std::string> roster = {"humanmale", "humanfemale", "orcmale", "orcfemale", "dwarfmale", "dwarffemale", "nightelfmale", "nightelffemale", "scourgemale", "scourgefemale", "taurenmale", "taurenfemale", "gnomemale", "gnomefemale", "trollmale", "trollfemale"};
        if (characterName && std::find(roster.begin(), roster.end(), characterName) == roster.end()) throw std::runtime_error("Unknown Classic character selection");
        for (const std::string& name : roster)
        {
            if (characterName && name != characterName) continue;
            const auto gender = name.ends_with("female") ? "female" : "male";
            const auto race = name.substr(0, name.size() - std::strlen(gender));
            const std::string source = "character/" + race + "/" + gender + "/" + name + ".m2";
            const u32 id = casc->GetFileIDFromListFilePath(source);
            auto root = casc->GetFileByID(id);
            SaveSource(output / "sources" / (name + ".m2"), root, inventory, id);
            M2::Parser parser;
            M2::Layout model;
            if (!parser.TryParse(M2::Parser::ParseType::Root, root, model)) throw std::runtime_error("M2 parse failed: " + source);
            inventory << "model\t" << name << "\tversion\t" << model.md21.version << "\tbones\t" << model.md21.bones.size << "\tsequences\t" << model.md21.sequences.size << "\tglobalLoops\t" << model.md21.globalLoops.size << "\tattachments\t" << model.md21.attachments.size << "\tevents\t" << model.md21.events.size << "\ttextureTransforms\t" << model.md21.textureTransforms.size << "\tparticles\t" << model.md21.particleEmitters.size << "\tribbons\t" << model.md21.ribbonEmitters.size << '\n';
            if (model.sfid.skinFileIDs.empty()) throw std::runtime_error("M2 has no skin");
            auto skin = casc->GetFileByID(model.sfid.skinFileIDs[0]);
            SaveSource(output / "sources" / (name + ".skin"), skin, inventory, model.sfid.skinFileIDs[0]);
            if (!parser.TryParse(M2::Parser::ParseType::Skin, skin, model)) throw std::runtime_error("Skin parse failed");
            std::ofstream mapping(output / (name + "-mapping.tsv"));
            // Tauren lower limbs retain their digitigrade bind posture when retargeted.
            auto skeleton = WoWSkeletonCooker::Cook(root, model, family, mapping);
            const std::string skeletonPath = "animations/wow/" + name + "/" + name + ".skeleton";
            auto skeletonBuffer = std::make_shared<Bytebuffer>(nullptr, skeleton.asset.GetSerializedSize(skeleton.data));
            if (!skeleton.asset.Save(skeletonBuffer, skeleton.data) || !runtime->pactInfo.GetManifestForFile(runtime, skeletonBuffer->writtenData).AddFile(runtime, skeletonPath, skeletonBuffer)) throw std::runtime_error("Skeleton publication failed");
            const u64 skeletonID = XXHash64::hash(skeletonPath.data(), skeletonPath.size(), 0);
            std::vector<u64> textures(model.md21.textures.size, FileFormat::INVALID_ASSET_ID);
            for (u32 textureIndex = 0; textureIndex < textures.size() && textureIndex < model.txid.textureFileIDs.size(); ++textureIndex)
            {
                const u32 textureID = model.txid.textureFileIDs[textureIndex];
                if (textureID == 0 || textureID == std::numeric_limits<u32>::max() || !casc->InCascAndListFile(textureID)) continue;
                fs::path texturePath = fs::path("texture") / casc->GetFilePathFromListFileID(textureID);
                texturePath.replace_extension("dds");
                std::string textureName = texturePath.generic_string();
                std::transform(textureName.begin(), textureName.end(), textureName.begin(), ::tolower);
                textures[textureIndex] = XXHash64::hash(textureName.data(), textureName.size(), 0);
            }
            if (!ModelV2Builder::ConvertM2AndAdd(runtime, root, skin, model, textures, "animations/wow/" + name + "/" + name + ".model", skeletonID, skeleton.sourceToCooked)) throw std::runtime_error("Model conversion failed");
            const std::string previewMaterial = "material/generated/nbs/blender.materialinstance";
            runtime->previewMaterialAssetID = XXHash64::hash(previewMaterial.data(), previewMaterial.size(), 0);
            if (!ModelV2Builder::ConvertM2AndAdd(runtime, root, skin, model, textures, "animations/wow/" + name + "/" + name + ".preview.model", skeletonID, skeleton.sourceToCooked)) throw std::runtime_error("Preview Model conversion failed");
            runtime->previewMaterialAssetID = FileFormat::INVALID_ASSET_ID;
            for (u32 index = 0; index < model.md21.bones.size; ++index)
            {
                const auto& bone = *model.md21.bones.GetElement(root, index);
                inventory << "bone\t" << name << '\t' << index << '\t' << bone.parentBone << '\t' << bone.keyBoneID << '\t' << bone.boneNameCRC << '\t' << Bits(bone.flags) << '\t' << bone.pivot.x << '\t' << bone.pivot.y << '\t' << bone.pivot.z;
                inventory << '\t' << bone.translation.interpolationType << '\t' << bone.rotation.interpolationType << '\t' << bone.scale.interpolationType << '\t' << bone.translation.globalSequence << '\t' << bone.rotation.globalSequence << '\t' << bone.scale.globalSequence << '\n';
            }
            std::map<std::string, std::vector<FileFormat::Animation::AnimationClipVariant>> slots;
            FileFormat::Animation::AnimationBoundsData boundsData;
            boundsData.clips.push_back({});
            vec3 boundsMinimum(std::numeric_limits<f32>::max()), boundsMaximum(std::numeric_limits<f32>::lowest());
            std::vector<i32> independentClocks;
            for (u32 boneIndex = 0; boneIndex < model.md21.bones.size; ++boneIndex)
            {
                const auto& bone = *model.md21.bones.GetElement(root, boneIndex);
                for (const i32 clock : {bone.translation.globalSequence, bone.rotation.globalSequence, bone.scale.globalSequence})
                    if (clock >= 0 && *model.md21.globalLoops.GetElement(root, clock) != 0 && std::find(independentClocks.begin(), independentClocks.end(), clock) == independentClocks.end()) independentClocks.push_back(clock);
            }
            for (const i32 clock : independentClocks)
            {
                const std::string clockSuffix = clock == independentClocks.front() ? "" : "-" + std::to_string(clock);
                auto isolated = std::make_shared<Bytebuffer>(nullptr, root->writtenData);
                std::memcpy(isolated->GetDataPointer(), root->GetDataPointer(), root->writtenData);
                isolated->writtenData = root->writtenData;
                FileFormat::Animation::BoneMaskAsset mask;
                FileFormat::Animation::BoneMaskData maskData;
                mask.poseDomainAssetID = skeletonID;
                mask.poseDomainType = FileFormat::Animation::PoseDomainType::Skeleton;
                maskData.jointWeights.resize(skeleton.data.joints.size(), 0);
                for (u32 boneIndex = 0; boneIndex < model.md21.bones.size; ++boneIndex)
                {
                    auto& bone = *model.md21.bones.GetElement(isolated, boneIndex);
                    const bool animated = bone.translation.globalSequence == clock || bone.rotation.globalSequence == clock || bone.scale.globalSequence == clock;
                    if (animated) maskData.jointWeights[skeleton.sourceToCooked[boneIndex]] = 255;
                    auto isolate = [&](auto& track)
                    {
                        // The isolated clip owns one complete clock period, including its
                        // final key. Runtime looping supplies the wrap, not the sampler.
                        if (track.globalSequence == clock) { track.globalSequence = -1; return; }
                        // Bake only this clock. Rest-relative additive channels
                        // leave the joint's body animation and other clocks intact.
                        track.timestamps = {}; track.values = {}; track.globalSequence = -1;
                    };
                    isolate(bone.translation); isolate(bone.rotation); isolate(bone.scale);
                }
                auto& sequence = *model.md21.sequences.GetElement(isolated, 0);
                sequence.duration = *model.md21.globalLoops.GetElement(root, clock);
                sequence.flags.IsAlias = false;
                sequence.flags.HasEmbeddedAnimationData = true;
                auto ambient = WoWClipCooker::Cook(isolated, model, skeleton, 0, skeletonID, 0, inventory);
                ambient.asset.flags |= FileFormat::Animation::AnimationClipFlags_Additive;
                for (auto& track : ambient.data.tracks)
                {
                    const auto& rest = skeleton.data.joints[track.sourceJointIndex].restTransform;
                    track.defaultTranslation -= rest.translation;
                    track.defaultRotation = glm::normalize(glm::inverse(rest.rotation) * track.defaultRotation);
                    track.defaultScale /= rest.scale;
                    for (u32 i = 0; i < track.numTranslations; ++i) ambient.data.translationSamples[track.translationOffset + i] -= rest.translation;
                    for (u32 i = 0; i < track.numRotations; ++i) ambient.data.rotationSamples[track.rotationOffset + i] = glm::normalize(glm::inverse(rest.rotation) * ambient.data.rotationSamples[track.rotationOffset + i]);
                    for (u32 i = 0; i < track.numScales; ++i) ambient.data.scaleSamples[track.scaleOffset + i] /= rest.scale;
                }
                const std::string ambientPath = "animations/wow/" + name + "/ambient-face" + clockSuffix + ".animationclip";
                auto ambientBuffer = std::make_shared<Bytebuffer>(nullptr, ambient.asset.GetSerializedSize(ambient.data));
                if (!ambient.asset.Save(ambientBuffer, ambient.data) || !runtime->pactInfo.GetManifestForFile(runtime, ambientBuffer->writtenData).AddFile(runtime, ambientPath, ambientBuffer)) throw std::runtime_error("Independent motion publication failed");
                const std::string maskPath = "animations/wow/" + name + "/ambient-face" + clockSuffix + ".bonemask";
                auto maskBuffer = std::make_shared<Bytebuffer>(nullptr, mask.GetSerializedSize(maskData));
                if (!mask.Save(maskBuffer, maskData) || !runtime->pactInfo.GetManifestForFile(runtime, maskBuffer->writtenData).AddFile(runtime, maskPath, maskBuffer)) throw std::runtime_error("Independent motion mask publication failed");
                FileFormat::Animation::AnimationClipVariant variant;
                variant.animationClipAssetID = XXHash64::hash(ambientPath.data(), ambientPath.size(), 0);
                variant.selectionWeight = 1;
                variant.stableVariationID = static_cast<u32>(variant.animationClipAssetID);
                slots["ambientface" + clockSuffix].push_back(variant);
                inventory << "independent-layer\t" << name << '\t' << clock << '\t' << ambientPath << '\t' << maskPath << '\t' << ambient.asset.durationMicroseconds << '\t' << XXHash64::hash(maskPath.data(), maskPath.size(), 0) << '\n';
            }
            // Independent channels are supplied by the additive clock clips, not
            // baked into each body clip a second time.
            for (u32 boneIndex = 0; boneIndex < model.md21.bones.size; ++boneIndex)
            {
                auto& bone = *model.md21.bones.GetElement(root, boneIndex);
                auto removeIndependent = [&](auto& track)
                {
                    if (std::find(independentClocks.begin(), independentClocks.end(), track.globalSequence) == independentClocks.end()) return;
                    track.timestamps = {}; track.values = {}; track.globalSequence = -1;
                };
                removeIndependent(bone.translation); removeIndependent(bone.rotation); removeIndependent(bone.scale);
            }
            for (u32 index = 0; index < model.md21.sequences.size; ++index)
            {
                const auto& sequence = *model.md21.sequences.GetElement(root, index);
                inventory << "sequence\t" << name << '\t' << index << '\t' << sequence.id << '\t' << sequence.variationID << '\t' << sequence.duration << '\t' << Bits(sequence.flags) << '\t' << sequence.nextAliasID << '\t' << sequence.frequency << '\n';
                if (sequence.id == 0 || sequence.id == 4 || sequence.id == 5 || sequence.id == 69)
                {
                    const std::string slot = sequence.id == 0 ? "idle" : sequence.id == 4 ? "walk" : sequence.id == 5 ? "run" : "dance";
                    auto clip = WoWClipCooker::Cook(root, model, skeleton, index, skeletonID, 0, inventory);
                    auto clipBuffer = std::make_shared<Bytebuffer>(nullptr, clip.asset.GetSerializedSize(clip.data));
                    const std::string clipPath = "animations/wow/" + name + "/" + slot + "-" + std::to_string(sequence.variationID) + ".animationclip";
                    if (!clip.asset.Save(clipBuffer, clip.data) || !runtime->pactInfo.GetManifestForFile(runtime, clipBuffer->writtenData).AddFile(runtime, clipPath, clipBuffer)) throw std::runtime_error("Clip publication failed: " + clipPath);
                    const std::string semantic = sequence.id == 4 ? "walkforward" : sequence.id == 5 ? "runforward" : slot;
                    FileFormat::Animation::AnimationClipVariant variant;
                    variant.animationClipAssetID = XXHash64::hash(clipPath.data(), clipPath.size(), 0);
                    variant.selectionWeight = static_cast<u16>(std::max<i32>(1, sequence.frequency));
                    variant.stableVariationID = static_cast<u32>(variant.animationClipAssetID);
                    slots[semantic].push_back(variant);
                    FileFormat::Animation::ClipBounds clipBounds;
                    clipBounds.animationClipAssetID = variant.animationClipAssetID;
                    clipBounds.segmentOffset = static_cast<u32>(boundsData.segments.size());
                    clipBounds.numSegments = 1;
                    clipBounds.fallbackBounds = clip.bounds;
                    boundsData.clips.push_back(clipBounds);
                    boundsData.segments.push_back({0, clip.asset.durationMicroseconds, clip.bounds});
                    boundsMinimum = glm::min(boundsMinimum, clip.bounds.center - clip.bounds.extents);
                    boundsMaximum = glm::max(boundsMaximum, clip.bounds.center + clip.bounds.extents);
                }
            }
            auto& globalBounds = boundsData.clips.front().fallbackBounds;
            globalBounds.center = (boundsMinimum + boundsMaximum) * 0.5f;
            globalBounds.extents = (boundsMaximum - boundsMinimum) * 0.5f;
            globalBounds.sphereRadius = glm::length(globalBounds.extents);
            for (const std::string suffix : {"", ".preview"})
            {
                const std::string modelPath = "animations/wow/" + name + "/" + name + suffix + ".model";
                const std::string boundsPath = "animations/wow/" + name + "/" + name + suffix + ".animationbounds";
                FileFormat::Animation::AnimationBoundsAsset boundsAsset;
                boundsAsset.modelAssetID = XXHash64::hash(modelPath.data(), modelPath.size(), 0);
                boundsAsset.skeletonAssetID = skeletonID;
                boundsAsset.flags = FileFormat::Animation::AnimationBoundsAssetFlags_HasGlobalFallback;
                auto boundsBuffer = std::make_shared<Bytebuffer>(nullptr, boundsAsset.GetSerializedSize(boundsData));
                if (!boundsAsset.Save(boundsBuffer, boundsData) || !runtime->pactInfo.GetManifestForFile(runtime, boundsBuffer->writtenData).AddFile(runtime, boundsPath, boundsBuffer)) throw std::runtime_error("Animation bounds publication failed");
            }
            FileFormat::Animation::AnimationSetAsset animationSet;
            FileFormat::Animation::AnimationSetData setData;
            animationSet.declaredPoseDomainType = FileFormat::Animation::PoseDomainType::Skeleton;
            animationSet.declaredPoseDomainAssetID = skeletonID;
            for (const auto& [name, variants] : slots)
            {
                FileFormat::Animation::AnimationSetSlot slot;
                const std::string semantic = "animation-slot/" + name;
                slot.semanticID = XXHash64::hash(semantic.data(), semantic.size(), 0);
                slot.variantOffset = static_cast<u32>(setData.variants.size());
                slot.numVariants = static_cast<u32>(variants.size());
                slot.flags = FileFormat::Animation::AnimationSetSlotFlags_Required;
                setData.slots.push_back(slot);
                setData.variants.insert(setData.variants.end(), variants.begin(), variants.end());
            }
            auto setBuffer = std::make_shared<Bytebuffer>(nullptr, animationSet.GetSerializedSize(setData));
            const std::string setPath = "animations/wow/" + name + "/" + name + ".animationset";
            if (!animationSet.Save(setBuffer, setData) || !runtime->pactInfo.GetManifestForFile(runtime, setBuffer->writtenData).AddFile(runtime, setPath, setBuffer)) throw std::runtime_error("AnimationSet publication failed");
            const std::string familyAssetPath = "animation/bipedal_humanoid.rigfamily";
            animationSet.declaredPoseDomainType = FileFormat::Animation::PoseDomainType::RigFamily;
            animationSet.declaredPoseDomainAssetID = XXHash64::hash(familyAssetPath.data(), familyAssetPath.size(), 0);
            auto familySetBuffer = std::make_shared<Bytebuffer>(nullptr, animationSet.GetSerializedSize(setData));
            const std::string familySetPath = "animations/wow/" + name + "/" + name + ".family.animationset";
            if (!animationSet.Save(familySetBuffer, setData) || !runtime->pactInfo.GetManifestForFile(runtime, familySetBuffer->writtenData).AddFile(runtime, familySetPath, familySetBuffer)) throw std::runtime_error("Family AnimationSet publication failed");
            FileFormat::Animation::AnimationSetData referenceData;
            FileFormat::Animation::AnimationSetSlot danceSlot;
            const std::string danceSemantic = "animation-slot/dance";
            danceSlot.semanticID = XXHash64::hash(danceSemantic.data(), danceSemantic.size(), 0);
            danceSlot.numVariants = 1;
            danceSlot.flags = FileFormat::Animation::AnimationSetSlotFlags_Required;
            referenceData.slots.push_back(danceSlot);
            FileFormat::Animation::AnimationClipVariant danceVariant;
            const std::string dancePath = "animations/wow/" + name + "/dance-0.animationclip";
            danceVariant.animationClipAssetID = XXHash64::hash(dancePath.data(), dancePath.size(), 0);
            danceVariant.selectionWeight = 1;
            danceVariant.stableVariationID = static_cast<u32>(danceVariant.animationClipAssetID);
            referenceData.variants.push_back(danceVariant);
            for (const auto& [slotName, variants] : slots)
            {
                if (!slotName.starts_with("ambientface")) continue;
                FileFormat::Animation::AnimationSetSlot ambientSlot;
                const std::string semantic = "animation-slot/" + slotName;
                ambientSlot.semanticID = XXHash64::hash(semantic.data(), semantic.size(), 0);
                ambientSlot.variantOffset = static_cast<u32>(referenceData.variants.size());
                ambientSlot.numVariants = 1;
                ambientSlot.flags = FileFormat::Animation::AnimationSetSlotFlags_Required;
                referenceData.slots.push_back(ambientSlot);
                referenceData.variants.push_back(variants.front());
            }
            auto referenceBuffer = std::make_shared<Bytebuffer>(nullptr, animationSet.GetSerializedSize(referenceData));
            const std::string referencePath = "animations/wow/" + name + "/" + name + ".dance-reference.animationset";
            if (!animationSet.Save(referenceBuffer, referenceData) || !runtime->pactInfo.GetManifestForFile(runtime, referenceBuffer->writtenData).AddFile(runtime, referencePath, referenceBuffer)) throw std::runtime_error("Reference AnimationSet publication failed");
            // Single-variation sets keep every actor on the same clip in synchronized comparisons.
            const auto& danceVariants = slots.at("dance");
            for (u32 variation = 0; variation < danceVariants.size(); ++variation)
            {
                referenceData.variants[0] = danceVariants[variation];
                auto variationBuffer = std::make_shared<Bytebuffer>(nullptr, animationSet.GetSerializedSize(referenceData));
                const std::string variationPath = "animations/wow/" + name + "/" + name + ".dance-variant-" + std::to_string(variation) + ".animationset";
                if (!animationSet.Save(variationBuffer, referenceData) || !runtime->pactInfo.GetManifestForFile(runtime, variationBuffer->writtenData).AddFile(runtime, variationPath, variationBuffer)) throw std::runtime_error("Dance variation publication failed");
                inventory << "dance-comparison\t" << name << '\t' << variation << '\t' << variationPath << '\t' << danceVariants[variation].animationClipAssetID << '\n';
            }
            for (const auto& animation : model.afid.data)
            {
                if (animation.animID != 0 && animation.animID != 4 && animation.animID != 5 && animation.animID != 69) continue;
                if (!animation.fileID) continue;
                auto bytes = casc->GetFileByID(animation.fileID);
                SaveSource(output / "sources" / (name + "-" + std::to_string(animation.animID) + "-" + std::to_string(animation.variationID) + ".anim"), bytes, inventory, animation.fileID);
            }
            NC_LOG_INFO("[WoW Animation] Inventoried {}: {} bones, {} sequences", name, model.md21.bones.size, model.md21.sequences.size);
        }
        casc->Close();
        if (!ModelV2Builder::FlushPendingMaterials(runtime)) throw std::runtime_error("Material publication failed");
        if (!inventory) throw std::runtime_error("Inventory write failed");
        NC_LOG_INFO("[WoW Animation] Corpus inventory complete");
        return 0;
    }
    catch (const std::exception& error)
    {
        NC_LOG_ERROR("[WoW Animation] {}", error.what());
        std::cerr << "[WoW Animation] " << error.what() << std::endl;
        return 1;
    }
}
