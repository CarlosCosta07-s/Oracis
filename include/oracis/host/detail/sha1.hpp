#ifndef ORACIS_HOST_DETAIL_SHA1_HPP
#define ORACIS_HOST_DETAIL_SHA1_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace oracis::host::detail {

/* SHA-1 is used only to verify the fixed local game-segment identities. */
std::string sha1_hex(const std::vector<uint8_t> &bytes);

} // namespace oracis::host::detail

#endif /* ORACIS_HOST_DETAIL_SHA1_HPP */
