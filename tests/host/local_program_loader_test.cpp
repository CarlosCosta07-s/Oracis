#include "oracis/host/runtime_bridge.h"

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
