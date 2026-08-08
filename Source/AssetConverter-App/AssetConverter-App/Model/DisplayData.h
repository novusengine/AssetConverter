#pragma once

#include <Base/Types.h>

#include <FileFormat/Novus/ClientDB/ClientDB.h>
#include <FileFormat/Novus/Model/Model.h>

#include <string>
#include <vector>

namespace DisplayData
{
    enum class Source : u8
    {
        CreatureDisplayInfo = 0,
        ItemDisplayInfo = 1
    };

    // Registers one Game-owned display key with the Model selected by that key.
    // Game indexes these rows by (source, displayID, modelVariant).
    struct RegistrationRecord
    {
        u64 modelAssetID = 0;
        u32 displayID = 0;
        u8 source = 0;
        u8 modelVariant = 0;
        u16 reserved = 0;

        inline static const std::vector<ClientDB::FieldInfo> FIELD_LIST = {
            { "modelAssetID", ClientDB::FieldType::u64 },
            { "displayID", ClientDB::FieldType::u32 },
            { "source", ClientDB::FieldType::u8 },
            { "modelVariant", ClientDB::FieldType::u8 },
            { "reserved", ClientDB::FieldType::u16 }
        };
    };

    // Stores one typed Model-parameter override belonging to one display registration.
    // Multiple rows reference the same registration when a display overrides several values.
    struct ParameterOverrideRecord
    {
        u64 value0 = 0;
        u64 value1 = 0;
        u32 displayRegistrationID = 0;
        u32 modelParameterStableID = 0;
        u8 type = 0;
        u8 reserved[7] = {};

        inline static const std::vector<ClientDB::FieldInfo> FIELD_LIST = {
            { "value0", ClientDB::FieldType::u64 },
            { "value1", ClientDB::FieldType::u64 },
            { "displayRegistrationID", ClientDB::FieldType::u32 },
            { "modelParameterStableID", ClientDB::FieldType::u32 },
            { "type", ClientDB::FieldType::u8 },
            { "reserved", ClientDB::FieldType::u8, 7 }
        };
    };

    static_assert(sizeof(RegistrationRecord) == 16);
    static_assert(sizeof(ParameterOverrideRecord) == 32);
}
