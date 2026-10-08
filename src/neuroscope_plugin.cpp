// NeuroScope file format plugin for NWB files: the C interface of neuroscope_plugin.h on top of
// nwb::Reader, which describes how streams, time, values, channels and events are read.

#include "nwbreader.hpp"

#include <neuroscope/neuroscope_plugin.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <memory>
#include <new>
#include <string>

struct ns_file {
    explicit ns_file(const std::string& path) : reader(path) {}
    nwb::Reader reader;
};

namespace {

void setError(ns_error* error, const std::string& message)
{
    if (error != nullptr) {
        std::snprintf(error->message, sizeof(error->message), "%s", message.c_str());
    }
}

/// Runs `f` and turns exceptions into NS_ERROR with a message.
template <typename F>
int guarded(ns_error* error, const F& f)
{
    try {
        f();
        return NS_OK;
    } catch (const std::bad_alloc&) {
        setError(error, "Out of memory.");
    } catch (const std::exception& e) {
        setError(error, e.what());
    } catch (...) {
        setError(error, "Unknown error.");
    }
    return NS_ERROR;
}

const nwb::Stream* findStream(ns_file* file, int stream)
{
    const auto& streams = file->reader.streams();
    if (stream < 0 || static_cast<std::size_t>(stream) >= streams.size()) {
        return nullptr;
    }
    return &streams[static_cast<std::size_t>(stream)];
}

const nwb::EventList* findEventList(ns_file* file, int list)
{
    const auto& lists = file->reader.eventLists();
    if (list < 0 || static_cast<std::size_t>(list) >= lists.size()) {
        return nullptr;
    }
    return &lists[static_cast<std::size_t>(list)];
}

int probe(const char* path)
{
    return nwb::Reader::isNwbFile(path) ? 100 : 0;
}

int openFile(const char* path, ns_file** file, ns_error* error)
{
    return guarded(error, [&] { *file = new ns_file(path); });
}

void closeFile(ns_file* file)
{
    delete file;
}

int streamCount(ns_file* file)
{
    return static_cast<int>(file->reader.streams().size());
}

int defaultStream(ns_file* file)
{
    return file->reader.defaultStream();
}

int streamInfo(ns_file* file, int stream, ns_stream_info* info)
{
    const auto* s = findStream(file, stream);
    if (s == nullptr) {
        return NS_ERROR;
    }
    info->id = s->id.c_str();
    info->label = s->label.c_str();
    info->channel_count = s->channelCount;
    info->sampling_rate = s->samplingRate;
    info->sample_count = s->sampleCount;
    return NS_OK;
}

const char* channelLabel(ns_file* file, int stream, int channel)
{
    const auto* s = findStream(file, stream);
    if (s == nullptr || channel < 0 || static_cast<std::size_t>(channel) >= s->channelLabels.size()) {
        return nullptr;
    }
    return s->channelLabels[static_cast<std::size_t>(channel)].c_str();
}

int channelGroup(ns_file* file, int stream, int channel)
{
    const auto* s = findStream(file, stream);
    if (s == nullptr || channel < 0 || static_cast<std::size_t>(channel) >= s->channelGroups.size()) {
        return 0;
    }
    return s->channelGroups[static_cast<std::size_t>(channel)];
}

int readSamples(ns_file* file, int stream, std::int64_t first, std::int64_t count, double* microvolts,
                ns_error* error)
{
    return guarded(error, [&] { file->reader.read(stream, first, count, microvolts); });
}

int eventListCount(ns_file* file)
{
    return static_cast<int>(file->reader.eventLists().size());
}

int eventListInfo(ns_file* file, int list, ns_event_list_info* info)
{
    const auto* l = findEventList(file, list);
    if (l == nullptr) {
        return NS_ERROR;
    }
    info->name = l->name.c_str();
    info->count = static_cast<std::int64_t>(l->times.size());
    info->label_count = static_cast<std::int32_t>(l->labels.size());
    return NS_OK;
}

const char* eventLabel(ns_file* file, int list, int label)
{
    const auto* l = findEventList(file, list);
    if (l == nullptr || label < 0 || static_cast<std::size_t>(label) >= l->labels.size()) {
        return nullptr;
    }
    return l->labels[static_cast<std::size_t>(label)].c_str();
}

int readEvents(ns_file* file, int list, std::int64_t* timesNs, std::int32_t* labels, ns_error* error)
{
    const auto* l = findEventList(file, list);
    if (l == nullptr) {
        setError(error, "No event list " + std::to_string(list));
        return NS_ERROR;
    }
    std::copy(l->times.begin(), l->times.end(), timesNs);
    std::copy(l->labelIndices.begin(), l->labelIndices.end(), labels);
    return NS_OK;
}

const char* const extensions[] = {"nwb", nullptr};

ns_plugin makePlugin()
{
    ns_plugin plugin{};
    plugin.api_version = NS_PLUGIN_API_VERSION;
    plugin.size = sizeof(ns_plugin);
    plugin.name = "Neurodata Without Borders";
    plugin.extensions = extensions;
    plugin.probe = probe;
    plugin.open = openFile;
    plugin.close = closeFile;
    plugin.stream_count = streamCount;
    plugin.default_stream = defaultStream;
    plugin.stream_info = streamInfo;
    plugin.channel_label = channelLabel;
    plugin.read = readSamples;
    plugin.event_list_count = eventListCount;
    plugin.event_list_info = eventListInfo;
    plugin.event_label = eventLabel;
    plugin.read_events = readEvents;
    plugin.channel_group = channelGroup;
    return plugin;
}

const ns_plugin plugin = makePlugin();

} // namespace

extern "C" NS_PLUGIN_EXPORT const ns_plugin* neuroscope_plugin(std::uint32_t host_api_version)
{
    (void)host_api_version;
    return &plugin;
}
