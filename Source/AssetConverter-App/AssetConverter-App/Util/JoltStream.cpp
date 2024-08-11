#include "JoltStream.h"

#include <Base/Util/DebugHandler.h>

JoltStream::JoltStream(std::shared_ptr<Bytebuffer>& buffer)
{
    _buffer = buffer;
    _didFail = false;
}

JoltStream::~JoltStream()
{
    _buffer = nullptr;
}

void JoltStream::WriteBytes(const void* inData, size_t inNumBytes)
{
    if (!_buffer)
        return;

    if (!_buffer->PutBytes(inData, inNumBytes))
    {
        _didFail = true;
        NC_LOG_ERROR("Failed to write bytes to JoltStream");
    }
}

bool JoltStream::IsFailed() const
{
    return _didFail;
}
