#include "temporal_corpus_core.h"

#include <exception>
#include <iostream>
#include <string>

namespace
{

int RunMain(const int argumentCount, char*[])
{
    if (argumentCount != 1)
    {
        std::cerr << "usage: PBRemoteVisualTemporalCorpus\n";
        return 2;
    }
    pbremotevisualtemporalcorpus::TemporalCorpusReport report;
    std::string error;
    if (!pbremotevisualtemporalcorpus::BuildDefaultTemporalCorpus(report, error))
    {
        std::cerr << "[error] " << error << "\n";
        return 1;
    }
    std::cout << report.canonicalJson;
    return std::cout && report.productionAdmissionSafe && report.expectationsMatched ? 0 : 1;
}

} // namespace

int main(const int argumentCount, char* arguments[])
{
    try
    {
        return RunMain(argumentCount, arguments);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[error] exception: " << exception.what() << "\n";
    }
    catch (...)
    {
        std::cerr << "[error] unknown exception\n";
    }
    return 1;
}
