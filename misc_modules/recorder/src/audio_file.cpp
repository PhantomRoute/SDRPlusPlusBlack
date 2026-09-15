#include "audio_file.h"
#include <FLAC/stream_encoder.h>
#include <lame.h>
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace recorder_audio {
    FILE* openForWriting(const std::string& path) {
#ifdef _WIN32
        std::wstring wide = std::filesystem::u8path(path).wstring();
        return _wfopen(wide.c_str(), L"w+b");
#else
        return fopen(path.c_str(), "w+b");
#endif
    }

    // ---- FLAC

    bool FlacFile::open(const std::string& path, int ch, int samplerate) {
        std::lock_guard<std::mutex> lck(mtx);
        if (encoder) { return false; }
        channels = ch;
        FLAC__StreamEncoder* enc = FLAC__stream_encoder_new();
        if (!enc) {
            error = "Could not start the FLAC encoder.";
            return false;
        }
        FLAC__stream_encoder_set_channels(enc, (uint32_t)ch);
        FLAC__stream_encoder_set_bits_per_sample(enc, (uint32_t)bits);
        FLAC__stream_encoder_set_sample_rate(enc, (uint32_t)samplerate);
        // The default level: most of the saving of the slowest, at a fraction of the work.
        FLAC__stream_encoder_set_compression_level(enc, 5);
        FLAC__stream_encoder_set_verify(enc, false);

        FILE* f = openForWriting(path);
        if (!f) {
            FLAC__stream_encoder_delete(enc);
            error = "Could not create " + path;
            return false;
        }
        FLAC__StreamEncoderInitStatus status = FLAC__stream_encoder_init_FILE(
            enc, f, [](const FLAC__StreamEncoder* e, FLAC__uint64 written, FLAC__uint64, uint32_t, uint32_t, void* ctx) {
                FlacFile::progress(e, written, ctx);
            }, this);
        if (status == FLAC__STREAM_ENCODER_INIT_STATUS_NOT_STREAMABLE) {
            // An unusual sample rate can fall outside the streamable subset, which only
            // matters to hardware players. Drop the restriction rather than refuse.
            FLAC__stream_encoder_set_streamable_subset(enc, false);
            status = FLAC__stream_encoder_init_FILE(
                enc, f, [](const FLAC__StreamEncoder* e, FLAC__uint64 written, FLAC__uint64, uint32_t, uint32_t, void* ctx) {
                    FlacFile::progress(e, written, ctx);
                }, this);
        }
        if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
            error = std::string("The FLAC encoder would not start: ") + FLAC__StreamEncoderInitStatusString[status];
            // The encoder took the file the moment init was called, even though init
            // failed, and closes it on delete. Closing it here as well would close it twice.
            FLAC__stream_encoder_delete(enc);
            // Nothing was ever written to the file just created; do not leave it behind.
            std::error_code ec;
            std::filesystem::remove(std::filesystem::u8path(path), ec);
            return false;
        }
        encoder = enc;
        frames = 0;
        bytes = 0;
        failed = false;
        return true;
    }

    void FlacFile::progress(const void*, uint64_t bytesWritten, void* ctx) {
        ((FlacFile*)ctx)->bytes.store(bytesWritten, std::memory_order_relaxed);
    }

    void FlacFile::write(const float* samples, int count) {
        std::lock_guard<std::mutex> lck(mtx);
        if (!encoder || failed || count <= 0) { return; }
        size_t n = (size_t)count * (size_t)channels;
        if (buffer.size() < n) { buffer.resize(n); }
        const float scale = (float)((1 << (bits - 1)) - 1);
        for (size_t i = 0; i < n; i++) {
            float v = std::clamp<float>(samples[i], -1.0f, 1.0f);
            buffer[i] = (int32_t)lrintf(v * scale);
        }
        if (!FLAC__stream_encoder_process_interleaved((FLAC__StreamEncoder*)encoder, buffer.data(), (uint32_t)count)) {
            failed = true;
            return;
        }
        frames.fetch_add((uint64_t)count, std::memory_order_relaxed);
    }

    void FlacFile::close() {
        std::lock_guard<std::mutex> lck(mtx);
        if (!encoder) { return; }
        FLAC__StreamEncoder* enc = (FLAC__StreamEncoder*)encoder;
        // Writes what is left, goes back to fill in the stream header, and closes the
        // file it was given.
        if (!FLAC__stream_encoder_finish(enc)) { failed = true; }
        FLAC__stream_encoder_delete(enc);
        encoder = nullptr;
    }

    // ---- MP3

    bool Mp3File::open(const std::string& path, int ch, int samplerate) {
        std::lock_guard<std::mutex> lck(mtx);
        if (lame) { return false; }
        channels = ch;
        lame_t l = lame_init();
        if (!l) {
            error = "Could not start the MP3 encoder.";
            return false;
        }
        lame_set_in_samplerate(l, samplerate);
        lame_set_num_channels(l, ch);
        lame_set_mode(l, (ch == 1) ? MONO : JOINT_STEREO);
        lame_set_VBR(l, vbr_off);
        lame_set_brate(l, kbps);
        // 2 is near the best LAME does; audio from a radio is a light load for it.
        lame_set_quality(l, 2);
        // No tags and no Xing header: a constant bitrate file needs neither to seek, and
        // leaving them out means nothing has to be rewritten at the start on close.
        lame_set_write_id3tag_automatic(l, 0);
        lame_set_bWriteVbrTag(l, 0);
        if (lame_init_params(l) < 0) {
            lame_close(l);
            error = "The MP3 encoder does not accept these settings.";
            return false;
        }
        file = openForWriting(path);
        if (!file) {
            lame_close(l);
            error = "Could not create " + path;
            return false;
        }
        lame = l;
        frames = 0;
        bytes = 0;
        failed = false;
        return true;
    }

    void Mp3File::put(const unsigned char* data, int count) {
        if (count <= 0 || failed) { return; }
        if (fwrite(data, 1, (size_t)count, file) != (size_t)count) {
            failed = true;
            return;
        }
        bytes.fetch_add((uint64_t)count, std::memory_order_relaxed);
    }

    void Mp3File::write(const float* samples, int count) {
        std::lock_guard<std::mutex> lck(mtx);
        if (!lame || failed || count <= 0) { return; }
        // LAME's own worst case for the output of one call.
        size_t need = (size_t)(1.25 * count) + 7200;
        if (out.size() < need) { out.resize(need); }
        int produced;
        if (channels == 1) {
            produced = lame_encode_buffer_ieee_float(lame, samples, samples, count, out.data(), (int)out.size());
        }
        else {
            produced = lame_encode_buffer_interleaved_ieee_float(lame, samples, count, out.data(), (int)out.size());
        }
        if (produced < 0) {
            failed = true;
            return;
        }
        put(out.data(), produced);
        frames.fetch_add((uint64_t)count, std::memory_order_relaxed);
    }

    void Mp3File::close() {
        std::lock_guard<std::mutex> lck(mtx);
        if (!lame) { return; }
        if (out.size() < 7200) { out.resize(7200); }
        int produced = lame_encode_flush(lame, out.data(), (int)out.size());
        put(out.data(), produced);
        lame_close(lame);
        lame = nullptr;
        if (file) {
            if (fclose(file) != 0) { failed = true; }
            file = nullptr;
        }
    }
}
