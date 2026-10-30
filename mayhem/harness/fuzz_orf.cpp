#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "Parameters.h"
#include "Orf.h"
#include "TranslateNucl.h"

DEFAULT_PARAMETER_SINGLETON_INIT
const char *binary_name = "fuzz_orf";

static const unsigned int GEN_CODES[] = {
    1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14, 15, 16, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4) {
        return -1;
    }
    const unsigned int genCode = GEN_CODES[data[0] % (sizeof(GEN_CODES) / sizeof(GEN_CODES[0]))];
    const bool allStarts = data[1] & 1;
    const unsigned int startMode = (data[1] >> 1) % 3;
    const unsigned int fwdFrames = data[2] & 7;
    const unsigned int revFrames = (data[2] >> 3) & 7;
    const size_t minLength = 1 + (data[3] & 15);
    const char *seq = reinterpret_cast<const char *>(data) + 4;
    const size_t seqLen = size - 4;

    Orf orf(genCode, allStarts);
    if (!orf.setSequence(seq, seqLen)) {
        return 0;
    }
    std::vector<Orf::SequenceLocation> results;
    orf.findAll(results, minLength, SIZE_MAX, 30, fwdFrames, revFrames, startMode);
    size_t residues = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        std::pair<const char *, size_t> view = orf.getSequence(results[i]);
        residues += view.second;
        char buffer[256];
        Orf::writeOrfHeader(buffer, 1, results[i].from, results[i].to,
                            results[i].hasIncompleteStart, results[i].hasIncompleteEnd);
    }

    TranslateNucl translate(static_cast<TranslateNucl::GenCode>(genCode));
    const size_t codons = seqLen / 3;
    if (codons > 0) {
        std::string aa(codons + 1, '\0');
        translate.translate(&aa[0], seq, static_cast<int>(codons * 3));
        residues += aa.size();
    }
    return residues == (size_t)-1 ? 1 : 0;
}
