// Copyright 2018 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.
//
// Disclaimer: This is not an official Google product.

#include "codec_png.h"

#include <string>

#include "third_party/lodepng/lodepng.h"
#include "byte_order.h"
#include "common.h"
#include "external_image.h"

namespace pik {
namespace {

#define PIK_PNG_VERBOSE 0

// Retrieves XMP and EXIF/IPTC from itext and text.
class MetadataReaderPNG {
 public:
  static Status Decode(const LodePNGInfo& info, Metadata* metadata) {
    for (unsigned idx_itext = 0; idx_itext < info.itext_num; ++idx_itext) {
      // We trust these are properly null-terminated by LodePNG.
      const char* key = info.itext_keys[idx_itext];
      const char* value = info.itext_strings[idx_itext];
      if (strstr(key, "XML:com.adobe.xmp")) {
        metadata->xmp.resize(strlen(value));  // safe, see above
        memcpy(metadata->xmp.data(), value, metadata->xmp.size());
      }
    }

    for (unsigned idx_text = 0; idx_text < info.text_num; ++idx_text) {
      // We trust these are properly null-terminated by LodePNG.
      const char* key = info.text_keys[idx_text];
      const char* value = info.text_strings[idx_text];
      std::string type;
      PaddedBytes bytes;
      if (DecodeBase16(key, value, &type, &bytes)) {
        if (type == "exif") {
          if (!metadata->exif.empty()) {
            fprintf(
                stderr,
                "WARNING: overwriting EXIF (%zu bytes) with base16 (%zu bytes)",
                metadata->exif.size(), bytes.size());
          }
          metadata->exif = std::move(bytes);
        } else if (type == "iptc") {
          metadata->iptc = std::move(bytes);
        } else if (type == "xmp") {
          // Generated by ImageMagick.
          metadata->xmp.resize(strlen(value));  // safe, see above
          memcpy(metadata->xmp.data(), value, metadata->xmp.size());
        } else {
          fprintf(stderr, "Unknown metadata %s: %zu bytes\n", type.c_str(),
                  bytes.size());
        }
      }
    }

    return true;
  }

 private:
  // Returns false if invalid.
  static PIK_INLINE Status DecodeNibble(const char c,
                                        uint32_t* PIK_RESTRICT nibble) {
    if ('a' <= c && c <= 'f') {
      *nibble = 10 + c - 'a';
    } else if ('0' <= c && c <= '9') {
      *nibble = c - '0';
    } else {
      *nibble = 0;
      return PIK_FAILURE("Invalid metadata nibble");
    }
    PIK_ASSERT(*nibble < 16);
    return true;
  }

