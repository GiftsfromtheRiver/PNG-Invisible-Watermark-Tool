#include "rs_codec.h"
#include <algorithm>
#include <cstring>

namespace rs_codec {

// ========================================================================
// ECC level mapping
// ========================================================================

int ecc_level_to_npar(int ecc_level) {
    static const int npar_table[] = {0, 8, 16, 24, 32, 48, 64};
    if (ecc_level < 0 || ecc_level > 6) return 0;
    return npar_table[ecc_level];
}

const char* ecc_level_name(int ecc_level) {
    static const char* names[] = {
        "None",
        "Low (npar=8, 4 errors)",
        "Medium-Low (npar=16, 8 errors)",
        "Medium (npar=24, 12 errors)",
        "Medium-High (npar=32, 16 errors)",
        "High (npar=48, 24 errors)",
        "Very High (npar=64, 32 errors)"
    };
    if (ecc_level < 0 || ecc_level > 6) return "Invalid";
    return names[ecc_level];
}

// ========================================================================
// GF(2^8) Arithmetic
// ========================================================================
// Primitive polynomial: p(x) = x^8 + x^4 + x^3 + x^2 + 1 = 0x11D
// Generator: alpha = 2

namespace {

uint8_t gf_exp_table[512];
int gf_log_table[256];
bool gf_initialized = false;

void gf_init() {
    if (gf_initialized) return;
    
    int x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp_table[i] = (uint8_t)x;
        gf_log_table[x] = i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    gf_log_table[0] = 0; // undefined, set to 0 for safety
    
    // Extend to avoid modular reduction
    for (int i = 255; i < 512; i++) {
        gf_exp_table[i] = gf_exp_table[i - 255];
    }
    
    gf_initialized = true;
}

inline uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return gf_exp_table[gf_log_table[a] + gf_log_table[b]];
}

inline uint8_t gf_inv(uint8_t a) {
    if (a == 0) return 0;
    return gf_exp_table[255 - gf_log_table[a]];
}

// ========================================================================
// Polynomial operations over GF(2^8)
// Convention: poly[i] = coefficient of x^i
// ========================================================================

std::vector<uint8_t> poly_mul(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.empty() || b.empty()) return {};
    std::vector<uint8_t> r(a.size() + b.size() - 1, 0);
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i] == 0) continue;
        for (size_t j = 0; j < b.size(); j++) {
            r[i + j] ^= gf_mul(a[i], b[j]);
        }
    }
    return r;
}

// Evaluate polynomial at point x using Horner's method
uint8_t poly_eval(const std::vector<uint8_t>& poly, uint8_t x) {
    uint8_t result = 0;
    for (int i = (int)poly.size() - 1; i >= 0; i--) {
        result = gf_mul(result, x) ^ poly[i];
    }
    return result;
}

} // anonymous namespace

// ========================================================================
// RS Generator Polynomial (narrow-sense)
// ========================================================================
// g(x) = (x + alpha^1)(x + alpha^2)...(x + alpha^{npar})
// Roots at alpha^1, alpha^2, ..., alpha^{npar}

static std::vector<uint8_t> rs_generator(int npar) {
    gf_init();
    std::vector<uint8_t> g = {1};
    for (int i = 1; i <= npar; i++) {
        // (x + alpha^i): poly[0] = alpha^i (coeff of x^0), poly[1] = 1 (coeff of x^1)
        std::vector<uint8_t> factor = {gf_exp_table[i], 1};
        g = poly_mul(g, factor);
    }
    return g; // degree = npar, size = npar+1
}

// ========================================================================
// RS Encode (single block) - Systematic encoding
// ========================================================================
// Narrow-sense RS(255, k) with roots at alpha^1..alpha^{npar}
//
// Byte order convention:
//   codeword[0] = data[0] (first data byte, corresponds to x^{254})
//   codeword[k-1] = data[k-1] (last data byte, corresponds to x^{npar})
//   codeword[k] = parity[0] (corresponds to x^{npar-1})
//   codeword[254] = parity[npar-1] (corresponds to x^0)
//
// Message polynomial: M(x) = data[0]*x^{254} + ... + data[k-1]*x^{npar}
// Shifted by npar: already included
// Remainder: R(x) = M(x) mod g(x)
// Codeword: C(x) = M(x) + R(x)
// In byte order: [data | parity]

