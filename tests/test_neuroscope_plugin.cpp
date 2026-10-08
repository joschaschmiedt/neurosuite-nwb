// Loads the NeuroScope plugin and reads the files of data/make_test_files.py through its C interface.

#include <neuroscope/neuroscope_plugin.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

const ns_plugin* loadPlugin()
{
#ifdef _WIN32
    HMODULE library = LoadLibraryA(NWB_NEUROSCOPE_PLUGIN);
    auto entry = library != nullptr
                     ? reinterpret_cast<ns_plugin_entry>(GetProcAddress(library, NS_PLUGIN_ENTRY_NAME))
                     : nullptr;
#else
    void* library = dlopen(NWB_NEUROSCOPE_PLUGIN, RTLD_NOW | RTLD_LOCAL);
    auto entry = library != nullptr
                     ? reinterpret_cast<ns_plugin_entry>(dlsym(library, NS_PLUGIN_ENTRY_NAME))
                     : nullptr;
#endif
    return entry != nullptr ? entry(NS_PLUGIN_API_VERSION) : nullptr;
}

const ns_plugin* plugin()
{
    static const ns_plugin* loaded = loadPlugin();
    return loaded;
}

std::string testFile(const char* name)
{
    return std::string(NWB_TEST_DATA_DIR) + "/" + name;
}

/// A file opened through the plugin, closed at the end of the test.
class OpenFile {
public:
    explicit OpenFile(const std::string& path)
    {
        ns_error error{};
        if (plugin()->open(path.c_str(), &m_file, &error) != NS_OK) {
            ADD_FAILURE() << "Cannot open " << path << ": " << error.message;
            m_file = nullptr;
        }
    }
    ~OpenFile()
    {
        if (m_file != nullptr) {
            plugin()->close(m_file);
        }
    }
    OpenFile(const OpenFile&) = delete;
    OpenFile& operator=(const OpenFile&) = delete;

    ns_file* get() const { return m_file; }

    ns_stream_info stream(int index) const
    {
        ns_stream_info info{};
        EXPECT_EQ(plugin()->stream_info(m_file, index, &info), NS_OK);
        return info;
    }

    std::vector<double> read(int stream, std::int64_t first, std::int64_t count) const
    {
        std::vector<double> values(static_cast<std::size_t>(count * this->stream(stream).channel_count), -1.0);
        ns_error error{};
        EXPECT_EQ(plugin()->read(m_file, stream, first, count, values.data(), &error), NS_OK)
            << error.message;
        return values;
    }

    std::vector<std::string> labels(int stream) const
    {
        std::vector<std::string> labels;
        for (int c = 0; c < this->stream(stream).channel_count; ++c) {
            const char* label = plugin()->channel_label(m_file, stream, c);
            labels.emplace_back(label != nullptr ? label : "(null)");
        }
        return labels;
    }

    std::vector<int> groups(int stream) const
    {
        std::vector<int> groups;
        for (int c = 0; c < this->stream(stream).channel_count; ++c) {
            groups.push_back(plugin()->channel_group(m_file, stream, c));
        }
        return groups;
    }

private:
    ns_file* m_file = nullptr;
};

TEST(NeuroScopePlugin, Describes)
{
    ASSERT_NE(plugin(), nullptr);
    EXPECT_EQ(plugin()->api_version, static_cast<std::uint32_t>(NS_PLUGIN_API_VERSION));
    EXPECT_EQ(plugin()->size, sizeof(ns_plugin));
    EXPECT_STREQ(plugin()->extensions[0], "nwb");
    EXPECT_EQ(plugin()->extensions[1], nullptr);
}

TEST(NeuroScopePlugin, Probes)
{
    ASSERT_NE(plugin(), nullptr);
    EXPECT_EQ(plugin()->probe(testFile("rate.nwb").c_str()), 100);
    EXPECT_EQ(plugin()->probe(testFile("make_test_files.py").c_str()), 0);
    EXPECT_EQ(plugin()->probe(testFile("missing.nwb").c_str()), 0);
}

