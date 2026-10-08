#include "nwbreader.hpp"

#include <H5Cpp.h>

#include "Types.hpp"
#include "io/BaseIO.hpp"
#include "io/ReadIO.hpp"
#include "io/hdf5/HDF5IO.hpp"

#include <algorithm>
#include <any>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <unordered_set>
#include <utility>

namespace nwb {

namespace {

using AQNWB::IO::BaseDataType;
using AQNWB::IO::DataBlockGeneric;
using AQNWB::IO::HDF5::HDF5IO;
using AQNWB::Types::SizeArray;

/// Timestamps read at a time when a series has timestamps instead of a rate.
constexpr std::size_t kTimestampChunk = 1 << 20;

/// HDF5 prints its error stack for every failed call unless told not to; we report errors ourselves.
void silenceHdf5()
{
    static std::once_flag once;
    std::call_once(once, [] {
        H5::Exception::dontPrint();
        H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
    });
}

/// Calls `f` with a pointer to the std::vector<T> in a numeric data block.
template <typename F>
void visitNumbers(const DataBlockGeneric& block, const F& f)
{
    switch (block.baseDataType.type) {
    case BaseDataType::T_U8: return f(std::any_cast<std::vector<std::uint8_t>>(&block.data));
    case BaseDataType::T_U16: return f(std::any_cast<std::vector<std::uint16_t>>(&block.data));
    case BaseDataType::T_U32: return f(std::any_cast<std::vector<std::uint32_t>>(&block.data));
    case BaseDataType::T_U64: return f(std::any_cast<std::vector<std::uint64_t>>(&block.data));
    case BaseDataType::T_I8: return f(std::any_cast<std::vector<std::int8_t>>(&block.data));
    case BaseDataType::T_I16: return f(std::any_cast<std::vector<std::int16_t>>(&block.data));
    case BaseDataType::T_I32: return f(std::any_cast<std::vector<std::int32_t>>(&block.data));
    case BaseDataType::T_I64: return f(std::any_cast<std::vector<std::int64_t>>(&block.data));
    case BaseDataType::T_F32: return f(std::any_cast<std::vector<float>>(&block.data));
    case BaseDataType::T_F64: return f(std::any_cast<std::vector<double>>(&block.data));
    default: throw Error("Not a numeric dataset");
    }
}

std::vector<double> toDoubles(const DataBlockGeneric& block)
{
    std::vector<double> values;
    visitNumbers(block, [&](const auto* data) {
        if (data == nullptr) {
            throw Error("Unexpected data type");
        }
        values.assign(data->begin(), data->end());
    });
    return values;
}

std::vector<std::string> toStrings(const DataBlockGeneric& block)
{
    if (block.baseDataType.type == BaseDataType::T_STR || block.baseDataType.type == BaseDataType::V_STR) {
        return std::any_cast<std::vector<std::string>>(block.data);
    }
    std::vector<std::string> strings;
    for (const double value : toDoubles(block)) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.15g", value);
        strings.emplace_back(text);
    }
    return strings;
}

/// The last component of a path, e.g. "LFP" for "/processing/ecephys/LFP".
std::string baseName(const std::string& path)
{
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parentPath(const std::string& path)
{
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash);
}

/// Letters, digits, '-' and '_', as NeuroScope uses stream ids in file names and on the command line.
std::string sanitizeId(const std::string& text)
{
    std::string id = text;
    for (char& c : id) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                             c == '-' || c == '_';
        if (!allowed) {
            c = '_';
        }
    }
    return id.empty() ? std::string("series") : id;
}

/// The rate with up to three decimals and without trailing zeros, in Hz or kHz, e.g. "30 kHz".
std::string formatRate(double rate)
{
    const bool kilo = rate >= 1000.0;
    char text[64];
    std::snprintf(text, sizeof(text), "%.3f", kilo ? rate / 1000.0 : rate);
    std::string formatted = text;
    formatted.erase(formatted.find_last_not_of('0') + 1);
    if (formatted.back() == '.') {
        formatted.pop_back();
    }
    return formatted + (kilo ? " kHz" : " Hz");
}

std::int64_t toNanoseconds(double seconds)
{
    return static_cast<std::int64_t>(std::llround(seconds * 1e9));
}

