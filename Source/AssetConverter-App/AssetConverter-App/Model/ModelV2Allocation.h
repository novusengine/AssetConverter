#pragma once

#include <FileFormat/Novus/Map/Map.h>
#include <FileFormat/Novus/Model/Model.h>

#include <shared_mutex>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ModelV2AllocationRecord
{
    Map::ModelResourceAllocationHints resources;
    u32 geometryGroupCount = 0;
    std::vector<FileFormat::Model::EmbeddedInstanceSet> embeddedInstanceSets;
    std::vector<FileFormat::Model::EmbeddedInstance> embeddedInstances;

    bool IsRenderable() const
    {
        return resources.meshes > 0 && resources.meshLODs > 0 && resources.meshlets > 0;
    }
};

// Process-local metadata published only after a Model V2 payload has been
// validated, serialized, and accepted by PACT. Map extraction consumes this
// instead of reopening model files or consulting legacy ComplexModel output.
class ModelV2AllocationRegistry
{
public:
    static void Reset()
    {
        State& state = GetState();
        std::scoped_lock lock(state.mutex);
        state.records.clear();
    }

    static void Register(u64 assetID, const FileFormat::Model::ModelAsset& asset,
        const FileFormat::Model::ModelData& data)
    {
        auto record = std::make_shared<ModelV2AllocationRecord>();
        record->resources.models = 1;
        record->resources.meshes = data.meshes.size();
        record->resources.meshLODs = data.meshLODs.size();
        record->resources.submeshes = data.submeshes.size();
        record->resources.meshlets = data.meshlets.size();
        record->resources.positions = data.positions.size();
        record->resources.vertexAttributes = data.vertexAttributes.size();
        record->resources.skinningRecords = data.skinningData.size();
        record->resources.meshletVertexIndices = data.meshletVertexIndices.size();
        record->resources.meshletTriangleRecords = data.meshletTriangles.size();
        record->resources.jointPaletteRemaps = data.jointPaletteRemaps.size();
        record->resources.materialSlots = data.materialSlots.size();
        record->resources.embeddedInstanceSets = data.embeddedInstanceSets.size();
        record->resources.embeddedInstanceRecords = data.embeddedInstances.size();
        record->geometryGroupCount = asset.geometryGroupCount;
        record->embeddedInstanceSets = data.embeddedInstanceSets;
        record->embeddedInstances = data.embeddedInstances;

        State& state = GetState();
        std::scoped_lock lock(state.mutex);
        state.records[assetID] = std::move(record);
    }

    static std::shared_ptr<const ModelV2AllocationRecord> Find(u64 assetID)
    {
        State& state = GetState();
        std::shared_lock lock(state.mutex);
        auto itr = state.records.find(assetID);
        if (itr == state.records.end())
            return {};
        return itr->second;
    }

private:
    struct State
    {
        std::shared_mutex mutex;
        std::unordered_map<u64, std::shared_ptr<const ModelV2AllocationRecord>> records;
    };

    static State& GetState()
    {
        static State state;
        return state;
    }
};

class ModelV2MapAllocationAccumulator
{
public:
    ModelV2MapAllocationAccumulator()
    {
        _hints.flags = Map::ModelAllocationHintFlags_SceneCountsAreUpperBounds;
    }

    void AddRootPlacement(u64 modelAssetID, u16 embeddedInstanceSetID)
    {
        ++_hints.scene.rootPlacements;

        const std::shared_ptr<const ModelV2AllocationRecord> root = ModelV2AllocationRegistry::Find(modelAssetID);
        if (!root || !root->IsRenderable())
            return;

        AddRenderableInstance(modelAssetID, *root);

        const FileFormat::Model::EmbeddedInstanceSet* selectedSet = nullptr;
        for (const FileFormat::Model::EmbeddedInstanceSet& set : root->embeddedInstanceSets)
        {
            if (set.stableID == embeddedInstanceSetID)
            {
                selectedSet = &set;
                break;
            }
        }
        if (!selectedSet || static_cast<u64>(selectedSet->instanceOffset) + selectedSet->numInstances > root->embeddedInstances.size())
            return;

        for (u32 index = 0; index < selectedSet->numInstances; ++index)
        {
            const u64 childAssetID = root->embeddedInstances[selectedSet->instanceOffset + index].modelAssetID;
            std::shared_ptr<const ModelV2AllocationRecord> child;
            if (childAssetID == FileFormat::INVALID_ASSET_ID ||
                !(child = ModelV2AllocationRegistry::Find(childAssetID)) || !child->IsRenderable())
                continue;

            AddRenderableInstance(childAssetID, *child);
            ++_hints.scene.selectedRenderableEmbeddedInstances;
        }
    }

    void Merge(const ModelV2MapAllocationAccumulator& other)
    {
        _hints.scene += other._hints.scene;
        _hints.flags |= other._hints.flags;
        for (u64 assetID : other._uniqueAssets)
        {
            if (!_uniqueAssets.insert(assetID).second)
                continue;
            const std::shared_ptr<const ModelV2AllocationRecord> record = ModelV2AllocationRegistry::Find(assetID);
            if (record)
                _hints.resources += record->resources;
        }
    }

    const Map::ModelAllocationHints& GetHints() const { return _hints; }

private:
    void AddRenderableInstance(u64 assetID, const ModelV2AllocationRecord& record)
    {
        ++_hints.scene.totalModelInstances;
        _hints.scene.geometryGroupMaskWords += (static_cast<u64>(record.geometryGroupCount) + 31u) / 32u;
        _hints.scene.meshletHistoryWords += (record.resources.meshlets + 31u) / 32u;

        if (_uniqueAssets.insert(assetID).second)
            _hints.resources += record.resources;
    }

    Map::ModelAllocationHints _hints;
    std::unordered_set<u64> _uniqueAssets;
};
