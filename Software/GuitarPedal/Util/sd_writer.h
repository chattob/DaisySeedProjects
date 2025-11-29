#pragma once

#include "daisy_seed.h"
#include "fatfs.h"
#include "../../libDaisy/src/util/WavWriter.h"

using namespace daisy;

class SDWriter
{
  public:
    struct Config
    {
        float sample_rate;
        uint16_t num_channels;
        uint16_t bits_per_sample;
    };

    SDWriter();
    ~SDWriter();
    void Init();//daisy::FatFSInterface& fsi, FIL* sd_file, daisy::SdmmcHandler& sd, const Config& cfg);
    void    StartWrite(const char* filename, const Config& cfg, const float* buffer, size_t totalSamples);
    void    PushSample();         // call this in audio callback
    bool    WriteFloatPoll();     // call this in main loop
    void    Close();
    bool    IsWriting() const;

  private:
    static SdmmcHandler   sd_;
    static FatFSInterface fsi;
    static FIL            SDFile;
    static WavWriter<16384> writer_;
    Config cfg_;

    // internal state
    const float* buffer_       = nullptr;
    size_t       totalSamples_ = 0;
    size_t       writeIndex_   = 0;
    bool         opened_       = false;
    bool         done_         = false;
};