/// Collects events with their labels and sorts them by time.
class EventListBuilder {
public:
    explicit EventListBuilder(std::string name) { m_list.name = std::move(name); }

    /// Index of `label`, added if it is new.
    std::int32_t label(const std::string& label)
    {
        const auto found = std::find(m_list.labels.begin(), m_list.labels.end(), label);
        if (found != m_list.labels.end()) {
            return static_cast<std::int32_t>(found - m_list.labels.begin());
        }
        m_list.labels.push_back(label);
        return static_cast<std::int32_t>(m_list.labels.size() - 1);
    }

    void add(std::int64_t time, std::int32_t label) { m_events.emplace_back(time, label); }

    /// Adds the list to `lists` unless it has no events.
    void addTo(std::vector<EventList>& lists)
    {
        if (m_events.empty()) {
            return;
        }
        std::stable_sort(m_events.begin(), m_events.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [time, label] : m_events) {
            m_list.times.push_back(time);
            m_list.labelIndices.push_back(label);
        }
        lists.push_back(std::move(m_list));
    }

private:
    EventList m_list;
    std::vector<std::pair<std::int64_t, std::int32_t>> m_events;
};

} // namespace

/// Samples of a series placed on the sample axis of its stream.
struct Region {
    /// Stream sample of the first sample of the region; negative for regions before time 0.
    std::int64_t start;
    /// Index of the first sample within the data dataset.
    std::uint64_t dataFirst;
    std::int64_t count;
};

struct Series {
    /// Rank of the data dataset: 1 (one channel) or 2 (samples x channels).
    std::size_t rank = 2;
    /// Microvolts per stored unit, per channel.
    std::vector<double> scale;
    double offsetMicrovolts = 0.0;
    std::vector<Region> regions;
};

struct Reader::Impl {
    std::shared_ptr<HDF5IO> io;
    std::vector<Stream> streams;
    /// Where the samples of each stream are.
    std::vector<Series> seriesList;
    std::vector<EventList> eventLists;
    int defaultStream = 0;

    explicit Impl(const std::string& path);

    bool exists(const std::string& path) const { return io->objectExists(path); }
    bool attributeExists(const std::string& path) const { return io->attributeExists(path); }

    double scalarAttribute(const std::string& path, double fallback) const
    {
        if (!attributeExists(path)) {
            return fallback;
        }
        const auto values = toDoubles(io->readAttribute(path));
        return values.empty() ? fallback : values.front();
    }

    std::vector<double> doubles(const std::string& path, const SizeArray& start = {},
                                const SizeArray& count = {})
    {
        return toDoubles(io->readDataset(path, start, count));
    }

    void readSeries(const std::string& path);
    void placeByRate(const std::string& path, std::int64_t samples, Stream& stream, Series& series);
    void placeByTimestamps(const std::string& path, std::int64_t samples, Stream& stream,
                           Series& series);
    void readElectrodes(const std::string& path, Stream& stream);
    void assignIds();
    void readEventLists();
};

Reader::Impl::Impl(const std::string& path)
{
    silenceHdf5();
    io = std::make_shared<HDF5IO>(path);
    try {
        if (io->open(AQNWB::IO::FileMode::ReadOnly) != AQNWB::Types::Status::Success) {
            throw Error("Cannot open " + path);
        }
    } catch (const H5::Exception& e) {
        throw Error("Cannot open " + path + ": " + e.getDetailMsg());
    }

    const auto found = io->findTypes("/", {"core::ElectricalSeries"},
                                     AQNWB::IO::SearchMode::CONTINUE_ON_TYPE);
    // Acquired data first, then by path, so that the order does not depend on the hash map.
    std::vector<std::string> paths;
    for (const auto& [seriesPath, type] : found) {
        paths.push_back(seriesPath.front() == '/' ? seriesPath : "/" + seriesPath);
    }
    std::sort(paths.begin(), paths.end(), [](const std::string& a, const std::string& b) {
        const bool aAcquired = a.rfind("/acquisition/", 0) == 0;
        const bool bAcquired = b.rfind("/acquisition/", 0) == 0;
        return aAcquired != bAcquired ? aAcquired : a < b;
    });
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

    for (const auto& seriesPath : paths) {
        readSeries(seriesPath);
    }
    if (streams.empty()) {
        throw Error(path + " has no ElectricalSeries with data.");
    }
    assignIds();

    double bestRate = -1.0;
    bool bestAcquired = false;
    for (std::size_t i = 0; i < streams.size(); ++i) {
        const bool acquired = streams[i].path.rfind("/acquisition/", 0) == 0;
        if ((acquired && !bestAcquired) ||
            (acquired == bestAcquired && streams[i].samplingRate > bestRate)) {
            defaultStream = static_cast<int>(i);
            bestRate = streams[i].samplingRate;
            bestAcquired = acquired;
        }
    }

    readEventLists();
}