  // We trust key and encoded are null-terminated because they come from
  // LodePNG.
  static Status DecodeBase16(const char* key, const char* encoded,
                             std::string* type, PaddedBytes* bytes) {
    const char* encoded_end = encoded + strlen(encoded);

    const char* kKey = "Raw profile type ";
    if (strncmp(key, kKey, strlen(kKey)) != 0) return false;
    *type = key + strlen(kKey);
    const size_t kMaxTypeLen = 20;
    if (type->length() > kMaxTypeLen) return PIK_FAILURE("Type too long");

    // GroupHeader: type and number of bytes
    char format[10 + kMaxTypeLen];
    snprintf(format, sizeof(format), "\n%s\n%%8lu%%n", type->c_str());
    unsigned long bytes_to_decode;
    int header_len;
    const int fields = sscanf(encoded, format, &bytes_to_decode, &header_len);
    if (fields != 1) return PIK_FAILURE("Failed to decode metadata header");
    PIK_ASSERT(bytes->empty());
    bytes->reserve(bytes_to_decode);

    // Encoding: base16 with newline after 72 chars.
    const char* pos = encoded + header_len;
    for (size_t i = 0; i < bytes_to_decode; ++i) {
      if (i % 36 == 0) {
        if (pos + 1 >= encoded_end) return PIK_FAILURE("Truncated base16 1");
        if (*pos != '\n') return PIK_FAILURE("Expected newline");
        ++pos;
      }

      if (pos + 2 >= encoded_end) return PIK_FAILURE("Truncated base16 2");
      uint32_t nibble0, nibble1;
      PIK_RETURN_IF_ERROR(DecodeNibble(pos[0], &nibble0));
      PIK_RETURN_IF_ERROR(DecodeNibble(pos[1], &nibble1));
      bytes->push_back(static_cast<uint8_t>((nibble1 << 4) + nibble0));
      pos += 2;
    }
    if (pos + 1 != encoded_end) return PIK_FAILURE("Too many encoded bytes");
    if (pos[0] != '\n') return PIK_FAILURE("Incorrect metadata terminator");
    return true;
  }
};

// Stores XMP and EXIF/IPTC into itext and text.
class MetadataWriterPNG {
 public:
  static Status Encode(const Metadata& metadata,
                       LodePNGInfo* PIK_RESTRICT info) {
    if (!metadata.exif.empty()) {
      PIK_RETURN_IF_ERROR(EncodeBase16("exif", metadata.exif, info));
    }
    if (!metadata.iptc.empty()) {
      PIK_RETURN_IF_ERROR(EncodeBase16("iptc", metadata.iptc, info));
    }

    if (!metadata.xmp.empty()) {
      const char* key = "XML:com.adobe.xmp";
      const std::string text(reinterpret_cast<const char*>(metadata.xmp.data()),
                             metadata.xmp.size());
      if (lodepng_add_itext(info, key, "", "", text.c_str()) != 0) {
        return PIK_FAILURE("Failed to add itext");
      }
    }

    return true;
  }

 private:
  static PIK_INLINE char EncodeNibble(const uint8_t nibble) {
    PIK_ASSERT(nibble < 16);
    return (nibble < 10) ? '0' + nibble : 'a' + nibble - 10;
  }

