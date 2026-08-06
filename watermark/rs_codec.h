#ifndef RS_CODEC_H
#define RS_CODEC_H

#include <cstdint>
#include <vector>

namespace rs_codec {

// ECC levels mapping:
// 0: no ECC
// 1: npar=8  (corrects 4 byte errors, 8 bytes overhead)
// 2: npar=16 (corrects 8 byte errors, 16 bytes overhead)
// 3: npar=24 (corrects 12 byte errors, 24 bytes overhead)
// 4: npar=32 (corrects 16 byte errors, 32 bytes overhead)
// 5: npar=48 (corrects 24 byte errors, 48 bytes overhead)
// 6: npar=64 (corrects 32 byte errors, 64 bytes overhead)

int ecc_level_to_npar(int ecc_level);
const char* ecc_level_name(int ecc_level);

// RS encode: data -> data + parity bytes
// Data is split into blocks of k=255-npar bytes each (last block padded with zeros)
// Each block gets npar parity bytes -> output block is 255 bytes
// Total output = num_blocks * 255
std::vector<uint8_t> rs_encode(const std::vector<uint8_t>& data, int npar);

// RS decode: received data (with possible errors) -> corrected data
// Input should be multiple of 255 bytes (each 255-byte block is decoded independently)
// Returns corrected data (parity stripped, padding stripped)
// Returns empty vector if decoding fails (too many errors in some block)
std::vector<uint8_t> rs_decode(const std::vector<uint8_t>& received, int npar);

// RS decode with erasure support
// erasures_per_block: for each 255-byte block, a list of byte indices (0-254) known to be unreliable
// If erasures_per_block is empty for a block, standard error-only decoding is used
// Erasure capacity: each erasure costs 1 parity symbol (vs 2 for errors)
// Constraint: num_erasures + 2*num_errors <= npar
std::vector<uint8_t> rs_decode_with_erasures(
    const std::vector<uint8_t>& received, 
    int npar,
    const std::vector<std::vector<int>>& erasures_per_block);

} // namespace rs_codec

#endif // RS_CODEC_H