void Reader::Impl::readSeries(const std::string& path)
{
    const std::string dataPath = path + "/data";
    if (!exists(dataPath)) {
        return;
    }
    const auto shape = io->getStorageObjectShape(dataPath);
    if (shape.empty() || shape.size() > 2 || shape[0] == 0) {
        return;
    }

    Stream stream;
    stream.path = path;
    stream.channelCount = shape.size() == 2 ? static_cast<std::int32_t>(shape[1]) : 1;
    if (stream.channelCount <= 0) {
        return;
    }
    Series series;
    series.rank = shape.size();

    const double conversion = scalarAttribute(dataPath + "/conversion", 1.0);
    series.offsetMicrovolts = scalarAttribute(dataPath + "/offset", 0.0) * 1e6;
    series.scale.assign(static_cast<std::size_t>(stream.channelCount), conversion * 1e6);
    if (exists(path + "/channel_conversion")) {
        const auto channelConversion = doubles(path + "/channel_conversion");
        for (std::size_t c = 0; c < series.scale.size() && c < channelConversion.size(); ++c) {
            series.scale[c] *= channelConversion[c];
        }
    }

    const auto samples = static_cast<std::int64_t>(shape[0]);
    if (exists(path + "/starting_time")) {
        placeByRate(path, samples, stream, series);
    } else if (exists(path + "/timestamps")) {
        placeByTimestamps(path, samples, stream, series);
    } else {
        return;
    }
    if (!(stream.samplingRate > 0.0) || !std::isfinite(stream.samplingRate) || stream.sampleCount <= 0) {
        return;
    }

    readElectrodes(path, stream);
    stream.label = path + ": " + formatRate(stream.samplingRate) + ", " +
                   std::to_string(stream.channelCount) +
                   (stream.channelCount == 1 ? " channel" : " channels");
    streams.push_back(std::move(stream));
    seriesList.push_back(std::move(series));
}

void Reader::Impl::placeByRate(const std::string& path, std::int64_t samples, Stream& stream,
                               Series& series)
{
    const auto start = doubles(path + "/starting_time");
    stream.samplingRate = scalarAttribute(path + "/starting_time/rate", 0.0);
    if (start.empty() || !(stream.samplingRate > 0.0)) {
        return;
    }
    const auto first = static_cast<std::int64_t>(std::llround(start.front() * stream.samplingRate));
    series.regions.push_back({first, 0, samples});
    stream.sampleCount = std::max<std::int64_t>(0, first + samples);
}

