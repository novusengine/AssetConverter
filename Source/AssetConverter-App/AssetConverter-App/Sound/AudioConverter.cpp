#include "AudioConverter.h"

#include <Base/Util/DebugHandler.h>
#include <Base/Util/StringUtils.h>

void ConvertToWAV(const std::shared_ptr<Bytebuffer>& buffer, const std::string& filePath)
{
    ma_result result;
    ma_decoder decoder;
    ma_encoder encoder;
    std::vector<i32> tempBuffer(4096);

    ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_s16, 0, 0);
    result = ma_decoder_init_memory(buffer->GetDataPointer(), buffer->writtenData, &decoderConfig, &decoder);
    if (result != MA_SUCCESS && result != MA_AT_END)
    {
        NC_LOG_ERROR("Failed to initialize audio decoder. Error code: {0}\n File: {}", static_cast<i32>(result), filePath);
        return;
    }

    ma_encoder_config encoderConfig = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, decoder.outputChannels, decoder.outputSampleRate);
    result = ma_encoder_init_file(filePath.c_str(), &encoderConfig, &encoder);
    if (result != MA_SUCCESS)
    {
        NC_LOG_ERROR("Failed to initialize WAV encoder. Error code: {0}\n File: {}", static_cast<i32>(result), filePath);
        ma_decoder_uninit(&decoder);
        return;
    }

    while (true)
    {
        size_t framesRead;
        result = ma_decoder_read_pcm_frames(&decoder, tempBuffer.data(), tempBuffer.size(), &framesRead);
        if (result != MA_SUCCESS && result != MA_AT_END)
        {
            NC_LOG_ERROR("Failed to read PCM frames. Error code: {0}\n File: {}", static_cast<i32>(result), filePath);
            return;
        }

        if (framesRead == 0)
        {
            break;
        }

        result = ma_encoder_write_pcm_frames(&encoder, tempBuffer.data(), tempBuffer.size(), &framesRead);
        if (result != MA_SUCCESS)
        {
            NC_LOG_ERROR("Failed to write PCM frames to encoder. Error code: {0}\n File: {}", static_cast<i32>(result), filePath);
            return;
        }
    }

    ma_encoder_uninit(&encoder);
    ma_decoder_uninit(&decoder);
}