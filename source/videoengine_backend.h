#pragma once

#include "videoengine.h"

#include <memory>
#include <string>
#include <vector>

namespace ReasonVideo {

class IVideoDecoderSession
{
public:
    virtual ~IVideoDecoderSession () = default;

    virtual bool open (const std::string& path, std::string& outError) = 0;
    virtual void close () = 0;
    virtual bool isOpen () const = 0;

    virtual int getWidth () const = 0;
    virtual int getHeight () const = 0;
    virtual double getFps () const = 0;
    virtual double getDurationSeconds () const = 0;

    virtual bool seekTo (double targetSec) = 0;
    virtual bool decodeNextFrame (PixelOrder order,
                                  std::vector<uint8_t>& outPixels,
                                  double& outPtsSec,
                                  bool& outEof) = 0;
};

class IAudioDecoderSession
{
public:
    virtual ~IAudioDecoderSession () = default;

    virtual bool open (const std::string& path) = 0;
    virtual void close () = 0;
    virtual bool isOpen () const = 0;

    virtual bool seekTo (double targetSec) = 0;
    virtual bool readNextChunk (int targetOutRate,
                                std::vector<float>& outInterleavedStereo,
                                double& outStartSec,
                                bool& outEof) = 0;
};

std::unique_ptr<IVideoDecoderSession> createVideoDecoderSession ();
std::unique_ptr<IAudioDecoderSession> createAudioDecoderSession ();

} // namespace ReasonVideo
