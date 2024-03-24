#pragma once

#include <Base/Types.h>
#include <Base/Memory/Bytebuffer.h>

#include <Jolt/Jolt.h>
#include <Jolt/Core/StreamOut.h>

class JoltStream : public JPH::StreamOut
{
public:
    JoltStream(std::shared_ptr<Bytebuffer>& buffer);
    ~JoltStream();

    virtual void WriteBytes(const void* inData, size_t inNumBytes) override;
    virtual bool IsFailed() const override;

private:
    bool _didFail = false;
    std::shared_ptr<Bytebuffer> _buffer = nullptr;
};