TEST(NeuroScopePlugin, ReportsErrors)
{
    ASSERT_NE(plugin(), nullptr);
    ns_file* file = nullptr;
    ns_error error{};
    EXPECT_EQ(plugin()->open(testFile("make_test_files.py").c_str(), &file, &error), NS_ERROR);
    EXPECT_NE(std::string(error.message), "");
}

TEST(NeuroScopePlugin, ListsSeriesAsStreams)
{
    ASSERT_NE(plugin(), nullptr);
    OpenFile file(testFile("rate.nwb"));
    ASSERT_NE(file.get(), nullptr);
    ASSERT_EQ(plugin()->stream_count(file.get()), 2);
    EXPECT_EQ(plugin()->default_stream(file.get()), 0);

    const auto raw = file.stream(0);
    EXPECT_STREQ(raw.id, "raw");
    EXPECT_STREQ(raw.label, "/acquisition/raw: 1 kHz, 3 channels");
    EXPECT_EQ(raw.channel_count, 3);
    EXPECT_DOUBLE_EQ(raw.sampling_rate, 1000.0);
    // 20 samples from 0.5 s on.
    EXPECT_EQ(raw.sample_count, 520);

    const auto lfp = file.stream(1);
    EXPECT_STREQ(lfp.id, "lfp");
    EXPECT_STREQ(lfp.label, "/processing/ecephys/LFP/lfp: 100 Hz, 2 channels");
    EXPECT_EQ(lfp.channel_count, 2);
    EXPECT_DOUBLE_EQ(lfp.sampling_rate, 100.0);
    EXPECT_EQ(lfp.sample_count, 10);

    ns_stream_info info{};
    EXPECT_EQ(plugin()->stream_info(file.get(), 2, &info), NS_ERROR);
}

TEST(NeuroScopePlugin, LabelsAndGroupsChannelsByElectrode)
{
    ASSERT_NE(plugin(), nullptr);
    OpenFile file(testFile("rate.nwb"));
    ASSERT_NE(file.get(), nullptr);
    EXPECT_EQ(file.labels(0), (std::vector<std::string>{"e0", "e1", "e2"}));
    EXPECT_EQ(file.groups(0), (std::vector<int>{0, 0, 1}));
    // The groups keep their numbers in a stream with other electrodes.
    EXPECT_EQ(file.labels(1), (std::vector<std::string>{"e2", "e0"}));
    EXPECT_EQ(file.groups(1), (std::vector<int>{1, 0}));
    EXPECT_EQ(plugin()->channel_label(file.get(), 0, 3), nullptr);
}

TEST(NeuroScopePlugin, ScalesToMicrovolts)
{
    ASSERT_NE(plugin(), nullptr);
    OpenFile file(testFile("rate.nwb"));
    ASSERT_NE(file.get(), nullptr);

    // Samples 498-501: two before the series starts at 0.5 s, and its first two.
    const auto values = file.read(0, 498, 4);
    const double channelConversion[] = {1.0, 2.0, 0.5};
    for (int i = 0; i < 4; ++i) {
        for (int c = 0; c < 3; ++c) {
            const int stored = 100 * c + (i - 2);
            // data * channel_conversion * conversion (2e-6 V) + offset (1e-6 V), in µV
            const double expected = i < 2 ? 0.0 : stored * channelConversion[c] * 2.0 + 1.0;
            EXPECT_DOUBLE_EQ(values[static_cast<std::size_t>(i * 3 + c)], expected) << i << ", " << c;
        }
    }

    const auto lfp = file.read(1, 8, 2);
    EXPECT_NEAR(lfp[0], 8.0, 1e-5);
    EXPECT_NEAR(lfp[1], 9.0, 1e-5);
    EXPECT_NEAR(lfp[2], 9.0, 1e-5);
    EXPECT_NEAR(lfp[3], 10.0, 1e-5);
}

TEST(NeuroScopePlugin, PlacesTimestampsWithGaps)
{
    ASSERT_NE(plugin(), nullptr);
    OpenFile file(testFile("timestamps.nwb"));
    ASSERT_NE(file.get(), nullptr);
    ASSERT_EQ(plugin()->stream_count(file.get()), 4);

    const auto gaps = file.stream(1);
    EXPECT_STREQ(gaps.id, "gaps");
    EXPECT_DOUBLE_EQ(gaps.sampling_rate, 1000.0);
    EXPECT_EQ(gaps.sample_count, 23);
    // Timestamps 10-14 ms and 20-22 ms.
    EXPECT_EQ(file.read(1, 8, 15), (std::vector<double>{0, 0, 1, 2, 3, 4, 5, 0, 0, 0, 0, 0, 6, 7, 8}));
}

