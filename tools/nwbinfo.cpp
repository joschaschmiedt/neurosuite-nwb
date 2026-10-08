// Prints the streams and event lists that the neurosuite reads from NWB files, and optionally the
// first samples of a stream.
//
//   nwbinfo <file.nwb>... [--samples <count>] [--stream <index>]

#include "nwbreader.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace {

void printFile(const std::string& path, int sampleCount, int streamIndex)
{
    nwb::Reader reader(path);
    std::printf("%s\n", path.c_str());
    const auto& streams = reader.streams();
    for (std::size_t i = 0; i < streams.size(); ++i) {
        const auto& s = streams[i];
        std::printf("  stream %zu%s: %s [%s], %.6g Hz, %d channels, %lld samples (%.3f s)\n", i,
                    static_cast<int>(i) == reader.defaultStream() ? " (default)" : "", s.id.c_str(),
                    s.label.c_str(), s.samplingRate, s.channelCount,
                    static_cast<long long>(s.sampleCount), static_cast<double>(s.sampleCount) / s.samplingRate);
        if (!s.channelLabels.empty()) {
            std::printf("    channels:");
            for (std::size_t c = 0; c < s.channelLabels.size() && c < 8; ++c) {
                std::printf(" %s (group %d)", s.channelLabels[c].c_str(), s.channelGroups[c]);
            }
            std::printf("%s\n", s.channelLabels.size() > 8 ? " ..." : "");
        }
    }
    for (const auto& list : reader.eventLists()) {
        std::printf("  events %s: %zu, labels:", list.name.c_str(), list.times.size());
        for (const auto& label : list.labels) {
            std::printf(" \"%s\"", label.c_str());
        }
        std::printf(", first at %.6f s\n", static_cast<double>(list.times.front()) / 1e9);
    }

    if (sampleCount > 0) {
        const int index = streamIndex >= 0 ? streamIndex : reader.defaultStream();
        const auto& s = streams.at(static_cast<std::size_t>(index));
        const auto count = std::min<std::int64_t>(sampleCount, s.sampleCount);
        std::vector<double> microvolts(static_cast<std::size_t>(count * s.channelCount));
        reader.read(index, 0, count, microvolts.data());
        for (std::int64_t i = 0; i < count; ++i) {
            std::printf("  %lld:", static_cast<long long>(i));
            for (std::int32_t c = 0; c < std::min(s.channelCount, 8); ++c) {
                std::printf(" %.3f", microvolts[static_cast<std::size_t>(i * s.channelCount + c)]);
            }
            std::printf("\n");
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> paths;
    int sampleCount = 0;
    int streamIndex = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--samples") == 0 && i + 1 < argc) {
            sampleCount = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--stream") == 0 && i + 1 < argc) {
            streamIndex = std::atoi(argv[++i]);
        } else {
            paths.emplace_back(argv[i]);
        }
    }
    if (paths.empty()) {
        std::fprintf(stderr, "Usage: nwbinfo <file.nwb>... [--samples <count>] [--stream <index>]\n");
        return 2;
    }
    int status = 0;
    for (const auto& path : paths) {
        try {
            printFile(path, sampleCount, streamIndex);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());
            status = 1;
        }
    }
    return status;
}
