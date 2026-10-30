// Known-answer checks over the library code the fuzz harnesses exercise. Prints one
// "PASS <name>" or "FAIL <name>" line per check; mayhem/test.sh parses them.
#include <cfloat>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "Parameters.h"
#include "ExpressionParser.h"
#include "KSeqWrapper.h"
#include "Orf.h"
#include "TranslateNucl.h"
#include "Sequence.h"
#include "SubstitutionMatrix.h"
#include "EvalueComputation.h"
#include "Matcher.h"

DEFAULT_PARAMETER_SINGLETON_INIT
const char *binary_name = "mayhem_selftest";

static void check(const char *name, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

static std::string str(const kstring_t &k) {
    return std::string(k.s ? k.s : "", k.l);
}

int main() {
    {
        ExpressionParser p("sqrt($11^2+$2^2)");
        bool ok = p.isOk();
        if (ok) {
            std::vector<int> idx = p.findBindableIndices();
            p.bind(10, 3);
            p.bind(1, 4);
            ok = idx.size() == 2 && idx[0] == 1 && idx[1] == 10 && p.evaluate() == 5.0;
        }
        check("expression_pythagoras", ok);
        ExpressionParser bad("2*(3+");
        check("expression_rejects_unbalanced", !bad.isOk());
        ExpressionParser arith("2*3+4");
        check("expression_precedence", arith.isOk() && arith.evaluate() == 10.0);
    }
    {
        const char fasta[] = ">seq1 first one\nACGT\nAC\n>seq2\nMKV\n";
        KSeqBuffer r(fasta, sizeof(fasta) - 1);
        bool a = r.ReadEntry() && str(r.entry.name) == "seq1" && str(r.entry.comment) == "first one"
                 && str(r.entry.sequence) == "ACGTAC";
        bool b = r.ReadEntry() && str(r.entry.name) == "seq2" && str(r.entry.sequence) == "MKV";
        bool c = !r.ReadEntry();
        check("kseq_fasta_two_records", a && b && c);
        const char fastq[] = "@r1 c\nACGT\n+\nIIII\n";
        KSeqBuffer q(fastq, sizeof(fastq) - 1);
        bool f = q.ReadEntry() && str(q.entry.name) == "r1" && str(q.entry.sequence) == "ACGT"
                 && str(q.entry.qual) == "IIII";
        check("kseq_fastq_record", f);
    }
    {
        const char *nucl = "ATGGATGGTACGGTTATCACCATCAAAATGAGCAGGGGTCAGGATATGCAGCCGACC";
        TranslateNucl t(TranslateNucl::CANONICAL);
        int len = (int)strlen(nucl);
        std::string aa(len / 3 + 1, '\0');
        t.translate(&aa[0], nucl, len);
        check("translate_canonical", aa.substr(0, len / 3) == "MDGTVITIKMSRGQDMQPT");
    }
    {
        struct Case { const char *seq; const char *view; size_t from, to; int strand; size_t total; };
        const Case cases[] = {
            {"CCATGAAATTTGGGTAACC", "ATGAAATTTGGG", 2, 13, 1, 6},
            {"GGTTACCCAAATTTCATGG", "ATGAAATTTGGG", 2, 13, -1, 6},
            {"CGAAGCGGGTGATGGCCGGCGCCGCGCCGGTTGGCGGCTGGCCATTCAAGGAGTGAGGAGATGGTCACTGGGCAGCGCGCCGGGGGGCGGCAGCAGCCCAAGGGTCGGGTCATTCCCGATTGGCCGCACCAGGCGCCCGCCACAGCCGGA",
             "ATGGTCACTGGGCAGCGCGCCGGGGGGCGGCAGCAGCCCAAGGGTCGGGTCATTCCCGATTGGCCGCACCAGGCGCCCGCCACAGCCGGA", 60, 149, 1, 8},
        };
        const char *names[] = {"orf_forward_strand", "orf_reverse_strand", "orf_long_sequence"};
        for (size_t c = 0; c < 3; ++c) {
            Orf orf(1, false);
            bool ok = orf.setSequence(cases[c].seq, strlen(cases[c].seq));
            std::vector<Orf::SequenceLocation> res;
            orf.findAll(res, 1, SIZE_MAX, 30, 7, 7, 0);
            bool found = false;
            for (size_t i = 0; i < res.size(); ++i) {
                std::pair<const char *, size_t> v = orf.getSequence(res[i]);
                if (res[i].from == cases[c].from && res[i].to == cases[c].to && (int)res[i].strand == cases[c].strand &&
                    std::string(v.first, v.second) == cases[c].view) {
                    found = true;
                }
            }
            check(names[c], ok && found && res.size() == cases[c].total);
        }
    }
    {
        Parameters &par = Parameters::getInstance();
        par.initMatrices();
        SubstitutionMatrix subMat(par.scoringMatrixFile.values.aminoacid().c_str(), 2.0, 0.0);
        EvalueComputation evaluer(100000, &subMat, par.gapOpen.values.aminoacid(), par.gapExtend.values.aminoacid());
        Matcher matcher(Parameters::DBTYPE_AMINO_ACIDS, 10000, &subMat, &evaluer, false, 1.0,
                        par.gapOpen.values.aminoacid(), par.gapExtend.values.aminoacid(), 0.0, 40);
        const std::string s1 = "PQITLWQRPLVTIKIGGQLKEALLDTGADDTVLEEMSLPGRWKPKMIGGIGGFIKVRQYDQILIEICGHKAIGTVLVGPTPVNIIGRNLLTQIGCTLNF";
        const std::string s2 = "PQFSLWKRPVVTAYIEGQPVEVLLDTGADDSIVAGIELGNNIVGGIGGFINTLEYKNVEIEVLNKKVRATIMTGDTPINIFGRNILTALGMSLNL";
        Sequence q(10000, Parameters::DBTYPE_AMINO_ACIDS, &subMat, 6, false, false);
        q.mapSequence(0, 0, s1.c_str(), s1.size());
        matcher.initQuery(&q);
        Sequence self(10000, Parameters::DBTYPE_AMINO_ACIDS, &subMat, 6, false, false);
        self.mapSequence(1, 1, s1.c_str(), s1.size());
        Matcher::result_t r = matcher.getSWResult(&self, INT_MAX, false, 0, 0.0, FLT_MAX, Matcher::SCORE_COV_SEQID, 0, false);
        int expected = 0;
        for (size_t i = 0; i < s1.size(); ++i) {
            int c = q.numSequence[i];
            expected += (int8_t)subMat.subMatrix[c][c];
        }
        check("align_identical_score_is_bitscore_of_diagonal_sum",
              r.score == static_cast<int>(evaluer.computeBitScore(expected) + 0.5) && r.score == 209 &&
              r.seqId == 1.0f && r.qStartPos == 0 && r.qEndPos == (int)s1.size() - 1 &&
              r.backtrace == std::string(s1.size(), 'M'));
        Sequence other(10000, Parameters::DBTYPE_AMINO_ACIDS, &subMat, 6, false, false);
        other.mapSequence(2, 2, s2.c_str(), s2.size());
        Matcher::result_t r2 = matcher.getSWResult(&other, INT_MAX, false, 0, 0.0, FLT_MAX, Matcher::SCORE_COV_SEQID, 0, false);
        check("align_homologous_pair", r2.score == 98 && r2.seqId > 0.469f && r2.seqId < 0.470f &&
              r2.qStartPos == 0 && r2.qEndPos == 97 && r2.dbStartPos == 0 && r2.dbEndPos == 93 &&
              r2.backtrace == std::string(40, 'M') + "IIII" + std::string(54, 'M'));
    }
    return 0;
}