  static Status EncodeBase16(const std::string& type, const PaddedBytes& bytes,
                             LodePNGInfo* PIK_RESTRICT info) {
    // Encoding: base16 with newline after 72 chars.
    const size_t base16_size =
        2 * bytes.size() + DivCeil(bytes.size(), size_t(36)) + 1;
    std::string base16;
    base16.reserve(base16_size);
    for (size_t i = 0; i < bytes.size(); ++i) {
      if (i % 36 == 0) base16.push_back('\n');
      base16.push_back(EncodeNibble(bytes[i] & 0x0F));
      base16.push_back(EncodeNibble(bytes[i] >> 4));
    }
    base16.push_back('\n');
    PIK_ASSERT(base16.length() == base16_size);

    char key[30];
    snprintf(key, sizeof(key), "Raw profile type %s", type.c_str());

    char header[30];
    snprintf(header, sizeof(header), "\n%s\n%8lu", type.c_str(), bytes.size());

    const std::string& encoded = std::string(header) + base16;
    if (lodepng_add_text(info, key, encoded.c_str()) != 0) {
      return PIK_FAILURE("Failed to add text");
    }

    return true;
  }
};

// Retrieves ColorEncoding from PNG chunks.
class ColorEncodingReaderPNG {
 public:
  // Sets c_original or returns false.
  Status operator()(const PaddedBytes& bytes, const bool is_gray,
                    Metadata* metadata, ColorEncoding* c_original) {
    PIK_RETURN_IF_ERROR(Decode(bytes, metadata));

    const ColorSpace color_space =
        is_gray ? ColorSpace::kGray : ColorSpace::kRGB;

    if (have_pq_) {
      ProfileParams pp;
      pp.color_space = color_space;
      if (!WhitePointToCIExy(WhitePoint::kD65, &pp.white_point) ||
          !PrimariesToCIExy(Primaries::k2020, &pp.primaries)) {
        PIK_NOTIFY_ERROR("Failed to set white point/primaries");
      }
      pp.gamma = GammaPQ();
      pp.rendering_intent = RenderingIntent::kRelative;
      if (ColorManagement::SetFromParams(pp, c_original)) return true;
      fprintf(stderr, "Failed to synthesize BT.2100 PQ.\n");
      // Else: try the actual ICC profile.
    }

    // ICC overrides anything else if present.
    if (ColorManagement::SetFromProfile(std::move(icc_), c_original)) {
      if (have_srgb_) {
        fprintf(stderr, "Invalid PNG with both sRGB and ICC; ignoring sRGB.\n");
      }
      if (is_gray != c_original->IsGray()) {
        return PIK_FAILURE("Mismatch between ICC and PNG header");
      }
      return true;  // it's fine to ignore gAMA/cHRM.
    }

    // PNG requires that sRGB override gAMA/cHRM.
    if (have_srgb_) {
      c_original->rendering_intent = params_.rendering_intent;
      c_original->SetSRGB(color_space);
      return ColorManagement::SetProfileFromFields(c_original);
    }

    // Try to create a custom profile:

    params_.color_space = color_space;

    if (!have_chrm_) {
#if PIK_PNG_VERBOSE >= 1
      fprintf(stderr, "No cHRM, assuming sRGB.\n");
#endif
      if (!WhitePointToCIExy(WhitePoint::kD65, &params_.white_point) ||
          !PrimariesToCIExy(Primaries::kSRGB, &params_.primaries)) {
        PIK_ASSERT(false);  // should always succeed with known enum
      }
    }

    if (!have_gama_ || params_.gamma <= 0.0 || params_.gamma > 1.0) {
#if PIK_PNG_VERBOSE >= 1
      fprintf(stderr, "No (valid) gAMA nor sRGB, assuming sRGB.\n");
#endif
      params_.gamma = GammaSRGB();
    }

    params_.rendering_intent = RenderingIntent::kPerceptual;
    if (ColorManagement::SetFromParams(params_, c_original)) return true;

    fprintf(stderr,
            "DATA LOSS: unable to create an ICC profile for PNG gAMA/cHRM."
            "Image pixels will be interpreted as sRGB. Please add an ICC"
            "profile to the input image.\n");
    c_original->SetSRGB(color_space);
    return ColorManagement::SetProfileFromFields(c_original);
  }

 private:
  Status DecodeICC(const unsigned char* const payload,
                   const size_t payload_size) {
    if (payload_size == 0) return PIK_FAILURE("Empty ICC payload");
    const unsigned char* pos = payload;
    const unsigned char* end = payload + payload_size;

    // Profile name
    if (*pos == '\0') return PIK_FAILURE("Expected ICC name");
    for (size_t i = 0;; ++i) {
      if (i == 80) return PIK_FAILURE("ICC profile name too long");
      if (pos == end) return PIK_FAILURE("Not enough bytes for ICC name");
      if (*pos++ == '\0') break;
    }

    // Special case for BT.2100 PQ (https://w3c.github.io/png-hdr-pq/) - try to
    // synthesize the profile because table-based curves are less accurate.
    // strcmp is safe because we already verified the string is 0-terminated.
    if (!strcmp(reinterpret_cast<const char*>(payload), "ITUR_2100_PQ_FULL")) {
      have_pq_ = true;
    }

    // Skip over compression method (only one is allowed)
    if (pos == end) return PIK_FAILURE("Not enough bytes for ICC method");
    if (*pos++ != 0) return PIK_FAILURE("Unsupported ICC method");

    // Decompress
    unsigned char* icc_buf = nullptr;
    size_t icc_size = 0;
    LodePNGDecompressSettings settings;
    lodepng_decompress_settings_init(&settings);
    const unsigned err = lodepng_zlib_decompress(
        &icc_buf, &icc_size, pos, payload_size - (pos - payload), &settings);
    if (err == 0) {
      icc_.resize(icc_size);
      memcpy(icc_.data(), icc_buf, icc_size);
    }
    free(icc_buf);
    return true;
  }

