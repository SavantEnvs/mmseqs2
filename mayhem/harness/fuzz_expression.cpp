#include <cstddef>
#include <cstdint>
#include <string>
#include "Parameters.h"
#include "ExpressionParser.h"

DEFAULT_PARAMETER_SINGLETON_INIT
const char *binary_name = "fuzz_expression";

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    std::string expr(reinterpret_cast<const char *>(data), size);
    ExpressionParser parser(expr.c_str());
    if (!parser.isOk()) {
        return 0;
    }
    std::vector<int> indices = parser.findBindableIndices();
    for (size_t i = 0; i < indices.size(); ++i) {
        parser.bind(indices[i], 1.5 + (double)i);
    }
    volatile double result = parser.evaluate();
    (void)result;
    return 0;
}