void Reader::Impl::placeByTimestamps(const std::string& path, std::int64_t samples, Stream& stream,
                                     Series& series)
{
    const std::string timestampsPath = path + "/timestamps";
    const auto length = static_cast<std::int64_t>(io->getStorageObjectShape(timestampsPath).at(0));
    samples = std::min(samples, length);
    if (samples <= 0) {
        return;
    }

    // The sampling period is the median of the positive differences at the start of the series.
    {
        const auto count = static_cast<std::size_t>(std::min<std::int64_t>(samples, kTimestampChunk));
        const auto head = doubles(timestampsPath, {0}, {count});
        std::vector<double> periods;
        for (std::size_t i = 1; i < head.size(); ++i) {
            if (head[i] > head[i - 1]) {
                periods.push_back(head[i] - head[i - 1]);
            }
        }
        if (periods.empty()) {
            // A single sample or no increasing timestamps: nothing to derive a rate from.
            return;
        }
        std::nth_element(periods.begin(), periods.begin() + periods.size() / 2, periods.end());
        // In mHz, so that timestamps 1 ms apart give 1000 Hz rather than 999.99999999 Hz.
        stream.samplingRate = std::round(1000.0 / periods[periods.size() / 2]) / 1000.0;
    }

    // A region continues while the timestamps advance by one period, give or take half a period.
    const double period = 1.0 / stream.samplingRate;
    double previous = 0.0;
    for (std::int64_t chunkStart = 0; chunkStart < samples;) {
        const auto count = static_cast<std::size_t>(
            std::min<std::int64_t>(samples - chunkStart, kTimestampChunk));
        const auto times = doubles(timestampsPath, {static_cast<std::size_t>(chunkStart)}, {count});
        for (std::size_t i = 0; i < times.size(); ++i) {
            const auto index = chunkStart + static_cast<std::int64_t>(i);
            const double t = times[i];
            if (index > 0 && std::abs(t - previous - period) <= 0.5 * period) {
                ++series.regions.back().count;
            } else {
                series.regions.push_back({static_cast<std::int64_t>(std::llround(t * stream.samplingRate)),
                                          static_cast<std::uint64_t>(index), 1});
            }
            previous = t;
        }
        chunkStart += static_cast<std::int64_t>(count);
    }
    for (const auto& region : series.regions) {
        stream.sampleCount = std::max(stream.sampleCount, region.start + region.count);
    }
}

void Reader::Impl::readElectrodes(const std::string& path, Stream& stream)
{
    try {
        const std::string electrodesPath = path + "/electrodes";
        if (!exists(electrodesPath)) {
            return;
        }
        std::string table = "/general/extracellular_ephys/electrodes";
        if (attributeExists(electrodesPath + "/table")) {
            table = io->readReferenceAttribute(electrodesPath + "/table");
        }
        const auto rows = doubles(electrodesPath);
        if (rows.size() != static_cast<std::size_t>(stream.channelCount)) {
            return;
        }

        std::vector<std::string> names;
        for (const char* column : {"channel_name", "label"}) {
            if (exists(table + "/" + column)) {
                names = toStrings(io->readDataset(table + "/" + column));
                break;
            }
        }
        if (names.empty() && exists(table + "/id")) {
            names = toStrings(io->readDataset(table + "/id"));
        }
        std::vector<std::string> groupNames;
        if (exists(table + "/group_name")) {
            groupNames = toStrings(io->readDataset(table + "/group_name"));
        }

        // Groups are numbered in the order of the table, so that they are the same in all streams.
        std::map<std::string, int> groupIndices;
        for (const auto& group : groupNames) {
            groupIndices.emplace(group, static_cast<int>(groupIndices.size()));
        }
        std::vector<std::string> labels;
        std::vector<int> groups;
        for (const double row : rows) {
            const auto r = static_cast<std::size_t>(row);
            if (row < 0 || r >= names.size()) {
                return;
            }
            labels.push_back(names[r]);
            groups.push_back(r < groupNames.size() ? groupIndices[groupNames[r]] : 0);
        }
        stream.channelLabels = std::move(labels);
        stream.channelGroups = std::move(groups);
    } catch (const std::exception&) {
        // Without the electrodes table, the channels are numbered and form one group.
        stream.channelLabels.clear();
        stream.channelGroups.clear();
    }
}

void Reader::Impl::assignIds()
{
    // The name of the series, preceded by the name of its parent if several series have the name
    // (e.g. "LFP" in two processing modules), and numbered if that is not unique either.
    std::map<std::string, int> uses;
    for (const auto& stream : streams) {
        ++uses[baseName(stream.path)];
    }
    std::set<std::string> ids;
    for (auto& stream : streams) {
        std::string name = baseName(stream.path);
        if (uses[name] > 1) {
            name = baseName(parentPath(stream.path)) + "_" + name;
        }
        std::string id = sanitizeId(name);
        for (int n = 2; ids.count(id) != 0; ++n) {
            id = sanitizeId(name) + "_" + std::to_string(n);
        }
        ids.insert(id);
        stream.id = id;
    }
}