  // Returns floating-point value from the PNG encoding (times 10^5).
  static double F64FromU32(const uint32_t x) {
    return static_cast<int32_t>(x) * 1E-5;
  }

  Status DecodeSRGB(const unsigned char* payload, const size_t payload_size) {
    if (payload_size != 1) return PIK_FAILURE("Wrong sRGB size");
    // (PNG uses the same values as ICC.)
    params_.rendering_intent = static_cast<RenderingIntent>(payload[0]);
    have_srgb_ = true;
    return true;
  }

  Status DecodeGAMA(const unsigned char* payload, const size_t payload_size) {
    if (payload_size != 4) return PIK_FAILURE("Wrong gAMA size");
    params_.gamma = F64FromU32(LoadBE32(payload));
    have_gama_ = true;
    return true;
  }

  Status DecodeCHRM(const unsigned char* payload, const size_t payload_size) {
    if (payload_size != 32) return PIK_FAILURE("Wrong cHRM size");
    params_.white_point.x = F64FromU32(LoadBE32(payload + 0));
    params_.white_point.y = F64FromU32(LoadBE32(payload + 4));
    params_.primaries.r.x = F64FromU32(LoadBE32(payload + 8));
    params_.primaries.r.y = F64FromU32(LoadBE32(payload + 12));
    params_.primaries.g.x = F64FromU32(LoadBE32(payload + 16));
    params_.primaries.g.y = F64FromU32(LoadBE32(payload + 20));
    params_.primaries.b.x = F64FromU32(LoadBE32(payload + 24));
    params_.primaries.b.y = F64FromU32(LoadBE32(payload + 28));
    have_chrm_ = true;
    return true;
  }

  Status DecodeEXIF(const unsigned char* payload, const size_t payload_size,
                    Metadata* metadata) {
    // If we already have EXIF, keep the larger one.
    if (metadata->exif.size() > payload_size) return true;
    metadata->exif.resize(payload_size);
    memcpy(metadata->exif.data(), payload, payload_size);
    return true;
  }

  Status Decode(const PaddedBytes& bytes, Metadata* metadata) {
    // Look for colorimetry and metadata chunks in the PNG image. The PNG chunks
    // begin after the PNG magic header of 8 bytes.
    const unsigned char* chunk = bytes.data() + 8;
    const unsigned char* end = bytes.data() + bytes.size();
    for (;;) {
      // chunk points to the first field of a PNG chunk. The chunk has
      // respectively 4 bytes of length, 4 bytes type, length bytes of data,
      // 4 bytes CRC.
      if (chunk + 4 >= end) {
        break;  // Regular end reached.
      }

      char type_char[5];
      lodepng_chunk_type(type_char, chunk);
      std::string type = type_char;

      if (type == "eXIf" || type == "iCCP" || type == "sRGB" ||
          type == "gAMA" || type == "cHRM") {
        const unsigned char* payload = lodepng_chunk_data_const(chunk);
        const size_t payload_size = lodepng_chunk_length(chunk);
        // The entire chunk needs also 4 bytes of CRC after the payload.
        if (payload + payload_size + 4 >= end) {
          PIK_NOTIFY_ERROR("PNG: truncated chunk");
          break;
        }
        if (lodepng_chunk_check_crc(chunk) != 0) {
          PIK_NOTIFY_ERROR("CRC mismatch in unknown PNG chunk");
          continue;
        }

        if (type == "eXIf") {
          PIK_RETURN_IF_ERROR(DecodeEXIF(payload, payload_size, metadata));
        } else if (type == "iCCP") {
          PIK_RETURN_IF_ERROR(DecodeICC(payload, payload_size));
        } else if (type == "sRGB") {
          PIK_RETURN_IF_ERROR(DecodeSRGB(payload, payload_size));
        } else if (type == "gAMA") {
          PIK_RETURN_IF_ERROR(DecodeGAMA(payload, payload_size));
        } else if (type == "cHRM") {
          PIK_RETURN_IF_ERROR(DecodeCHRM(payload, payload_size));
        }
      }

      chunk = lodepng_chunk_next_const(chunk);
    }
    return true;
  }