static std::vector<uint8_t> rs_encode_block(const std::vector<uint8_t>& data, int npar) {
    int k = 255 - npar;
    int nn = 255;
    
    std::vector<uint8_t> d = data;
    if ((int)d.size() < k) d.resize(k, 0);
    
    // Build message polynomial: M(x) = d[0]*x^{nn-1} + d[1]*x^{nn-2} + ... + d[k-1]*x^{npar}
    // poly[j] = coefficient of x^j
    std::vector<uint8_t> msg_poly(nn, 0);
    for (int i = 0; i < k; i++) {
        msg_poly[nn - 1 - i] = d[i];
    }
    
    // Compute remainder R(x) = M(x) mod g(x)
    auto gen = rs_generator(npar);
    
    // Polynomial long division
    std::vector<uint8_t> dividend = msg_poly;
    uint8_t gen_lead_inv = gf_inv(gen.back()); // gen is degree npar, leading coeff = gen[npar]
    
    for (int i = nn - 1; i >= npar; i--) {
        if (dividend[i] == 0) continue;
        uint8_t factor = gf_mul(dividend[i], gen_lead_inv);
        for (size_t j = 0; j < gen.size(); j++) {
            dividend[i - npar + j] ^= gf_mul(gen[j], factor);
        }
    }
    
    // Remainder is in dividend[0..npar-1]
    // R(x) = dividend[0] + dividend[1]*x + ... + dividend[npar-1]*x^{npar-1}
    
    // Build codeword in byte order: [data | parity]
    // byte[j] for j < k: data[j]
    // byte[k+p] for p < npar: corresponds to x^{npar-1-p}, so = remainder[npar-1-p]
    std::vector<uint8_t> codeword(nn);
    for (int i = 0; i < k; i++) codeword[i] = d[i];
    for (int p = 0; p < npar; p++) codeword[k + p] = dividend[npar - 1 - p];
    
    return codeword;
}

// ========================================================================
// RS Decode (single block) - Berlekamp-Massey + Chien + Forney
// ========================================================================