void Reader::Impl::readEventLists()
{
    std::unordered_map<std::string, std::string> found;
    try {
        found = io->findTypes("/", {"core::TimeIntervals"}, AQNWB::IO::SearchMode::CONTINUE_ON_TYPE);
    } catch (const std::exception&) {
        return;
    }
    std::vector<std::string> paths;
    for (const auto& [path, type] : found) {
        paths.push_back(path.front() == '/' ? path : "/" + path);
    }
    std::sort(paths.begin(), paths.end());

    for (const auto& path : paths) {
        try {
            const auto starts = doubles(path + "/start_time");
            const auto stops = exists(path + "/stop_time") ? doubles(path + "/stop_time")
                                                           : std::vector<double>();
            EventListBuilder list(baseName(path));
            const auto start = list.label("start");
            const auto stop = stops.empty() ? 0 : list.label("stop");
            for (std::size_t i = 0; i < starts.size(); ++i) {
                list.add(toNanoseconds(starts[i]), start);
                if (i < stops.size()) {
                    list.add(toNanoseconds(stops[i]), stop);
                }
            }
            list.addTo(eventLists);
        } catch (const std::exception&) {
            // Tables that cannot be read are left out.
        }
    }
}

bool Reader::isNwbFile(const std::string& path)
{
    silenceHdf5();
    try {
        if (H5Fis_accessible(path.c_str(), H5P_DEFAULT) <= 0) {
            return false;
        }
        HDF5IO io(path);
        if (io.open(AQNWB::IO::FileMode::ReadOnly) != AQNWB::Types::Status::Success) {
            return false;
        }
        return io.attributeExists("/nwb_version");
    } catch (...) {
        return false;
    }
}

Reader::Reader(const std::string& path)
{
    try {
        m_impl = std::make_unique<Impl>(path);
    } catch (const Error&) {
        throw;
    } catch (const H5::Exception& e) {
        throw Error(path + ": " + e.getDetailMsg());
    } catch (const std::exception& e) {
        throw Error(path + ": " + e.what());
    }
}

Reader::~Reader() = default;

const std::vector<Stream>& Reader::streams() const
{
    return m_impl->streams;
}

int Reader::defaultStream() const
{
    return m_impl->defaultStream;
}

const std::vector<EventList>& Reader::eventLists() const
{
    return m_impl->eventLists;
}

void Reader::read(int streamIndex, std::int64_t first, std::int64_t count, double* microvolts)
{
    if (streamIndex < 0 || static_cast<std::size_t>(streamIndex) >= m_impl->streams.size()) {
        throw Error("No stream " + std::to_string(streamIndex));
    }
    const auto& stream = m_impl->streams[static_cast<std::size_t>(streamIndex)];
    const auto& series = m_impl->seriesList[static_cast<std::size_t>(streamIndex)];
    const auto channels = static_cast<std::size_t>(stream.channelCount);
    std::fill(microvolts, microvolts + static_cast<std::size_t>(count) * channels, 0.0);

    const std::int64_t end = first + count;
    for (const auto& region : series.regions) {
        const std::int64_t from = std::max(first, region.start);
        const std::int64_t to = std::min(end, region.start + region.count);
        if (from >= to) {
            continue;
        }
        const auto samples = static_cast<std::size_t>(to - from);
        const auto dataFirst = static_cast<std::size_t>(region.dataFirst + static_cast<std::uint64_t>(from - region.start));
        SizeArray start{dataFirst};
        SizeArray size{samples};
        if (series.rank == 2) {
            start.push_back(0);
            size.push_back(channels);
        }
        try {
            const auto block = m_impl->io->readDataset(stream.path + "/data", start, size);
            visitNumbers(block, [&](const auto* data) {
                if (data == nullptr || data->size() != samples * channels) {
                    throw Error("Unexpected data in " + stream.path);
                }
                double* out = microvolts + static_cast<std::size_t>(from - first) * channels;
                const auto* in = data->data();
                for (std::size_t i = 0; i < samples; ++i) {
                    for (std::size_t c = 0; c < channels; ++c) {
                        out[c] = static_cast<double>(in[c]) * series.scale[c] + series.offsetMicrovolts;
                    }
                    in += channels;
                    out += channels;
                }
            });
        } catch (const H5::Exception& e) {
            throw Error(stream.path + ": " + e.getDetailMsg());
        }
    }
}

} // namespace nwb
