// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Sam Aaron
//
// Writes dsp/scsynth/scsynth_tables.cpp: scsynth's lookup tables as
// constants, each computed exactly as upstream computes it at load (Samp.cpp
// SynthLibInit::FillTables, SC_Complex.h initTables, SC_fftlib.cpp
// scfft_create_fftwindow and create_cosTable). A guest any number of which
// can run in one process keeps nothing process-wide, and a table filled at
// load is a mutable static; a constant is not, and is shared by every
// instance for nothing. test/native/test_scsynth_tables.cpp holds the data to
// these formulas.
//
// Run through scripts/gen-scsynth-tables.sh, which compiles and runs this
// and writes the file. Each value is written as a hexadecimal float literal,
// so what is read back is what was computed.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr int kSineSize = 8192;
constexpr int kPolarLUTSize = 2049;
constexpr int kPolarLUTSize2 = kPolarLUTSize >> 1;
constexpr float kBadValue = 1e20f;
constexpr int kFftLog2Min = 3;
constexpr int kFftLog2Max = 15;

// SC_fftlib.cpp's own constants, float literals promoted to double in the
// arithmetic, so the windows come out as upstream's do.
constexpr float kFftPi = 3.1415926535898f;
constexpr float kFftTwoPi = 6.28318530717952646f;

size_t fftWindowOffset(int log2n) { return (size_t(1) << log2n) - (size_t(1) << kFftLog2Min); }
size_t fftWindowFloats() { return fftWindowOffset(kFftLog2Max + 1); }
size_t fftCosFloats(int log2k) { return ((size_t(1) << log2k) / 4 + 1 + 3) & ~size_t(3); }
size_t fftCosOffset(int log2n) {
    size_t at = 0;
    for (int k = kFftLog2Min; k < log2n; ++k) at += fftCosFloats(k);
    return at;
}

// Samp.cpp: SignalAsWavetable.
void signalAsWavetable(const float* signal, float* wavetable, long inSize) {
    float* out = wavetable - 1;
    for (int i = 0; i < inSize - 1; ++i) {
        const float val1 = signal[i];
        const float val2 = signal[i + 1];
        *++out = 2.f * val1 - val2;
        *++out = val2 - val1;
    }
    const float val1 = signal[inSize - 1];
    const float val2 = signal[0];
    *++out = 2.f * val1 - val2;
    *++out = val2 - val1;
}

void writeArray(FILE* out, const char* declaration, const std::vector<float>& values) {
    std::fprintf(out, "%s = {\n", declaration);
    for (size_t i = 0; i < values.size(); ++i) {
        std::fprintf(out, "%s%af,", i % 6 == 0 ? "    " : " ", (double)values[i]);
        if (i % 6 == 5 || i + 1 == values.size()) std::fputc('\n', out);
    }
    std::fprintf(out, "};\n\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: gen-scsynth-tables <output.cpp>\n");
        return 2;
    }
    FILE* out = std::fopen(argv[1], "w");
    if (!out) {
        std::perror(argv[1]);
        return 1;
    }

    // Samp.cpp: SynthLibInit::FillTables.
    std::vector<float> sine(kSineSize + 1), invSine(kSineSize + 1), pmSine(kSineSize + 1), wavetable(2 * kSineSize);
    {
        const double twopi = std::acos(-1.0) * 2.0;
        const double sineIndexToPhase = twopi / kSineSize;
        const double pmf = (1L << 29) / twopi;
        for (int i = 0; i <= kSineSize; ++i) {
            const double phase = i * sineIndexToPhase;
            const float d = (float)std::sin(phase);
            sine[i] = d;
            invSine[i] = (float)(1. / d);
            pmSine[i] = (float)(d * pmf);
        }
        signalAsWavetable(sine.data(), wavetable.data(), kSineSize);
        invSine[0] = invSine[kSineSize / 2] = invSine[kSineSize] = kBadValue;
        const int sz = kSineSize, sz2 = sz >> 1;
        for (int i = 1; i <= 8; ++i) {
            invSine[i] = invSine[sz - i] = kBadValue;
            invSine[sz2 - i] = invSine[sz2 + i] = kBadValue;
        }
    }

    // SC_Complex.h: initTables (its sine table is Samp's, index for index).
    std::vector<float> magLUT(kPolarLUTSize), phaseLUT(kPolarLUTSize);
    {
        const double rPolarLUTSize2 = 1. / kPolarLUTSize2;
        for (int i = 0; i < kPolarLUTSize; ++i) {
            const double slope = (i - kPolarLUTSize2) * rPolarLUTSize2;
            const double angle = std::atan(slope);
            phaseLUT[i] = (float)angle;
            magLUT[i] = (float)(1.f / std::cos(angle));
        }
    }

    // SC_fftlib.cpp: scfft_create_fftwindow for both window types, every
    // size, end to end; then create_cosTable likewise.
    std::vector<float> sineWindows(fftWindowFloats()), hannWindows(fftWindowFloats());
    std::vector<float> cosTables(fftCosOffset(kFftLog2Max + 1), 0.f);
    for (int log2n = kFftLog2Min; log2n <= kFftLog2Max; ++log2n) {
        const int size = 1 << log2n;
        float* sineWin = sineWindows.data() + fftWindowOffset(log2n);
        float* hannWin = hannWindows.data() + fftWindowOffset(log2n);
        double winc = kFftPi / size;
        for (int i = 0; i < size; ++i) sineWin[i] = (float)std::sin(i * winc);
        winc = kFftTwoPi / size;
        for (int i = 0; i < size; ++i) hannWin[i] = (float)(0.5 - 0.5 * std::cos(i * winc));
        float* cosTable = cosTables.data() + fftCosOffset(log2n);
        const int size2 = size / 4 + 1;
        for (int i = 0; i < size2; ++i) cosTable[i] = (float)std::cos(i * winc);
    }

    std::fprintf(out,
                 "// Generated by scripts/gen-scsynth-tables.sh from scripts/gen-scsynth-tables.cpp: scsynth's\n"
                 "// lookup tables as constants, each exactly as upstream computes it at load. Do not edit;\n"
                 "// test/native/test_scsynth_tables.cpp holds this data to the formulas.\n"
                 "#include \"scsynth_tables.h\"\n\n");
    writeArray(out, "const float32 gSine[kSineSize + 1]", sine);
    writeArray(out, "const float32 gPMSine[kSineSize + 1]", pmSine);
    writeArray(out, "const float32 gInvSine[kSineSize + 1]", invSine);
    writeArray(out, "const float32 gSineWavetable[2 * kSineSize]", wavetable);
    std::fprintf(out, "namespace detail {\n\n");
    writeArray(out, "const float gMagLUT[kPolarLUTSize]", magLUT);
    writeArray(out, "const float gPhaseLUT[kPolarLUTSize]", phaseLUT);
    std::fprintf(out, "} // namespace detail\n\n");
    writeArray(out, "const float gFftSineWindows[scfft_window_floats()]", sineWindows);
    writeArray(out, "const float gFftHannWindows[scfft_window_floats()]", hannWindows);
    writeArray(out, "const float gFftCosTables[scfft_cos_offset(SC_FFT_LOG2_MAXSIZE + 1)]", cosTables);
    return std::fclose(out) == 0 ? 0 : 1;
}
