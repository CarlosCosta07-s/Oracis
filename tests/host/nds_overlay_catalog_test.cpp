#include "oracis/host/detail/nds_overlay_catalog.hpp"
#include "oracis/host/detail/sha1.hpp"
#include "oracis/host/runtime_bridge.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using oracis::host::detail::NdsOverlayCatalog;
using oracis::host::detail::NdsOverlayCatalogStatus;
using oracis::host::detail::NdsOverlayCatalogRecord;

void require(bool condition)
{
    if (!condition) std::abort();
}

constexpr const char *kRomSha1 =
    "0862ec35b24de5c7e2dcb88c9eea0873110d755c";

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        const auto ticks =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("oracis-overlay-catalog-" + std::to_string(ticks));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path &path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void write_text(
    const std::filesystem::path &path,
    const std::string &contents)
{
    std::ofstream stream(path, std::ios::binary);
    require(stream.good());
    stream.write(
        contents.data(), static_cast<std::streamsize>(contents.size()));
    require(stream.good());
}

void write_bytes(
    const std::filesystem::path &path,
    const std::vector<uint8_t> &contents)
{
    std::ofstream stream(path, std::ios::binary);
    require(stream.good());
    stream.write(
        reinterpret_cast<const char *>(contents.data()),
        static_cast<std::streamsize>(contents.size()));
    require(stream.good());
}

std::string make_record(
    const char *cpu,
    uint32_t id,
    uint32_t address,
    const char *image_name,
    const std::vector<uint8_t> &bytes,
    uint32_t bss_size = 8u,
    uint32_t init_start = 0u,
    uint32_t init_end = 0u)
{
    return
        "{"
        "\"cpu\":\"" + std::string(cpu) + "\","
        "\"overlay_id\":" + std::to_string(id) + ","
        "\"ram_address\":" + std::to_string(address) + ","
        "\"ram_size\":" + std::to_string(bytes.size()) + ","
        "\"bss_size\":" + std::to_string(bss_size) + ","
        "\"static_init_start\":" + std::to_string(init_start) + ","
        "\"static_init_end\":" + std::to_string(init_end) + ","
        "\"file_id\":0,"
        "\"table_compressed_size\":0,"
        "\"flags\":0,"
        "\"compressed\":false,"
        "\"verify_hash\":false,"
        "\"source_offset\":512,"
        "\"source_size\":" + std::to_string(bytes.size()) + ","
        "\"source_sha1\":\"" +
            oracis::host::detail::sha1_hex(bytes) + "\","
        "\"image_file\":\"" + image_name + "\","
        "\"image_size\":" + std::to_string(bytes.size()) + ","
        "\"image_sha1\":\"" +
            oracis::host::detail::sha1_hex(bytes) + "\","
        "\"image_sha256\":\"" + std::string(64u, 'a') + "\""
        "}";
}

std::string make_manifest(
    const std::string &arm9_record,
    const std::string &arm7_record,
    const std::string &rom_sha1 = kRomSha1,
    uint32_t declared_count = 2u)
{
    return
        "{\n"
        "\"schema\":\"oracis-nds-overlays-v1\","
        "\"rom_sha1\":\"" + rom_sha1 + "\","
        "\"rom_size\":123456,"
        "\"source_file_name\":\"user-copy.nds\","
        "\"overlay_count\":" + std::to_string(declared_count) + ","
        "\"overlays\":[" + arm9_record + "," + arm7_record + "],"
        "\"generated_files\":["
        "\"arm9_overlay_0003.bin\","
        "\"arm7_overlay_0001.bin\""
        "],"
        "\"notice\":\"metadata fixture only\""
        "}\n";
}

void create_valid_fixture(
    const std::filesystem::path &directory,
    std::string *manifest_output = nullptr)
{
    const std::vector<uint8_t> arm9{0x11u, 0x22u, 0x33u, 0x44u};
    const std::vector<uint8_t> arm7{0x55u, 0x66u, 0x77u, 0x88u};
    write_bytes(directory / "arm9_overlay_0003.bin", arm9);
    write_bytes(directory / "arm7_overlay_0001.bin", arm7);

    const std::string arm9_record = make_record(
        "arm9", 3u, UINT32_C(0x02200000),
        "arm9_overlay_0003.bin", arm9);
    const std::string arm7_record = make_record(
        "arm7", 1u, UINT32_C(0x02380000),
        "arm7_overlay_0001.bin", arm7);
    const std::string manifest = make_manifest(arm9_record, arm7_record);
    write_text(directory / "manifest.json", manifest);
    if (manifest_output != nullptr) *manifest_output = manifest;
}

