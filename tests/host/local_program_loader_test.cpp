#include "oracis/host/runtime_bridge.h"
#include "oracis/host/detail/nds_cartridge_header.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int fail(const char *message)
{
    std::cerr << "local_program_loader_test: " << message << '\n';
    return 1;
}

bool write_file(
    const std::filesystem::path &path,
    const std::vector<uint8_t> &bytes)
{
    std::ofstream stream(path, std::ios::binary);
    if (!stream) return false;
    stream.write(
        reinterpret_cast<const char *>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return stream.good();
}

} // namespace

int main()
{
    namespace fs = std::filesystem;
    constexpr uint32_t kArm9Size = 0x001023F8u;

    std::vector<uint8_t> boot_header(
        oracis::host::detail::NdsCartridgeHeaderStoredSize, 0u);
    boot_header[0x0Cu] = 'C';
    boot_header[0x0Du] = 'P';
    boot_header[0x0Eu] = 'U';
    boot_header[0x0Fu] = 'E';
    const auto write_u32 = [&boot_header](std::size_t offset, uint32_t value) {
        for (unsigned index = 0u; index < 4u; ++index) {
            boot_header[offset + index] =
                static_cast<uint8_t>(value >> (index * 8u));
        }
    };
    const auto write_u16 = [&boot_header](std::size_t offset, uint16_t value) {
        boot_header[offset] = static_cast<uint8_t>(value);
        boot_header[offset + 1u] = static_cast<uint8_t>(value >> 8u);
    };
    const auto refresh_crc = [&]() {
        write_u16(
            0x15Eu,
            oracis::host::detail::nds_header_crc16(boot_header.data(), 0x15Eu));
    };

    write_u32(0x20u, UINT32_C(0x00004000));
    write_u32(0x24u, UINT32_C(0x02000800));
    write_u32(0x28u, UINT32_C(0x02000000));
    write_u32(0x2Cu, UINT32_C(0x001023F8));
    write_u32(0x30u, UINT32_C(0x00409800));
    write_u32(0x34u, UINT32_C(0x02380000));
    write_u32(0x38u, UINT32_C(0x02380000));
    write_u32(0x3Cu, UINT32_C(0x000277FC));
    write_u16(0x15Cu, UINT16_C(0xCF56));
    refresh_crc();
    if (!oracis::host::detail::validate_platinum_usa_rev1_boot_header(
            boot_header.data(), boot_header.size())) {
        return fail("valid synthetic Platinum boot header was rejected");
    }

    write_u32(0x24u, UINT32_C(0x02000804));
    refresh_crc();
    if (oracis::host::detail::validate_platinum_usa_rev1_boot_header(
            boot_header.data(), boot_header.size())) {
        return fail("wrong entry-point metadata was accepted in the Platinum header");
    }
    write_u32(0x24u, UINT32_C(0x02000800));
    refresh_crc();
    if (oracis::host::detail::validate_platinum_usa_rev1_boot_header(
            boot_header.data(), boot_header.size() - 1u)) {
        return fail("truncated Platinum boot header was accepted");
    }

    const fs::path root =
        fs::temp_directory_path() / "oracis-local-program-loader-test";
    std::error_code error;
    fs::remove_all(root, error);
    error.clear();
    fs::create_directories(root, error);
    if (error) return fail("could not create temporary test directory");

    if (oracis_host_runtime_initialize() != ORACIS_HOST_RUNTIME_OK) {
        fs::remove_all(root, error);
        return fail("host runtime initialization failed");
    }

    int status = oracis_host_runtime_load_local_platinum_segments(
        root.string().c_str());
    if (status != ORACIS_HOST_RUNTIME_LOCAL_IMAGE_MISSING) {
        (void)oracis_host_runtime_shutdown();
        fs::remove_all(root, error);
        return fail("missing ARM9 image was not reported");
    }

    if (!write_file(root / "arm9.bin", {0x00u, 0x01u, 0x02u, 0x03u})) {
        (void)oracis_host_runtime_shutdown();
        fs::remove_all(root, error);
        return fail("could not write truncated ARM9 fixture");
    }

    status = oracis_host_runtime_load_local_platinum_segments(
        root.string().c_str());
    if (status != ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID) {
        (void)oracis_host_runtime_shutdown();
        fs::remove_all(root, error);
        return fail("truncated ARM9 image was not rejected");
    }

    std::vector<uint8_t> wrong_image(kArm9Size, 0u);
    if (!write_file(root / "arm9.bin", wrong_image)) {
        (void)oracis_host_runtime_shutdown();
        fs::remove_all(root, error);
        return fail("could not write wrong-hash ARM9 fixture");
    }

    status = oracis_host_runtime_load_local_platinum_segments(
        root.string().c_str());
    if (status != ORACIS_HOST_RUNTIME_LOCAL_IMAGE_INVALID) {
        (void)oracis_host_runtime_shutdown();
        fs::remove_all(root, error);
        return fail("wrong-hash ARM9 image was not rejected");
    }

    if (oracis_host_runtime_program_loaded() != 0 ||
        oracis_host_runtime_global_bus_cycles() != 0u) {
        (void)oracis_host_runtime_shutdown();
        fs::remove_all(root, error);
        return fail("invalid local image mutated machine execution state");
    }

    if (oracis_host_runtime_shutdown() != ORACIS_HOST_RUNTIME_OK) {
        fs::remove_all(root, error);
        return fail("host runtime shutdown failed");
    }
    fs::remove_all(root, error);

    std::cout << "local program loader rejects missing, truncated and "
                 "wrong-hash images without loading a program\n";
    return 0;
}