static bool rs_decode_block(const std::vector<uint8_t>& received, int npar, std::vector<uint8_t>& corrected) {
    int nn = 255;
    int k = nn - npar;
    
    // Step 1: Compute syndromes S_i = R(alpha^i) for i = 1..npar
    // Using Horner's method: byte[0] corresponds to x^{254}
    // R(alpha^i) = byte[0]*alpha^{254i} + byte[1]*alpha^{253i} + ... + byte[254]
    std::vector<uint8_t> syndromes(npar);
    bool all_zero = true;
    
    for (int s = 0; s < npar; s++) {
        uint8_t alpha_s = gf_exp_table[s + 1]; // alpha^{s+1}, s=0..npar-1 → alpha^1..alpha^{npar}
        uint8_t val = 0;
        for (int j = 0; j < nn; j++) {
            val = gf_mul(val, alpha_s) ^ received[j];
        }
        syndromes[s] = val;
        if (val != 0) all_zero = false;
    }
    
    if (all_zero) {
        corrected.assign(received.begin(), received.begin() + k);
        return true;
    }
    
    // Step 2: Berlekamp-Massey algorithm
    // syndromes[0] = S_1, syndromes[1] = S_2, ..., syndromes[npar-1] = S_{npar}
    // BM finds Lambda(x) = 1 + lambda_1*x + ... + lambda_v*x^v
    // where roots of Lambda are X_j^{-1} (inverse of error location numbers)
    
    std::vector<uint8_t> C = {1}; // error locator polynomial Lambda(x)
    std::vector<uint8_t> B = {1}; // previous polynomial
    int L = 0;
    int m = 1;
    uint8_t b = 1;
    
    for (int n = 0; n < npar; n++) {
        // Compute discrepancy
        uint8_t d = syndromes[n]; // S_{n+1}
        for (int i = 1; i <= L; i++) {
            if (i < (int)C.size()) {
                d ^= gf_mul(C[i], syndromes[n - i]); // S_{n+1-i}
            }
        }
        
        if (d == 0) {
            m++;
        } else if (2 * L <= n) {
            std::vector<uint8_t> T = C;
            uint8_t coeff = gf_mul(d, gf_inv(b));
            // C(x) += coeff * x^m * B(x)
            std::vector<uint8_t> shifted_B(m + B.size(), 0);
            for (size_t i = 0; i < B.size(); i++) {
                shifted_B[i + m] = gf_mul(B[i], coeff);
            }
            C = poly_mul(C, {1}); // no-op, keep C as is
            // Actually just XOR:
            if (shifted_B.size() > C.size()) C.resize(shifted_B.size(), 0);
            for (size_t i = 0; i < shifted_B.size(); i++) {
                C[i] ^= shifted_B[i];
            }
            L = n + 1 - L;
            B = T;
            b = d;
            m = 1;
        } else {
            uint8_t coeff = gf_mul(d, gf_inv(b));
            std::vector<uint8_t> shifted_B(m + B.size(), 0);
            for (size_t i = 0; i < B.size(); i++) {
                shifted_B[i + m] = gf_mul(B[i], coeff);
            }
            if (shifted_B.size() > C.size()) C.resize(shifted_B.size(), 0);
            for (size_t i = 0; i < shifted_B.size(); i++) {
                C[i] ^= shifted_B[i];
            }
            m++;
        }
    }
    
    int num_errors = L;
    if (num_errors > npar / 2) return false;
    
    // Step 3: Chien search - find error positions
    // Lambda(x) has roots at X_j^{-1} where X_j = alpha^{pos_j}
    // pos_j is the power of x corresponding to the error byte position
    // byte[j] corresponds to x^{nn-1-j}, so error at byte[j] means X_j = alpha^{nn-1-j}
    // X_j^{-1} = alpha^{-(nn-1-j)} = alpha^{j-nn+1} = alpha^{j-254} (mod 255)
    // We need to find all j in 0..254 such that Lambda(alpha^{j-254 mod 255}) = 0
    // Equivalently: Lambda(alpha^{(255+254-j) mod 255}) = 0 = Lambda(alpha^{(509-j) mod 255})
    // Simpler: Lambda evaluated at alpha^{255-1-j+1} ... let me think differently.
    
    // We need Lambda(X_j^{-1}) = 0 where X_j = alpha^{254-j}
    // X_j^{-1} = alpha^{-(254-j)} = alpha^{255-254+j} = alpha^{1+j} (mod 255, since alpha^255=1)
    // Wait: alpha^{-(254-j)} = alpha^{255-(254-j)} = alpha^{1+j}
    // Hmm, that's not right either. -(254-j) mod 255 = (255 - 254 + j) mod 255 = (1+j) mod 255
    // So X_j^{-1} = alpha^{(1+j) mod 255}
    
    // So we need: for each byte position j (0..254), test if Lambda(alpha^{(1+j)}) = 0
    // But alpha^{255} = 1 = alpha^0, so for j=254: alpha^{255} = alpha^0 = 1
    
    std::vector<int> error_positions; // byte positions
    for (int j = 0; j < nn; j++) {
        int exp = (1 + j) % 255;
        uint8_t test_point = gf_exp_table[exp];
        if (poly_eval(C, test_point) == 0) {
            error_positions.push_back(j);
        }
    }
    
    if ((int)error_positions.size() != num_errors) return false;
    
    // Step 4: Forney algorithm - compute error values
    // Error evaluator: Omega(x) = S(x) * Lambda(x) mod x^{npar}
    // where S(x) = S_1 + S_2*x + S_3*x^2 + ... + S_{npar}*x^{npar-1}
    //              = syndromes[0] + syndromes[1]*x + ...
    
    std::vector<uint8_t> S_poly(npar);
    for (int i = 0; i < npar; i++) S_poly[i] = syndromes[i];
    
    std::vector<uint8_t> omega_full = poly_mul(S_poly, C);
    std::vector<uint8_t> omega(npar, 0);
    for (int i = 0; i < npar && i < (int)omega_full.size(); i++) {
        omega[i] = omega_full[i];
    }
    
    // Formal derivative of Lambda: Lambda'(x)
    // Lambda(x) = lambda_0 + lambda_1*x + lambda_2*x^2 + ...
    // Lambda'(x) = lambda_1 + 0*x + lambda_3*x^2 + 0*x^3 + lambda_5*x^4 + ...
    // In GF(2^m): i*lambda_i = 0 when i is even, lambda_i when i is odd
    // So Lambda'[j] = Lambda[j+1] for even j (where j+1 is odd), 0 for odd j
    size_t deriv_len = C.size() > 1 ? C.size() - 1 : 1;
    std::vector<uint8_t> lambda_deriv(deriv_len, 0);
    for (size_t i = 1; i < C.size(); i += 2) {
        lambda_deriv[i - 1] = C[i]; // odd index i -> derivative position i-1 (even)
    }
    
    // For each error at byte position j:
    // X_j = alpha^{254-j} (error location number)
    // X_j^{-1} = alpha^{(1+j) mod 255}
    // Forney formula for narrow-sense (b=1):
    // e_j = Omega(X_j^{-1}) / Lambda'(X_j^{-1})
    // (no X_j factor since b=1 means X_j^{1-b} = X_j^0 = 1)
    
    corrected = received;
    for (int j : error_positions) {
        int x_inv_exp = (1 + j) % 255;
        uint8_t X_inv = gf_exp_table[x_inv_exp];
        
        uint8_t omega_val = poly_eval(omega, X_inv);
        uint8_t lambda_d_val = poly_eval(lambda_deriv, X_inv);
        
        if (lambda_d_val == 0) return false;
        
        uint8_t error_val = gf_mul(omega_val, gf_inv(lambda_d_val));
        corrected[j] ^= error_val;
    }
    
    corrected.resize(k);
    return true;
}