void test_opens_catalog_and_reads_verified_images()
{
    TemporaryDirectory temporary;
    create_valid_fixture(temporary.path());

    NdsOverlayCatalog catalog;
    std::string diagnostic;
    require(NdsOverlayCatalog::open(
        temporary.path(), kRomSha1, catalog, &diagnostic) ==
        NdsOverlayCatalogStatus::Ok);
    require(diagnostic == "ok");
    require(catalog.records().size() == 2u);

    const auto *arm9 = catalog.find(NDS_ARM9, 3u);
    const auto *arm7 = catalog.find(NDS_ARM7, 1u);
    require(arm9 != nullptr && arm7 != nullptr);
    require(arm9->ram_address == UINT32_C(0x02200000));
    require(arm9->bss_size == 8u);
    require(catalog.find(NDS_ARM9, 1u) == nullptr);

    std::vector<uint8_t> contents;
    require(catalog.read_image(*arm9, contents, &diagnostic) ==
        NdsOverlayCatalogStatus::Ok);
    require((contents == std::vector<uint8_t>{
        0x11u, 0x22u, 0x33u, 0x44u
    }));

    require(catalog.read_image(*arm7, contents, &diagnostic) ==
        NdsOverlayCatalogStatus::Ok);
    require((contents == std::vector<uint8_t>{
        0x55u, 0x66u, 0x77u, 0x88u
    }));
}

void test_rejects_rom_identity_mismatch_without_replacing_catalog()
{
    TemporaryDirectory temporary;
    create_valid_fixture(temporary.path());

    NdsOverlayCatalog catalog;
    require(NdsOverlayCatalog::open(
        temporary.path(), kRomSha1, catalog) ==
        NdsOverlayCatalogStatus::Ok);
    require(catalog.records().size() == 2u);

    require(NdsOverlayCatalog::open(
        temporary.path(), std::string(40u, '0'), catalog) ==
        NdsOverlayCatalogStatus::RomIdentityMismatch);
    require(catalog.records().size() == 2u);
}

void test_rejects_manifest_count_mismatch()
{
    TemporaryDirectory temporary;
    const std::vector<uint8_t> arm9{1u, 2u, 3u, 4u};
    const std::vector<uint8_t> arm7{5u, 6u, 7u, 8u};
    write_bytes(temporary.path() / "arm9_overlay_0003.bin", arm9);
    write_bytes(temporary.path() / "arm7_overlay_0001.bin", arm7);
    const auto first = make_record(
        "arm9", 3u, UINT32_C(0x02200000),
        "arm9_overlay_0003.bin", arm9);
    const auto second = make_record(
        "arm7", 1u, UINT32_C(0x02380000),
        "arm7_overlay_0001.bin", arm7);
    write_text(
        temporary.path() / "manifest.json",
        make_manifest(first, second, kRomSha1, 3u));

    NdsOverlayCatalog catalog;
    require(NdsOverlayCatalog::open(
        temporary.path(), kRomSha1, catalog) ==
        NdsOverlayCatalogStatus::InvalidManifest);
}

void test_rejects_duplicate_cpu_overlay_identity()
{
    TemporaryDirectory temporary;
    const std::vector<uint8_t> image{1u, 2u, 3u, 4u};
    write_bytes(temporary.path() / "arm9_overlay_0003.bin", image);
    write_bytes(temporary.path() / "arm7_overlay_0001.bin", image);
    const auto first = make_record(
        "arm9", 3u, UINT32_C(0x02200000),
        "arm9_overlay_0003.bin", image);
    const auto duplicate = make_record(
        "arm9", 3u, UINT32_C(0x02210000),
        "arm9_overlay_0003.bin", image);
    write_text(
        temporary.path() / "manifest.json",
        make_manifest(first, duplicate));

    NdsOverlayCatalog catalog;
    require(NdsOverlayCatalog::open(
        temporary.path(), kRomSha1, catalog) ==
        NdsOverlayCatalogStatus::InvalidManifest);
}

void test_rejects_path_traversal_image_name()
{
    TemporaryDirectory temporary;
    const std::vector<uint8_t> arm9{1u, 2u, 3u, 4u};
    const std::vector<uint8_t> arm7{5u, 6u, 7u, 8u};
    const auto first = make_record(
        "arm9", 3u, UINT32_C(0x02200000),
        "../arm9_overlay_0003.bin", arm9);
    const auto second = make_record(
        "arm7", 1u, UINT32_C(0x02380000),
        "arm7_overlay_0001.bin", arm7);
    write_text(
        temporary.path() / "manifest.json",
        make_manifest(first, second));

    NdsOverlayCatalog catalog;
    require(NdsOverlayCatalog::open(
        temporary.path(), kRomSha1, catalog) ==
        NdsOverlayCatalogStatus::InvalidManifest);
}

void test_detects_image_hash_tampering_after_catalog_open()
{
    TemporaryDirectory temporary;
    create_valid_fixture(temporary.path());

    NdsOverlayCatalog catalog;
    require(NdsOverlayCatalog::open(
        temporary.path(), kRomSha1, catalog) ==
        NdsOverlayCatalogStatus::Ok);
    const auto *arm9 = catalog.find(NDS_ARM9, 3u);
    require(arm9 != nullptr);

    write_bytes(
        temporary.path() / "arm9_overlay_0003.bin",
        std::vector<uint8_t>{0xFFu, 0xEEu, 0xDDu, 0xCCu});

    std::vector<uint8_t> output{0xAAu};
    require(catalog.read_image(*arm9, output) ==
        NdsOverlayCatalogStatus::ImageHashMismatch);
    require((output == std::vector<uint8_t>{0xAAu}));
}

