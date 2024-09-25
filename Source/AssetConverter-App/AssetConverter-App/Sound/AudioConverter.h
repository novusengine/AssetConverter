#pragma once

#include <Base/Types.h>
#include <Base/Memory/Bytebuffer.h>

#include <miniaudio/miniaudio.h>

void ConvertToWAV(const std::shared_ptr<Bytebuffer>& buffer, const std::string& fileName);