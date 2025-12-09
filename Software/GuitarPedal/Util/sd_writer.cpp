#include "sd_writer.h"

static constexpr size_t TRANSFER_SIZE = 16384;

// Force these into AXI SRAM (not DTCM)
DSY_SDRAM_BSS SdmmcHandler   SDWriter::sd_;
DSY_SDRAM_BSS FatFSInterface SDWriter::fsi;
DSY_SDRAM_BSS MyWavWriter<16384> SDWriter::writer_;

// Default Constructor
SDWriter::SDWriter() {
    // Because .sram1_bss and .sdram_bss are NOLOAD, clear what we rely on:
    memset(&writer_, 0, sizeof(writer_));
}

// Destructor
SDWriter::~SDWriter() {
    // No Code Needed
}

void SDWriter::Init() {
    // Init SD Card
    SdmmcHandler::Config sd_cfg;
    sd_cfg.Defaults();
    //sd_cfg.speed = SdmmcHandler::Speed::SLOW;
    sd_cfg.width = SdmmcHandler::BusWidth::BITS_1;
    sd_.Init(sd_cfg);

    // Links libdaisy i/o to fatfs driver.
    fsi.Init(FatFSInterface::Config::MEDIA_SD);

    // Mount SD Card
    FRESULT res = f_mount(&fsi.GetSDFileSystem(), "/", 1);
    if(res != FR_OK) {
        status_ = SDWriter::INIT_FAILED;
    } else {
        status_ = SDWriter::IDLE;
    }
}

bool SDWriter::StartWrite(const char* filename, const Config& cfg, const float* buffer, size_t totalSamples)
{
    if (status_ != IDLE || buffer == nullptr || totalSamples == 0) {
        return false;   // either busy or invalid params
    }

    cfg_ = cfg;

    MyWavWriter<TRANSFER_SIZE>::Config wcfg;
    wcfg.samplerate    = static_cast<float>(cfg.sample_rate);
    wcfg.channels      = cfg.num_channels;
    wcfg.bitspersample = cfg.bits_per_sample;

    writer_.Init(wcfg);
    writer_.OpenFile(filename);

    if (!writer_.IsRecording()) {
        status_ = ERROR;
        opened_ = false;
        done_   = true;
        return false;
    }
    
    buffer_       = buffer;
    totalSamples_ = totalSamples;
    writeIndex_   = 0;
    done_         = false;
    opened_       = true;
    status_       = WRITING;

    return true;
}

void SDWriter::PushSample()
{
    if(!opened_ || done_ || buffer_ == nullptr || status_ != WRITING) {
        return;
    }

    if(writeIndex_ >= totalSamples_) {
        done_ = true;
        return;
    }

    float sample = buffer_[writeIndex_++];
    writer_.Sample(&sample); // Push single-sample frame
}

bool SDWriter::WriteFloatPoll()
{
    if(!opened_ || status_ != WRITING) {
        return false;
    }

    writer_.Write();              // drain if a half is ready

    if(done_){
        // one last drain in case the last half was marked while we raced
        writer_.Write();
        return true;              // signal caller to Close()
    }

    if(writeIndex_ >= totalSamples_) {
        done_ = true;
    }

    return done_;
}

void SDWriter::Close()
{
    if(!opened_) {
        return;
    }

    // final drain before header rewrite
    writer_.Write();
    writer_.SaveFile();
    opened_ = false;
    done_   = true;

    if(status_ == WRITING) {
        status_ = IDLE;
    }
}
