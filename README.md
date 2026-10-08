# neurosuite-nwb

Reads [Neurodata Without Borders](https://nwb.org) (NWB) files for the programs of the
[neurosuite](https://github.com/neurosuite). For now it is a plugin that lets
[NeuroScope](https://github.com/neurosuite/neuroscope) open `.nwb` files; Klusters is to follow.

NWB files are read with [aqnwb](https://github.com/NeurodataWithoutBorders/aqnwb), the C++ API of
the NWB project, which uses the C++ API of HDF5. Only HDF5-based NWB files are supported, not Zarr.

## What NeuroScope shows

- **Streams**: each `ElectricalSeries` of the file, wherever it is (acquisition, LFP in a
  processing module, ...). NeuroScope opens the one with the highest sampling rate among the acquired
  series and asks for the others, or takes `--stream-id <id>`. The id is the name of the series,
  e.g. `ElectricalSeries`, or `ecephys_LFP` if several series have the same name.
- **Time**: seconds on the clock of the file (`timestamps_reference_time`). A series with
  `starting_time` and `rate` starts at `starting_time`. A series with `timestamps` gets the median
  sampling period, and is placed in pieces wherever the timestamps jump; samples without data read
  as 0.
- **Values**: `data * channel_conversion * conversion + offset`, in µV.
- **Channels**: named by the `channel_name` or `label` column of the electrodes table, or by electrode
  id, and grouped by electrode group (`group_name`).
- **Events**: one list per `TimeIntervals` table (`trials`, `epochs`, ...) with the start and stop of
  each interval.

Not yet: `SpikeEventSeries`, units (spike times), `EventsTable`/`AnnotationSeries` events, files on
S3 or HTTP, and Klusters.

## Layout

| Path | Contents |
|------|----------|
| `src/nwbreader.{hpp,cpp}` | `nwb::Reader`: streams, samples and events of an NWB file, independent of NeuroScope |
| `src/neuroscope_plugin.cpp` | the C interface of NeuroScope plugins (`neuroscope_plugin.h`) on top of the reader |
| `tools/nwbinfo.cpp` | prints what the reader finds in NWB files |
| `tests/` | tests that load the plugin like NeuroScope; `tests/data/make_test_files.py` writes their files with pynwb |
| `ports/aqnwb/` | vcpkg overlay port of aqnwb, which is not in the vcpkg registry |

## Building

See [BUILD.md](BUILD.md).

## License

GPL-3.0-or-later, like NeuroScope. aqnwb is under a BSD license and HDF5 under the HDF5 license; both
are linked into the plugin.
