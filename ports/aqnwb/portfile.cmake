# aqnwb is not in the vcpkg registry; this overlay port builds the release from GitHub.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO NeurodataWithoutBorders/aqnwb
    REF "${VERSION}"
    SHA512 679d132925ccc7122ed590fbc4e1e183c5181d74d36f2aae3aa337386038f0b83353edb33b1b5f31482bc80e047ce508f15136690cb93ea98480fe8688118c03
    HEAD_REF main
)

# Without remfile (remote files over HTTP), which aqnwb would download during configuration. The
# headers go to include/aqnwb, as they include each other as "Types.hpp", "io/BaseIO.hpp" etc.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DAQNWB_USE_REMFILE=OFF
        -DBUILD_TESTING=OFF
        -Daqnwb_DEVELOPER_MODE=OFF
        -DCMAKE_INSTALL_INCLUDEDIR=include/aqnwb
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/aqnwb)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
