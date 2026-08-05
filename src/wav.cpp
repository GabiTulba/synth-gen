#include "wav.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace synth {

namespace {

[[noreturn]] void bail(const std::string& path, const std::string& msg) {
  throw std::runtime_error("'" + path + "': " + msg);
}

uint32_t readU32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
uint16_t readU16(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

void putU32(std::string& s, uint32_t v) {
  s += (char)(v & 0xff);
  s += (char)((v >> 8) & 0xff);
  s += (char)((v >> 16) & 0xff);
  s += (char)((v >> 24) & 0xff);
}
void putU16(std::string& s, uint16_t v) {
  s += (char)(v & 0xff);
  s += (char)((v >> 8) & 0xff);
}

}  // namespace

WavData readWav(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) bail(path, "cannot open file");
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
      std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
    bail(path, "not a RIFF/WAVE file");

  uint16_t format = 0, channelCount = 0, bits = 0;
  uint32_t rate = 0;
  const uint8_t* data = nullptr;
  uint32_t dataLen = 0;

  size_t pos = 12;
  while (pos + 8 <= bytes.size()) {
    char id[5] = {0};
    std::memcpy(id, bytes.data() + pos, 4);
    uint32_t len = readU32(bytes.data() + pos + 4);
    const uint8_t* body = bytes.data() + pos + 8;
    if (pos + 8 + len > bytes.size()) len = (uint32_t)(bytes.size() - pos - 8);
    if (std::strcmp(id, "fmt ") == 0 && len >= 16) {
      format = readU16(body);
      channelCount = readU16(body + 2);
      rate = readU32(body + 4);
      bits = readU16(body + 14);
      if (format == 0xFFFE && len >= 40)  // WAVE_FORMAT_EXTENSIBLE
        format = readU16(body + 24);
    } else if (std::strcmp(id, "data") == 0) {
      data = body;
      dataLen = len;
    }
    pos += 8 + len + (len & 1);  // chunks are word-aligned
  }

  if (!rate || !channelCount) bail(path, "missing fmt chunk");
  if (!data) bail(path, "missing data chunk");
  if (format != 1 && format != 3)
    bail(path, "unsupported WAV format tag " + std::to_string(format) +
                   " (only PCM and IEEE float are supported)");

  uint32_t bytesPerSample = bits / 8;
  if (bytesPerSample == 0) bail(path, "invalid bit depth");
  uint64_t frameCount = dataLen / (bytesPerSample * channelCount);

  WavData out;
  out.rate = rate;
  out.channels.assign(channelCount, std::vector<double>((size_t)frameCount));
  for (uint64_t f = 0; f < frameCount; f++) {
    for (uint16_t c = 0; c < channelCount; c++) {
      const uint8_t* p = data + (f * channelCount + c) * bytesPerSample;
      double v = 0;
      if (format == 1 && bits == 16) {
        v = (int16_t)readU16(p) / 32768.0;
      } else if (format == 1 && bits == 24) {
        int32_t s = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                              ((uint32_t)p[2] << 16));
        if (s & 0x800000) s |= (int32_t)0xFF000000;
        v = s / 8388608.0;
      } else if (format == 1 && bits == 32) {
        v = (int32_t)readU32(p) / 2147483648.0;
      } else if (format == 3 && bits == 32) {
        float fv;
        std::memcpy(&fv, p, 4);
        v = fv;
      } else if (format == 3 && bits == 64) {
        double dv;
        std::memcpy(&dv, p, 8);
        v = dv;
      } else {
        bail(path, "unsupported bit depth " + std::to_string(bits));
      }
      out.channels[c][(size_t)f] = v;
    }
  }
  return out;
}

void writeWav(const std::string& path, double rate, int channelCount,
              const std::vector<double>& interleaved) {
  uint32_t sampleRate = (uint32_t)std::lround(rate);
  uint32_t dataLen = (uint32_t)(interleaved.size() * 2);
  std::string s;
  s.reserve(44 + dataLen);
  s += "RIFF";
  putU32(s, 36 + dataLen);
  s += "WAVE";
  s += "fmt ";
  putU32(s, 16);
  putU16(s, 1);  // PCM
  putU16(s, (uint16_t)channelCount);
  putU32(s, sampleRate);
  putU32(s, sampleRate * channelCount * 2);  // byte rate
  putU16(s, (uint16_t)(channelCount * 2));   // block align
  putU16(s, 16);                             // bits
  s += "data";
  putU32(s, dataLen);
  for (double v : interleaved) {
    double clamped = std::fmax(-1.0, std::fmin(1.0, v));
    int16_t q = (int16_t)std::lround(clamped * 32767.0);
    putU16(s, (uint16_t)q);
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("cannot open '" + path + "' for writing");
  out.write(s.data(), (std::streamsize)s.size());
  if (!out) throw std::runtime_error("failed writing '" + path + "'");
}

}  // namespace synth
