#include <cfloat>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include "Parameters.h"
#include "Sequence.h"
#include "SubstitutionMatrix.h"
#include "EvalueComputation.h"
#include "Matcher.h"

DEFAULT_PARAMETER_SINGLETON_INIT
const char *binary_name = "fuzz_align";

static const int MAX_RES = 600;
static SubstitutionMatrix *subMat;
static EvalueComputation *evaluer;

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
    Parameters &par = Parameters::getInstance();
    par.initMatrices();
    subMat = new SubstitutionMatrix(par.scoringMatrixFile.values.aminoacid().c_str(), 2.0, 0.0);
    evaluer = new EvalueComputation(100000, subMat, par.gapOpen.values.aminoacid(), par.gapExtend.values.aminoacid());
    return 0;
}

// input: first byte = split point (query length), rest = query followed by target residues
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 3) {
        return -1;
    }
    const size_t total = size - 1;
    size_t qLen = 1 + data[0] % (total - 1);
    const char *residues = reinterpret_cast<const char *>(data) + 1;
    size_t tLen = total - qLen;
    if (qLen > MAX_RES) qLen = MAX_RES;
    if (tLen > MAX_RES) tLen = MAX_RES;
    const char *tSeq = residues + (total - tLen);

    Parameters &par = Parameters::getInstance();
    Matcher matcher(Parameters::DBTYPE_AMINO_ACIDS, 2 * MAX_RES, subMat, evaluer, false, 1.0,
                    par.gapOpen.values.aminoacid(), par.gapExtend.values.aminoacid(), 0.0, 40);

    Sequence query(10000, Parameters::DBTYPE_AMINO_ACIDS, subMat, 6, false, false);
    query.mapSequence(0, 0, residues, qLen);
    Sequence target(10000, Parameters::DBTYPE_AMINO_ACIDS, subMat, 6, false, false);
    target.mapSequence(1, 1, tSeq, tLen);

    matcher.initQuery(&query);
    Matcher::result_t res = matcher.getSWResult(&target, INT_MAX, false, 0, 0.0, FLT_MAX,
                                                 Matcher::SCORE_COV_SEQID, 0, false);
    char buffer[4096];
    Matcher::resultToBuffer(buffer, res, true, false);
    return res.score < 0 ? 1 : 0;
}