void test_rejects_malformed_manifest_and_invalid_static_range()
{
    TemporaryDirectory malformed;
    write_text(malformed.path() / "manifest.json", "{\"schema\":");
    NdsOverlayCatalog catalog;
    require(NdsOverlayCatalog::open(
        malformed.path(), kRomSha1, catalog) ==
        NdsOverlayCatalogStatus::InvalidManifest);

    TemporaryDirectory invalid_range;
    const std::vector<uint8_t> arm9{1u, 2u, 3u, 4u};
    const std::vector<uint8_t> arm7{5u, 6u, 7u, 8u};
    const auto first = make_record(
        "arm9", 3u, UINT32_C(0x02200000),
        "arm9_overlay_0003.bin", arm9, 0u,
        UINT32_C(0x02200004), UINT32_C(0x02200010));
    const auto second = make_record(
        "arm7", 1u, UINT32_C(0x02380000),
        "arm7_overlay_0001.bin", arm7);
    write_text(
        invalid_range.path() / "manifest.json",
        make_manifest(first, second));
    require(NdsOverlayCatalog::open(
        invalid_range.path(), kRomSha1, catalog) ==
        NdsOverlayCatalogStatus::InvalidManifest);
}

void test_missing_directory_is_reported_explicitly()
{
    TemporaryDirectory temporary;
    NdsOverlayCatalog catalog;
    require(NdsOverlayCatalog::open(
        temporary.path() / "not-created", kRomSha1, catalog) ==
        NdsOverlayCatalogStatus::Missing);
}

void test_host_bridge_loads_catalog_overlay_into_guest_memory()
{
    TemporaryDirectory temporary;
    create_valid_fixture(temporary.path());

    require(oracis_host_runtime_initialize() == ORACIS_HOST_RUNTIME_OK);

    const uint8_t arm9_code[] = {
        0x00u, 0x00u, 0xA0u, 0xE1u
    };
    const uint8_t arm7_code[] = {
        0x70u, 0x47u
    };
    const OracisHostProgramSegment arm9{
        UINT32_C(0x02001000),
        static_cast<uint32_t>(sizeof(arm9_code)),
        UINT32_C(0x02001000),
        0u,
        arm9_code
    };
    const OracisHostProgramSegment arm7{
        UINT32_C(0x02381000),
        static_cast<uint32_t>(sizeof(arm7_code)),
        UINT32_C(0x02381000),
        1u,
        arm7_code
    };
    require(oracis_host_runtime_load_program_segments(
        &arm9, &arm7) == ORACIS_HOST_RUNTIME_OK);

    /* The host bridge must not let a host-boundary call mutate the guest
       global destructor chain; registration is valid only in the owning
       CPU's active guest slice. */
    require(oracis_host_runtime_register_global_destructor(
        static_cast<uint32_t>(NDS_ARM9),
        UINT32_C(0x02001000),
        UINT32_C(0x02001020),
        UINT32_C(0x02001031),
        UINT32_C(0x02001040)) ==
        ORACIS_HOST_RUNTIME_OVERLAY_BUSY);
    require(oracis_host_runtime_register_active_global_destructor(
        UINT32_C(0x02001000)) ==
        ORACIS_HOST_RUNTIME_OVERLAY_BUSY);

    const std::string directory = temporary.path().string();
    require(oracis_host_runtime_load_local_overlay(
        directory.c_str(), static_cast<uint32_t>(NDS_ARM9), 3u) ==
        ORACIS_HOST_RUNTIME_OK);
    require(oracis_host_runtime_load_local_overlay(
        directory.c_str(), static_cast<uint32_t>(NDS_ARM9), 3u) ==
        ORACIS_HOST_RUNTIME_OVERLAY_ALREADY_LOADED);
    require(oracis_host_runtime_load_local_overlay(
        directory.c_str(), static_cast<uint32_t>(NDS_ARM9), 99u) ==
        ORACIS_HOST_RUNTIME_OVERLAY_NOT_LOADED);

    require(oracis_host_runtime_request_local_overlay(
        directory.c_str(), static_cast<uint32_t>(NDS_ARM7), 1u) ==
        ORACIS_HOST_RUNTIME_OK);
    require(oracis_host_runtime_request_local_overlay(
        directory.c_str(), static_cast<uint32_t>(NDS_ARM7), 1u) ==
        ORACIS_HOST_RUNTIME_OVERLAY_ALREADY_LOADED);

    require(oracis_host_runtime_shutdown() == ORACIS_HOST_RUNTIME_OK);
}

} // namespace

int main()
{
    test_opens_catalog_and_reads_verified_images();
    test_rejects_rom_identity_mismatch_without_replacing_catalog();
    test_rejects_manifest_count_mismatch();
    test_rejects_duplicate_cpu_overlay_identity();
    test_rejects_path_traversal_image_name();
    test_detects_image_hash_tampering_after_catalog_open();
    test_rejects_malformed_manifest_and_invalid_static_range();
    test_missing_directory_is_reported_explicitly();
    test_host_bridge_loads_catalog_overlay_into_guest_memory();
    return 0;
}
