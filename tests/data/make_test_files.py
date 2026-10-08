"""Writes the small NWB files the tests read, with pynwb, the reference implementation of NWB.

    uv run --with pynwb==3.1.2 python tests/data/make_test_files.py tests/data

The values are chosen so that the tests can compute what the reader must return; keep the two in
sync when changing this script.
"""

import sys
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
from pynwb import NWBHDF5IO, NWBFile
from pynwb.ecephys import LFP, ElectricalSeries


def new_file(identifier):
    return NWBFile(
        session_description="neurosuite-nwb test file",
        identifier=identifier,
        session_start_time=datetime(2026, 10, 8, 12, 0, tzinfo=timezone.utc),
    )


def add_electrodes(nwbfile, groups):
    """Adds electrodes in the given groups, e.g. ["shank0", "shank0", "shank1"], named e0, e1, ..."""
    device = nwbfile.create_device(name="probe")
    electrode_groups = {}
    nwbfile.add_electrode_column(name="channel_name", description="name of the channel")
    for index, group in enumerate(groups):
        if group not in electrode_groups:
            electrode_groups[group] = nwbfile.create_electrode_group(
                name=group, description=group, location="CA1", device=device
            )
        nwbfile.add_electrode(group=electrode_groups[group], location="CA1", channel_name=f"e{index}")


def write(nwbfile, path):
    with NWBHDF5IO(str(path), "w") as io:
        io.write(nwbfile)


def rate_file(path):
    """Raw data with starting_time and rate, LFP in a processing module, trials and epochs.

    raw: 3 channels (shank0: e0, e1; shank1: e2) at 1 kHz from 0.5 s on, 20 samples, int16
         value[i, c] = 100 * c + i, conversion 2e-6, offset 1e-6, channel_conversion [1, 2, 0.5]
    LFP: channels e2, e0 at 100 Hz from 0 s on, 10 samples, float32 volts, value[i, c] = (i + c) * 1e-6
    """
    nwbfile = new_file("rate")
    add_electrodes(nwbfile, ["shank0", "shank0", "shank1"])

    data = np.array([[100 * c + i for c in range(3)] for i in range(20)], dtype=np.int16)
    nwbfile.add_acquisition(
        ElectricalSeries(
            name="raw",
            data=data,
            electrodes=nwbfile.create_electrode_table_region([0, 1, 2], "all electrodes"),
            starting_time=0.5,
            rate=1000.0,
            conversion=2e-6,
            offset=1e-6,
            channel_conversion=[1.0, 2.0, 0.5],
        )
    )

    lfp_data = np.array([[(i + c) * 1e-6 for c in range(2)] for i in range(10)], dtype=np.float32)
    lfp = LFP(
        ElectricalSeries(
            name="lfp",
            data=lfp_data,
            electrodes=nwbfile.create_electrode_table_region([2, 0], "e2 and e0"),
            starting_time=0.0,
            rate=100.0,
        )
    )
    nwbfile.create_processing_module(name="ecephys", description="processed").add(lfp)

    nwbfile.add_trial(start_time=0.5, stop_time=0.6)
    nwbfile.add_trial(start_time=0.2, stop_time=0.3)
    nwbfile.add_epoch(start_time=0.0, stop_time=1.0, tags=["all"])
    write(nwbfile, path)


def timestamps_file(path):
    """Data with timestamps instead of a rate, and two series of the same name.

    gaps: 1 channel at 1 kHz, timestamps 0.010-0.014 s and 0.020-0.022 s, uint8, value = index + 1
    backwards: 1 channel at 1 kHz, timestamps 0.005-0.006 s, then 0.001-0.002 s, value = index + 1
    m1/same, m2/same: 1 channel at 10 Hz from 0 s on, 2 samples each (ids must differ)
    """
    nwbfile = new_file("timestamps")
    add_electrodes(nwbfile, ["shank0"])
    electrode = lambda: nwbfile.create_electrode_table_region([0], "e0")

    gap_times = [0.010, 0.011, 0.012, 0.013, 0.014, 0.020, 0.021, 0.022]
    nwbfile.add_acquisition(
        ElectricalSeries(
            name="gaps",
            data=np.arange(1, len(gap_times) + 1, dtype=np.uint8),
            electrodes=electrode(),
            timestamps=gap_times,
            conversion=1e-6,
        )
    )
    backwards_times = [0.005, 0.006, 0.001, 0.002]
    nwbfile.add_acquisition(
        ElectricalSeries(
            name="backwards",
            data=np.arange(1, len(backwards_times) + 1, dtype=np.float64),
            electrodes=electrode(),
            timestamps=backwards_times,
            conversion=1e-6,
        )
    )
    for module in ("m1", "m2"):
        nwbfile.create_processing_module(name=module, description=module).add(
            ElectricalSeries(name="same", data=np.zeros(2), electrodes=electrode(), starting_time=0.0, rate=10.0)
        )
    write(nwbfile, path)


if __name__ == "__main__":
    directory = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    rate_file(directory / "rate.nwb")
    timestamps_file(directory / "timestamps.nwb")