TEST(NeuroScopePlugin, PlacesTimestampsThatGoBackwards)
{
    ASSERT_NE(plugin(), nullptr);
    OpenFile file(testFile("timestamps.nwb"));
    ASSERT_NE(file.get(), nullptr);

    const auto backwards = file.stream(0);
    EXPECT_STREQ(backwards.id, "backwards");
    EXPECT_EQ(backwards.sample_count, 7);
    // Timestamps 5 and 6 ms, then 1 and 2 ms.
    EXPECT_EQ(file.read(0, 0, 7), (std::vector<double>{0, 3, 4, 0, 0, 1, 2}));
}

TEST(NeuroScopePlugin, NamesSeriesOfTheSameNameByTheirParents)
{
    ASSERT_NE(plugin(), nullptr);
    OpenFile file(testFile("timestamps.nwb"));
    ASSERT_NE(file.get(), nullptr);
    EXPECT_STREQ(file.stream(2).id, "m1_same");
    EXPECT_STREQ(file.stream(3).id, "m2_same");
}

TEST(NeuroScopePlugin, ListsTimeIntervalsAsEvents)
{
    ASSERT_NE(plugin(), nullptr);
    OpenFile file(testFile("rate.nwb"));
    ASSERT_NE(file.get(), nullptr);
    ASSERT_EQ(plugin()->event_list_count(file.get()), 2);

    ns_event_list_info epochs{};
    ASSERT_EQ(plugin()->event_list_info(file.get(), 0, &epochs), NS_OK);
    EXPECT_STREQ(epochs.name, "epochs");
    EXPECT_EQ(epochs.count, 2);

    ns_event_list_info trials{};
    ASSERT_EQ(plugin()->event_list_info(file.get(), 1, &trials), NS_OK);
    EXPECT_STREQ(trials.name, "trials");
    ASSERT_EQ(trials.count, 4);
    ASSERT_EQ(trials.label_count, 2);
    EXPECT_STREQ(plugin()->event_label(file.get(), 1, 0), "start");
    EXPECT_STREQ(plugin()->event_label(file.get(), 1, 1), "stop");
    EXPECT_EQ(plugin()->event_label(file.get(), 1, 2), nullptr);

    // In time order, although the second trial comes first.
    std::vector<std::int64_t> times(4);
    std::vector<std::int32_t> labels(4);
    ns_error error{};
    ASSERT_EQ(plugin()->read_events(file.get(), 1, times.data(), labels.data(), &error), NS_OK);
    EXPECT_EQ(times, (std::vector<std::int64_t>{200'000'000, 300'000'000, 500'000'000, 600'000'000}));
    EXPECT_EQ(labels, (std::vector<std::int32_t>{0, 1, 0, 1}));
}

TEST(NeuroScopePlugin, ReadsFurtherFiles)
{
    ASSERT_NE(plugin(), nullptr);
    const std::string directory = NWB_EXTRA_TEST_DATA_DIR;
    if (directory.empty()) {
        GTEST_SKIP() << "Set NEUROSUITE_NWB_EXTRA_TEST_DATA to a directory with NWB files.";
    }
    int files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".nwb") {
            continue;
        }
        SCOPED_TRACE(entry.path().string());
        ++files;
        EXPECT_EQ(plugin()->probe(entry.path().string().c_str()), 100);
        OpenFile file(entry.path().string());
        ASSERT_NE(file.get(), nullptr);
        for (int s = 0; s < plugin()->stream_count(file.get()); ++s) {
            // The first and the last second of each stream.
            const auto info = file.stream(s);
            const auto count = std::min<std::int64_t>(info.sample_count,
                                                      static_cast<std::int64_t>(info.sampling_rate));
            file.read(s, 0, count);
            file.read(s, info.sample_count - count, count);
        }
    }
    EXPECT_GT(files, 0);
}

} // namespace
