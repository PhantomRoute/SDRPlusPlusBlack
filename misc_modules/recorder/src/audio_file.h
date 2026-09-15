#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

struct lame_global_struct;

// Compressed audio files for the recorder. WAV stays with wav::Writer, which baseband
// recordings use too; these only ever take audio.
namespace recorder_audio {
    class EncodedFile {
    public:
        virtual ~EncodedFile() = default;

        // Creates the file and gets the encoder ready. False, with lastError() saying
        // why, when either cannot be done.
        virtual bool open(const std::string& path, int channels, int samplerate) = 0;

        // frames of interleaved samples, full scale at +-1. Anything louder is clipped.
        virtual void write(const float* samples, int frames) = 0;

        // Finishes the stream and closes the file. Safe to call more than once.
        virtual void close() = 0;

        uint64_t framesWritten() const { return frames.load(std::memory_order_relaxed); }
        uint64_t bytesWritten() const { return bytes.load(std::memory_order_relaxed); }
        // True once a write to the file has failed - a full disk, a pulled drive.
        // Nothing after that reaches the file.
        bool writeFailed() const { return failed.load(std::memory_order_relaxed); }
        const std::string& lastError() const { return error; }

    protected:
        std::mutex mtx;
        std::atomic<uint64_t> frames{ 0 };
        std::atomic<uint64_t> bytes{ 0 };
        std::atomic<bool> failed{ false };
        std::string error;
    };

    // Lossless. bits is 16 or 24.
    class FlacFile : public EncodedFile {
    public:
        explicit FlacFile(int bits) : bits(bits) {}
        ~FlacFile() override { close(); }
        bool open(const std::string& path, int channels, int samplerate) override;
        void write(const float* samples, int frames) override;
        void close() override;

    private:
        static void progress(const void* encoder, uint64_t bytesWritten, void* ctx);

        void* encoder = nullptr; // FLAC__StreamEncoder, kept out of this header
        int bits;
        int channels = 1;
        std::vector<int32_t> buffer;
    };

    // Lossy, at a constant bitrate in kbit/s.
    class Mp3File : public EncodedFile {
    public:
        explicit Mp3File(int kbps) : kbps(kbps) {}
        ~Mp3File() override { close(); }
        bool open(const std::string& path, int channels, int samplerate) override;
        void write(const float* samples, int frames) override;
        void close() override;

    private:
        void put(const unsigned char* data, int count);

        lame_global_struct* lame = nullptr;
        FILE* file = nullptr;
        int kbps;
        int channels = 1;
        std::vector<unsigned char> out;
    };

    // fopen for a UTF-8 path, which on Windows means going through the wide API: the
    // narrow one reads the path in the local code page, and a folder with an accented
    // name in it then cannot be written to at all.
    FILE* openForWriting(const std::string& path);
}