// ========================================================================
// Public API - Multi-block encode/decode
// ========================================================================

std::vector<uint8_t> rs_encode(const std::vector<uint8_t>& data, int npar) {
    gf_init();
    if (npar <= 0 || npar >= 255) return data;
    
    int k = 255 - npar;
    size_t num_blocks = (data.size() + k - 1) / k;
    if (num_blocks == 0) num_blocks = 1;
    
    std::vector<uint8_t> result;
    result.reserve(num_blocks * 255);
    
    for (size_t b = 0; b < num_blocks; b++) {
        size_t start = b * k;
        size_t end = std::min(start + (size_t)k, data.size());
        std::vector<uint8_t> block(data.begin() + start, data.begin() + end);
        
        auto encoded = rs_encode_block(block, npar);
        result.insert(result.end(), encoded.begin(), encoded.end());
    }
    
    return result;
}

std::vector<uint8_t> rs_decode(const std::vector<uint8_t>& received, int npar) {
    gf_init();
    if (npar <= 0 || npar >= 255) return received;
    if (received.size() % 255 != 0) return {};
    
    size_t num_blocks = received.size() / 255;
    std::vector<uint8_t> result;
    
    for (size_t b = 0; b < num_blocks; b++) {
        std::vector<uint8_t> block(received.begin() + b * 255,
                                    received.begin() + (b + 1) * 255);
        
        std::vector<uint8_t> corr;
        if (!rs_decode_block(block, npar, corr)) return {};
        result.insert(result.end(), corr.begin(), corr.end());
    }
    
    return result;
}

// ========================================================================
// RS Decode with Erasures (pure erasure decoding via Forney direct method)
// ========================================================================

