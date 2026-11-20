#include "oracis/memory/nds_post_flag.hpp"

#include <cstdint>
#include <cstdio>

namespace {

int fail(const char *message)
{
    std::fprintf(stderr, "nds_post_flag_test: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using oracis::memory::NdsPostFlag;
    using oracis::memory::Status;

    NdsPostFlag post_flag;
    if (post_flag.read(NDS_ARM9, NdsPostFlag::Address, 1u).status !=
            Status::Unmapped ||
        post_flag.install() != Status::Ok ||
        post_flag.install() != Status::InvalidRegion ||
        post_flag.value() != 0u) {
        return fail("POSTFLG installation/reset state was incorrect");
    }

    if (post_flag.read(NDS_ARM9, NdsPostFlag::Address, 1u).value != 0u ||
        post_flag.read(NDS_ARM7, NdsPostFlag::Address, 1u).value != 0u) {
        return fail("POSTFLG was not readable from both CPUs");
    }

    if (post_flag.write(NDS_ARM9, NdsPostFlag::Address, 1u, 3u) !=
            Status::Ok ||
        post_flag.value() != 3u ||
        post_flag.read(NDS_ARM9, NdsPostFlag::Address, 1u).value != 3u) {
        return fail("ARM9 POSTFLG byte write did not update implemented bits");
    }

    if (post_flag.write(NDS_ARM9, NdsPostFlag::Address, 1u, 0u) !=
            Status::Ok ||
        post_flag.value() != NdsPostFlag::PostBootMask) {
        return fail("POSTFLG post-boot bit was incorrectly cleared");
    }

    if (post_flag.write(NDS_ARM7, NdsPostFlag::Address, 1u, 3u) !=
            Status::PermissionDenied ||
        post_flag.value() != NdsPostFlag::PostBootMask) {
        return fail("ARM7 was allowed to write POSTFLG");
    }

    if (post_flag.write(NDS_ARM9, NdsPostFlag::Address, 2u, 3u) !=
            Status::InvalidWidth ||
        post_flag.read(NDS_ARM9, NdsPostFlag::Address, 4u).status !=
            Status::InvalidWidth ||
        post_flag.write(static_cast<NdsCpu>(7), NdsPostFlag::Address,
                        1u, 1u) != Status::InvalidCpu) {
        return fail("POSTFLG accepted an invalid width or CPU");
    }

    /* BIOS CustomPost passes a 32-bit R0 value, but only implemented bits persist. */
    if (post_flag.write_from_bios(NDS_ARM9, UINT32_C(0xFFFFFFFE)) !=
            Status::Ok ||
        post_flag.value() != 3u ||
        post_flag.write_from_bios(NDS_ARM9, 0u) != Status::Ok ||
        post_flag.value() != NdsPostFlag::PostBootMask ||
        post_flag.write_from_bios(NDS_ARM7, 3u) !=
            Status::PermissionDenied) {
        return fail("BIOS CustomPost semantics did not match POSTFLG bit rules");
    }

    std::puts("NDS POSTFLG access and ARM9 CustomPost semantics passed");
    return 0;
}