  PaddedBytes icc_;

  bool have_pq_ = false;
  bool have_srgb_ = false;
  bool have_gama_ = false;
  bool have_chrm_ = false;
  ProfileParams params_;
};

// Stores ColorEncoding into PNG chunks.
class ColorEncodingWriterPNG {
 public:
  static Status Encode(const ColorEncoding& c, LodePNGInfo* PIK_RESTRICT info) {
    if (c.icc.empty()) {
      // Only ALLOW sRGB if no ICC present.
      PIK_RETURN_IF_ERROR(MaybeAddSRGB(c, info));
    } else {
      PIK_RETURN_IF_ERROR(AddICC(c.icc, info));
    }

    PIK_RETURN_IF_ERROR(MaybeAddGAMA(c, info));
    PIK_RETURN_IF_ERROR(MaybeAddCHRM(c, info));
    return true;
  }

 private:
  static Status AddChunk(const char* type, const PaddedBytes& payload,
                         LodePNGInfo* PIK_RESTRICT info) {
    // Ignore original location/order of chunks; place them in the first group.
    if (lodepng_chunk_create(&info->unknown_chunks_data[0],
                             &info->unknown_chunks_size[0], payload.size(),
                             type, payload.data()) != 0) {
      return PIK_FAILURE("Failed to add chunk");
    }
    return true;
  }

  static Status AddICC(const PaddedBytes& icc, LodePNGInfo* PIK_RESTRICT info) {
    LodePNGCompressSettings settings;
    lodepng_compress_settings_init(&settings);
    unsigned char* out = nullptr;
    size_t out_size = 0;
    if (lodepng_zlib_compress(&out, &out_size, icc.data(), icc.size(),
                              &settings) != 0) {
      return PIK_FAILURE("Failed to compress ICC");
    }

    PaddedBytes payload;
    payload.resize(3 + out_size);
    // TODO(janwas): use special name if PQ
    payload[0] = '1';  // profile name
    payload[1] = '\0';
    payload[2] = 0;  // compression method (zlib)
    memcpy(&payload[3], out, out_size);
    free(out);

    return AddChunk("iCCP", payload, info);
  }

  static Status MaybeAddSRGB(const ColorEncoding& c,
                             LodePNGInfo* PIK_RESTRICT info) {
    if (!c.IsGray() && c.color_space != ColorSpace::kRGB) return true;
    if (c.white_point != WhitePoint::kD65) return true;
    if (c.primaries != Primaries::kSRGB) return true;
    if (c.transfer_function != TransferFunction::kSRGB) return true;

    PaddedBytes payload;
    payload.push_back(static_cast<uint8_t>(c.rendering_intent));
    return AddChunk("sRGB", payload, info);
  }

  // Returns PNG encoding of floating-point value (times 10^5).
  static uint32_t U32FromF64(const double x) {
    return static_cast<int32_t>(std::round(x * 1E5));
  }

  static Status MaybeAddGAMA(const ColorEncoding& c,
                             LodePNGInfo* PIK_RESTRICT info) {
    const double gamma = GammaFromTransferFunction(c.transfer_function);
    if (gamma == 0.0) return true;

    PaddedBytes payload(4);
    StoreBE32(U32FromF64(gamma), payload.data());
    return AddChunk("gAMA", payload, info);
  }