static bool rs_decode_block_with_erasures(const std::vector<uint8_t>& received, int npar,
                                           const std::vector<int>& erasures,
                                           std::vector<uint8_t>& corrected) {
    int nn = 255;
    int k = nn - npar;
    
    if ((int)erasures.size() > npar) return false;  // pure erasure: max npar erasures
    
    // Step 1: Compute syndromes S_i = R(alpha^i) for i = 1..npar
    std::vector<uint8_t> syndromes(npar);
    bool all_zero = true;
    
    for (int s = 0; s < npar; s++) {
        uint8_t alpha_s = gf_exp_table[s + 1];
        uint8_t val = 0;
        for (int j = 0; j < nn; j++) {
            val = gf_mul(val, alpha_s) ^ received[j];
        }
        syndromes[s] = val;
        if (val != 0) all_zero = false;
    }
    
    if (all_zero) {
        // No errors at all
        corrected.assign(received.begin(), received.begin() + k);
        return true;
    }
    
    if (erasures.empty()) {
        // No erasures but has syndromes -> fall back to error-only (shouldn't happen
        // if called correctly, but handle gracefully)
        return false;
    }
    
    // Step 2: Build erasure locator polynomial Lambda(x) = prod(1 - X_j * x)
    // where X_j = alpha^(254 - pos_j) for erasure at byte position pos_j
    // In polynomial form (coeff of x^i):
    // Lambda[0] = 1
    // Each factor (1 - X_j * x) has: coeff[0] = 1, coeff[1] = gf_exp_table[254-pos_j] (negated in GF(2^m))
    // Wait: in GF(2^m), subtraction = addition = XOR, so (1 + X_j * x) = (1 - X_j * x)
    
    std::vector<uint8_t> Lambda = {1};
    for (int pos : erasures) {
        // X_j = alpha^(254 - pos)
        int xj_exp = (254 - pos) % 255;
        if (xj_exp < 0) xj_exp += 255;
        uint8_t xj = gf_exp_table[xj_exp];
        
        // Multiply Lambda by (1 + X_j * x)
        // new_Lambda[i] = Lambda[i] + X_j * Lambda[i-1]
        std::vector<uint8_t> new_Lambda(Lambda.size() + 1, 0);
        for (size_t i = 0; i < Lambda.size(); i++) {
            new_Lambda[i] ^= Lambda[i];
            new_Lambda[i + 1] ^= gf_mul(Lambda[i], xj);
        }
        Lambda = new_Lambda;
    }
    
    // Step 3: Compute error evaluator Omega(x) = S(x) * Lambda(x) mod x^{npar}
    std::vector<uint8_t> S_poly(npar);
    for (int i = 0; i < npar; i++) S_poly[i] = syndromes[i];
    
    std::vector<uint8_t> omega_full = poly_mul(S_poly, Lambda);
    std::vector<uint8_t> omega(npar, 0);
    for (int i = 0; i < npar && i < (int)omega_full.size(); i++) {
        omega[i] = omega_full[i];
    }
    
    // Step 4: Formal derivative of Lambda: Lambda'(x)
    // Lambda'(x) = sum of Lambda[i] * i * x^{i-1}
    // In GF(2^m): i*Lambda[i] = Lambda[i] when i is odd, 0 when i is even
    size_t deriv_len = Lambda.size() > 1 ? Lambda.size() - 1 : 1;
    std::vector<uint8_t> lambda_deriv(deriv_len, 0);
    for (size_t i = 1; i < Lambda.size(); i += 2) {
        lambda_deriv[i - 1] = Lambda[i];
    }
    
    // Step 5: For each erasure position, compute error value via Forney formula
    // For narrow-sense (b=1):
    // e_j = Omega(X_j^{-1}) / Lambda'(X_j^{-1})
    // X_j^{-1} = alpha^{-(254-pos)} = alpha^{(1+pos) mod 255}
    
    corrected = received;
    for (int pos : erasures) {
        int x_inv_exp = (1 + pos) % 255;
        uint8_t X_inv = gf_exp_table[x_inv_exp];
        
        uint8_t omega_val = poly_eval(omega, X_inv);
        uint8_t lambda_d_val = poly_eval(lambda_deriv, X_inv);
        
        if (lambda_d_val == 0) return false;
        
        uint8_t error_val = gf_mul(omega_val, gf_inv(lambda_d_val));
        corrected[pos] ^= error_val;
    }
    
    corrected.resize(k);
    return true;
}

std::vector<uint8_t> rs_decode_with_erasures(const std::vector<uint8_t>& received, int npar,
                                              const std::vector<std::vector<int>>& erasures_per_block) {
    gf_init();
    if (npar <= 0 || npar >= 255) return received;
    if (received.size() % 255 != 0) return {};
    
    size_t num_blocks = received.size() / 255;
    std::vector<uint8_t> result;
    
    for (size_t b = 0; b < num_blocks; b++) {
        std::vector<uint8_t> block(received.begin() + b * 255,
                                    received.begin() + (b + 1) * 255);
        
        std::vector<uint8_t> corr;
        if (b < erasures_per_block.size() && !erasures_per_block[b].empty()) {
            if (!rs_decode_block_with_erasures(block, npar, erasures_per_block[b], corr)) {
                return {};
            }
        } else {
            // No erasures for this block - use standard error-only decode
            if (!rs_decode_block(block, npar, corr)) return {};
        }
        result.insert(result.end(), corr.begin(), corr.end());
    }
    
    return result;
}

} // namespace rs_codec
