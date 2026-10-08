// Reads the continuous data and the events of NWB files for the programs of the neurosuite. It
// knows NWB but none of the programs, which adapt it to their plugin interfaces.
//
// Streams: each ElectricalSeries is a stream, wherever it is in the file (acquisition, LFP in a
// processing module, ...). Its channels are the electrodes it references.
//
// Time: sample k of a stream is at k / samplingRate seconds on the file's clock, i.e. relative to
// timestamps_reference_time. A series with starting_time and rate is one region that starts at
// starting_time. A series with timestamps is split into regions wherever a timestamp is not one
// sample period after the previous one, and the sampling rate is the median of these periods; each
// region starts at its first timestamp. Such rates are rounded to mHz. Samples outside the regions read as 0, samples before
// time 0 are left out.
//
// Values: data * channel_conversion * conversion + offset, in microvolts.
//
// Channels: labelled by the channel_name or label column of the electrodes table, or by electrode
// id; grouped by its group_name column, with the groups numbered in the order of the table so that
// they are the same in all streams.
//
// Events: one list per TimeIntervals table (trials, epochs, ...), with the start and stop of each
// interval.

#ifndef NEUROSUITE_NWBREADER_HPP
#define NEUROSUITE_NWBREADER_HPP

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace nwb {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Stream {
    /// Unique within the file and only letters, digits, '-' and '_', e.g. "ElectricalSeries".
    std::string id;
    /// For the user, e.g. "/acquisition/ElectricalSeries: 30 kHz, 32 channels".
    std::string label;
    /// Path of the ElectricalSeries in the file.
    std::string path;
    double samplingRate = 0.0;
    std::int32_t channelCount = 0;
    /// Number of samples from time 0 to the end of the last recorded sample.
    std::int64_t sampleCount = 0;
    /// Empty if the electrodes table cannot be read.
    std::vector<std::string> channelLabels;
    /// Group of each channel, from 0.
    std::vector<int> channelGroups;
};

struct EventList {
    std::string name;
    std::vector<std::string> labels;
    /// Events in time order: times in nanoseconds on the file's clock and label indices.
    std::vector<std::int64_t> times;
    std::vector<std::int32_t> labelIndices;
};

class Reader {
public:
    /// Whether the file is an HDF5 file with the attributes of an NWB file.
    static bool isNwbFile(const std::string& path);

    /// Opens a file for reading; throws Error if it cannot be read or has no ElectricalSeries.
    explicit Reader(const std::string& path);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    const std::vector<Stream>& streams() const;
    /// The stream with the highest sampling rate, preferring acquired data over processed data.
    int defaultStream() const;
    const std::vector<EventList>& eventLists() const;

    /// Reads `count` samples of all channels of a stream from sample `first` on, row-major:
    /// microvolts[sample * channelCount + channel].
    void read(int stream, std::int64_t first, std::int64_t count, double* microvolts);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace nwb

#endif