  static Status MaybeAddCHRM(const ColorEncoding& c,
                             LodePNGInfo* PIK_RESTRICT info) {
    CIExy white_point;
    if (!WhitePointToCIExy(c.white_point, &white_point)) return true;
    PrimariesCIExy primaries;
    if (!PrimariesToCIExy(c.primaries, &primaries)) return true;

    PaddedBytes payload(32);
    StoreBE32(U32FromF64(white_point.x), &payload[0]);
    StoreBE32(U32FromF64(white_point.y), &payload[4]);
    StoreBE32(U32FromF64(primaries.r.x), &payload[8]);
    StoreBE32(U32FromF64(primaries.r.y), &payload[12]);
    StoreBE32(U32FromF64(primaries.g.x), &payload[16]);
    StoreBE32(U32FromF64(primaries.g.y), &payload[20]);
    StoreBE32(U32FromF64(primaries.b.x), &payload[24]);
    StoreBE32(U32FromF64(primaries.b.y), &payload[28]);
    return AddChunk("cHRM", payload, info);
  }
};

// RAII - ensures state is freed even if returning early.
struct PNGState {
  PNGState() { lodepng_state_init(&s); }
  ~PNGState() { lodepng_state_cleanup(&s); }

  LodePNGState s;
};

Status CheckGray(const LodePNGColorMode& mode, bool* is_gray) {
  switch (mode.colortype) {
    case LCT_GREY:
    case LCT_GREY_ALPHA:
      *is_gray = true;
      return true;

    case LCT_RGB:
    case LCT_RGBA:
      *is_gray = false;
      return true;

    case LCT_PALETTE: {
      *is_gray = true;
      for (size_t i = 0; i < mode.palettesize; i++) {
        if (mode.palette[i * 4] != mode.palette[i * 4 + 1] ||
            mode.palette[i * 4] != mode.palette[i * 4 + 2]) {
          *is_gray = false;
          break;
        }
      }
      return true;
    }

    default:
      *is_gray = false;
      return PIK_FAILURE("Unexpected PNG color type");
  }
}

Status CheckAlpha(const LodePNGColorMode& mode, bool* has_alpha) {
  if (mode.key_defined) {
    // Color key marks a single color as transparent.
    *has_alpha = true;
    return true;
  }

  switch (mode.colortype) {
    case LCT_GREY:
    case LCT_RGB:
      *has_alpha = false;
      return true;

    case LCT_GREY_ALPHA:
    case LCT_RGBA:
      *has_alpha = true;
      return true;

    case LCT_PALETTE: {
      *has_alpha = false;
      for (size_t i = 0; i < mode.palettesize; i++) {
        // PNG palettes are always 8-bit.
        if (mode.palette[i * 4 + 3] != 255) {
          *has_alpha = true;
          break;
        }
      }
      return true;
    }

    default:
      *has_alpha = false;
      return PIK_FAILURE("Unexpected PNG color type");
  }
}

LodePNGColorType MakeType(const bool is_gray, const bool has_alpha) {
  if (is_gray) {
    return has_alpha ? LCT_GREY_ALPHA : LCT_GREY;
  }
  return has_alpha ? LCT_RGBA : LCT_RGB;
}

// Inspects first chunk of the given type and updates state with the information
// when the chunk is relevant and present in the file.
Status InspectChunkType(const PaddedBytes& bytes, const std::string& type,
                        LodePNGState* state) {
  const unsigned char* chunk = lodepng_chunk_find_const(
      bytes.data(), bytes.data() + bytes.size(), type.c_str());
  if (chunk && lodepng_inspect_chunk(state, chunk - bytes.data(), bytes.data(),
                                     bytes.size()) != 0) {
    return PIK_FAILURE(("Invalid " + type + " chunk in PNG image").c_str());
  }
  return true;
}

}  // namespace

Status DecodeImagePNG(const PaddedBytes& bytes, ThreadPool* pool,
                      CodecInOut* io) {
  unsigned w, h;
  PNGState state;
  if (lodepng_inspect(&w, &h, &state.s, bytes.data(), bytes.size()) != 0) {
    return false;  // not an error - just wrong format
  }
  // Palette RGB values
  if (!InspectChunkType(bytes, "PLTE", &state.s)) {
    return false;
  }
  // Transparent color key, or palette transparency
  if (!InspectChunkType(bytes, "tRNS", &state.s)) {
    return false;
  }
  const LodePNGColorMode& color_mode = state.s.info_png.color;

  bool is_gray, has_alpha;
  PIK_RETURN_IF_ERROR(CheckGray(color_mode, &is_gray));
  PIK_RETURN_IF_ERROR(CheckAlpha(color_mode, &has_alpha));
  // We want LodePNG to promote 1/2/4 bit pixels to 8.
  size_t bits_per_sample = std::max(color_mode.bitdepth, 8u);
  io->SetOriginalBitsPerSample(bits_per_sample);
  if (bits_per_sample != 8 && bits_per_sample != 16) {
    return PIK_FAILURE("Unexpected PNG bit depth");
  }

  io->enc_size = bytes.size();
  io->dec_hints.Foreach([](const std::string& key, const std::string& value) {
    fprintf(stderr, "PNG decoder ignoring %s hint\n", key.c_str());
  });

  // Always decode to 8/16-bit RGB/RGBA, not LCT_PALETTE.
  state.s.info_raw.bitdepth = bits_per_sample;
  state.s.info_raw.colortype = MakeType(is_gray, has_alpha);
  unsigned char* out;
  if (lodepng_decode(&out, &w, &h, &state.s, bytes.data(), bytes.size()) != 0) {
    return PIK_FAILURE("PNG decode failed");
  }

  if (!MetadataReaderPNG::Decode(state.s.info_png, &io->metadata)) {
    fprintf(stderr, "PNG metadata may be incomplete.\n");
  }
  ColorEncodingReaderPNG reader;
  PIK_RETURN_IF_ERROR(
      reader(bytes, is_gray, &io->metadata, &io->dec_c_original));

  const bool big_endian = true;  // PNG requirement
  const uint8_t* end = nullptr;  // Don't know.
  const ExternalImage external(w, h, io->dec_c_original, has_alpha,
                               /*alpha_bits=*/ bits_per_sample, bits_per_sample,
                               big_endian, out, end);
  free(out);
  const CodecIntervals* temp_intervals = nullptr;  // Don't know min/max.
  return external.CopyTo(temp_intervals, pool, io);
}

Status EncodeImagePNG(const CodecInOut* io, const ColorEncoding& c_desired,
                      size_t bits_per_sample, ThreadPool* pool,
                      PaddedBytes* bytes) {
  io->enc_bits_per_sample = bits_per_sample == 8 ? 8 : 16;

  const ImageU* alpha = io->HasAlpha() ? &io->alpha() : nullptr;
  const size_t alpha_bits = io->HasAlpha() ? io->AlphaBits() : 0;
  const bool big_endian = true;              // PNG requirement
  CodecIntervals* temp_intervals = nullptr;  // Can't store min/max.
  const ExternalImage external(pool, io->color(), Rect(io->color()),
                               io->c_current(), c_desired, io->HasAlpha(),
                               alpha, alpha_bits, io->enc_bits_per_sample,
                               big_endian, temp_intervals);
  PIK_RETURN_IF_ERROR(external.IsHealthy());

  PNGState state;
  // For maximum compatibility, still store 8-bit even if pixels are all zero.
  state.s.encoder.auto_convert = 0;

  LodePNGInfo* info = &state.s.info_png;
  info->color.bitdepth = io->enc_bits_per_sample;
  info->color.colortype = MakeType(io->IsGray(), io->HasAlpha());
  state.s.info_raw = info->color;

  PIK_RETURN_IF_ERROR(ColorEncodingWriterPNG::Encode(c_desired, info));
  PIK_RETURN_IF_ERROR(MetadataWriterPNG::Encode(io->metadata, info));

  unsigned char* out = nullptr;
  size_t out_size = 0;
  if (lodepng_encode(&out, &out_size, external.Bytes().data(), io->xsize(),
                     io->ysize(), &state.s) != 0) {
    return PIK_FAILURE("Failed to encode PNG");
  }
  bytes->resize(out_size);
  memcpy(bytes->data(), out, out_size);
  free(out);

  io->enc_size = out_size;
  return true;
}

}  // namespace pik
