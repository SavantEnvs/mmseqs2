#include <cstddef>
#include <cstdint>
#include "Parameters.h"
#include "KSeqWrapper.h"

DEFAULT_PARAMETER_SINGLETON_INIT
const char *binary_name = "fuzz_kseq";

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    KSeqBuffer reader(reinterpret_cast<const char *>(data), size);
    size_t total = 0;
    while (reader.ReadEntry()) {
        total += reader.entry.name.l + reader.entry.comment.l + reader.entry.sequence.l + reader.entry.qual.l;
    }
    return total == (size_t)-1 ? 1 : 0;
}
