#include "oracis/host/detail/nds_overlay_catalog.hpp"

#include "oracis/host/detail/sha1.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

namespace oracis::host::detail {
namespace {

constexpr std::string_view ExpectedSchema = "oracis-nds-overlays-v1";

void set_diagnostic(std::string *output, const char *message) noexcept
{
    if (output == nullptr) return;
    try {
        *output = message;
    } catch (...) {
        output->clear();
    }
}

bool is_hex_string(std::string_view value, std::size_t length) noexcept
{
    if (value.size() != length) return false;
    for (const char character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::string expected_image_name(NdsCpu cpu, uint32_t overlay_id)
{
    const char *prefix = cpu == NDS_ARM9 ? "arm9" : "arm7";
    std::string name(prefix);
    name += "_overlay_";

    char digits[16]{};
    unsigned value = overlay_id;
    unsigned count = 0u;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u && count < sizeof(digits));

    while (count < 4u) digits[count++] = '0';
    while (count != 0u) name.push_back(digits[--count]);
    name += ".bin";
    return name;
}

bool valid_record(const NdsOverlayCatalogRecord &record)
{
    if ((record.cpu != NDS_ARM9 && record.cpu != NDS_ARM7) ||
        record.ram_size == 0u ||
        record.image_size != record.ram_size ||
        record.image_size > NdsOverlayCatalog::MaxOverlayImageBytes ||
        !is_hex_string(record.image_sha1, 40u)) {
        return false;
    }

    const uint64_t code_end =
        static_cast<uint64_t>(record.ram_address) + record.ram_size;
    const uint64_t total_end =
        code_end + static_cast<uint64_t>(record.bss_size);
    if (code_end > UINT64_C(0x1'0000'0000) ||
        total_end > UINT64_C(0x1'0000'0000)) {
        return false;
    }

    const bool no_static_init =
        record.static_init_start == 0u && record.static_init_end == 0u;
    if (!no_static_init &&
        (record.static_init_start == 0u ||
         record.static_init_end == 0u ||
         record.static_init_start < record.ram_address ||
         record.static_init_end < record.static_init_start ||
         static_cast<uint64_t>(record.static_init_end) > code_end ||
         (record.static_init_start & 3u) != 0u ||
         (record.static_init_end & 3u) != 0u ||
         ((record.static_init_end - record.static_init_start) & 3u) != 0u)) {
        return false;
    }

    return record.image_file ==
        expected_image_name(record.cpu, record.overlay_id);
}

/*
 * Small strict JSON reader for the extractor's metadata-only manifest.
 * Unknown properties are skipped recursively, so newer extractor versions
 * can add fields without changing the runtime contract.
 */
class JsonReader final {
public:
    explicit JsonReader(std::string_view input) noexcept : input_(input) {}

    bool parse_manifest(
        std::string &schema,
        std::string &rom_sha1,
        uint32_t &overlay_count,
        std::vector<NdsOverlayCatalogRecord> &records) noexcept
    {
        try {
            skip_whitespace();
            if (!consume('{')) return false;

            bool schema_seen = false;
            bool rom_sha1_seen = false;
            bool overlay_count_seen = false;
            bool overlays_seen = false;

            skip_whitespace();
            if (consume('}')) return false;

            for (;;) {
                std::string key;
                if (!parse_string(key)) return false;
                skip_whitespace();
                if (!consume(':')) return false;
                skip_whitespace();

                if (key == "schema") {
                    if (schema_seen || !parse_string(schema)) return false;
                    schema_seen = true;
                } else if (key == "rom_sha1") {
                    if (rom_sha1_seen || !parse_string(rom_sha1)) return false;
                    rom_sha1_seen = true;
                } else if (key == "overlay_count") {
                    if (overlay_count_seen ||
                        !parse_uint32(overlay_count)) {
                        return false;
                    }
                    overlay_count_seen = true;
                } else if (key == "overlays") {
                    if (overlays_seen || !parse_overlays(records)) return false;
                    overlays_seen = true;
                } else if (!skip_value(0u)) {
                    return false;
                }

                skip_whitespace();
                if (consume('}')) break;
                if (!consume(',')) return false;
                skip_whitespace();
            }

            skip_whitespace();
            return at_end() && schema_seen && rom_sha1_seen &&
                overlay_count_seen && overlays_seen &&
                schema == ExpectedSchema &&
                records.size() == static_cast<std::size_t>(overlay_count);
        } catch (...) {
            return false;
        }
    }

private:
    bool at_end() const noexcept { return position_ >= input_.size(); }

    void skip_whitespace() noexcept
    {
        while (!at_end()) {
            const char value = input_[position_];
            if (value != ' ' && value != '\t' &&
                value != '\r' && value != '\n') {
                break;
            }
            ++position_;
        }
    }

    bool consume(char expected) noexcept
    {
        if (at_end() || input_[position_] != expected) return false;
        ++position_;
        return true;
    }

    static int hex_value(char value) noexcept
    {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    }

    static void append_utf8(std::string &output, uint32_t codepoint)
    {
        if (codepoint <= 0x7Fu) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFu) {
            output.push_back(static_cast<char>(0xC0u | (codepoint >> 6u)));
            output.push_back(static_cast<char>(
                0x80u | (codepoint & UINT32_C(0x3F))));
        } else if (codepoint <= 0xFFFFu) {
            output.push_back(static_cast<char>(0xE0u | (codepoint >> 12u)));
            output.push_back(static_cast<char>(
                0x80u | ((codepoint >> 6u) & UINT32_C(0x3F))));
            output.push_back(static_cast<char>(
                0x80u | (codepoint & UINT32_C(0x3F))));
        } else {
            output.push_back(static_cast<char>(0xF0u | (codepoint >> 18u)));
            output.push_back(static_cast<char>(
                0x80u | ((codepoint >> 12u) & UINT32_C(0x3F))));
            output.push_back(static_cast<char>(
                0x80u | ((codepoint >> 6u) & UINT32_C(0x3F))));
            output.push_back(static_cast<char>(
                0x80u | (codepoint & UINT32_C(0x3F))));
        }
    }

    bool parse_string(std::string &output)
    {
        if (!consume('"')) return false;
        output.clear();

        while (!at_end()) {
            const unsigned char value =
                static_cast<unsigned char>(input_[position_++]);
            if (value == static_cast<unsigned char>('"')) return true;
            if (value < 0x20u) return false;

            if (value != static_cast<unsigned char>('\\')) {
                output.push_back(static_cast<char>(value));
                continue;
            }

            if (at_end()) return false;
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                if (input_.size() - position_ < 4u) return false;
                uint32_t codepoint = 0u;
                for (unsigned index = 0u; index < 4u; ++index) {
                    const int digit = hex_value(input_[position_++]);
                    if (digit < 0) return false;
                    codepoint = (codepoint << 4u) |
                        static_cast<uint32_t>(digit);
                }
                /* Surrogate pairs are not needed for the ASCII manifest keys
                   and filenames; reject lone surrogates rather than mangle. */
                if (codepoint >= 0xD800u && codepoint <= 0xDFFFu) {
                    return false;
                }
                append_utf8(output, codepoint);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool parse_uint32(uint32_t &output) noexcept
    {
        if (at_end() || input_[position_] < '0' ||
            input_[position_] > '9') {
            return false;
        }
        if (input_[position_] == '0') {
            ++position_;
            if (!at_end() && input_[position_] >= '0' &&
                input_[position_] <= '9') {
                return false;
            }
            output = 0u;
            return true;
        }

        uint64_t value = 0u;
        while (!at_end() && input_[position_] >= '0' &&
               input_[position_] <= '9') {
            value = value * 10u +
                static_cast<uint64_t>(input_[position_] - '0');
            if (value > UINT32_MAX) return false;
            ++position_;
        }
        output = static_cast<uint32_t>(value);
        return true;
    }

    bool skip_number() noexcept
    {
        if (consume('-')) {
            if (at_end()) return false;
        }
        if (at_end()) return false;
        if (input_[position_] == '0') {
            ++position_;
        } else {
            if (input_[position_] < '1' || input_[position_] > '9') {
                return false;
            }
            while (!at_end() && input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
        }
        if (consume('.')) {
            if (at_end() || input_[position_] < '0' ||
                input_[position_] > '9') return false;
            while (!at_end() && input_[position_] >= '0' &&
                   input_[position_] <= '9') ++position_;
        }
        if (!at_end() && (input_[position_] == 'e' ||
                          input_[position_] == 'E')) {
            ++position_;
            if (!at_end() && (input_[position_] == '+' ||
                              input_[position_] == '-')) ++position_;
            if (at_end() || input_[position_] < '0' ||
                input_[position_] > '9') return false;
            while (!at_end() && input_[position_] >= '0' &&
                   input_[position_] <= '9') ++position_;
        }
        return true;
    }

    bool skip_value(unsigned depth)
    {
        if (depth > 32u) return false;
        skip_whitespace();
        if (at_end()) return false;

        if (input_[position_] == '"') {
            std::string ignored;
            return parse_string(ignored);
        }
        if (input_[position_] == '{') {
            ++position_;
            skip_whitespace();
            if (consume('}')) return true;
            for (;;) {
                std::string key;
                if (!parse_string(key)) return false;
                skip_whitespace();
                if (!consume(':') || !skip_value(depth + 1u)) return false;
                skip_whitespace();
                if (consume('}')) return true;
                if (!consume(',')) return false;
                skip_whitespace();
            }
        }
        if (input_[position_] == '[') {
            ++position_;
            skip_whitespace();
            if (consume(']')) return true;
            for (;;) {
                if (!skip_value(depth + 1u)) return false;
                skip_whitespace();
                if (consume(']')) return true;
                if (!consume(',')) return false;
            }
        }
        if (input_.substr(position_, 4u) == "true") {
            position_ += 4u;
            return true;
        }
        if (input_.substr(position_, 5u) == "false") {
            position_ += 5u;
            return true;
        }
        if (input_.substr(position_, 4u) == "null") {
            position_ += 4u;
            return true;
        }
        return skip_number();
    }

    bool parse_overlays(
        std::vector<NdsOverlayCatalogRecord> &records)
    {
        if (!consume('[')) return false;
        skip_whitespace();
        if (consume(']')) return true;

        for (;;) {
            if (records.size() >= NdsOverlayCatalog::MaxOverlayRecords) {
                return false;
            }
            NdsOverlayCatalogRecord record;
            if (!parse_overlay_record(record) || !valid_record(record)) {
                return false;
            }
            for (const auto &existing : records) {
                if (existing.cpu == record.cpu &&
                    existing.overlay_id == record.overlay_id) {
                    return false;
                }
            }
            records.push_back(std::move(record));

            skip_whitespace();
            if (consume(']')) return true;
            if (!consume(',')) return false;
            skip_whitespace();
        }
    }

    bool parse_overlay_record(NdsOverlayCatalogRecord &record)
    {
        if (!consume('{')) return false;

        enum Field : uint32_t {
            Cpu = 1u << 0u,
            Id = 1u << 1u,
            RamAddress = 1u << 2u,
            RamSize = 1u << 3u,
            BssSize = 1u << 4u,
            InitStart = 1u << 5u,
            InitEnd = 1u << 6u,
            ImageFile = 1u << 7u,
            ImageSize = 1u << 8u,
            ImageSha1 = 1u << 9u
        };
        constexpr uint32_t Required =
            Cpu | Id | RamAddress | RamSize | BssSize | InitStart |
            InitEnd | ImageFile | ImageSize | ImageSha1;

        uint32_t seen = 0u;
        skip_whitespace();
        if (consume('}')) return false;

        for (;;) {
            std::string key;
            if (!parse_string(key)) return false;
            skip_whitespace();
            if (!consume(':')) return false;
            skip_whitespace();

            uint32_t field = 0u;
            if (key == "cpu") field = Cpu;
            else if (key == "overlay_id") field = Id;
            else if (key == "ram_address") field = RamAddress;
            else if (key == "ram_size") field = RamSize;
            else if (key == "bss_size") field = BssSize;
            else if (key == "static_init_start") field = InitStart;
            else if (key == "static_init_end") field = InitEnd;
            else if (key == "image_file") field = ImageFile;
            else if (key == "image_size") field = ImageSize;
            else if (key == "image_sha1") field = ImageSha1;

            if (field == 0u) {
                if (!skip_value(0u)) return false;
            } else {
                if ((seen & field) != 0u) return false;
                seen |= field;

                if (field == Cpu) {
                    std::string cpu;
                    if (!parse_string(cpu)) return false;
                    if (cpu == "arm9") record.cpu = NDS_ARM9;
                    else if (cpu == "arm7") record.cpu = NDS_ARM7;
                    else return false;
                } else if (field == ImageFile || field == ImageSha1) {
                    std::string value;
                    if (!parse_string(value)) return false;
                    if (field == ImageFile) record.image_file = std::move(value);
                    else record.image_sha1 = std::move(value);
                } else {
                    uint32_t value = 0u;
                    if (!parse_uint32(value)) return false;
                    switch (field) {
                    case Id: record.overlay_id = value; break;
                    case RamAddress: record.ram_address = value; break;
                    case RamSize: record.ram_size = value; break;
                    case BssSize: record.bss_size = value; break;
                    case InitStart: record.static_init_start = value; break;
                    case InitEnd: record.static_init_end = value; break;
                    case ImageSize: record.image_size = value; break;
                    default: return false;
                    }
                }
            }

            skip_whitespace();
            if (consume('}')) break;
            if (!consume(',')) return false;
            skip_whitespace();
        }

        return seen == Required;
    }

    std::string_view input_;
    std::size_t position_ = 0u;
};

NdsOverlayCatalogStatus read_manifest(
    const std::filesystem::path &path,
    std::string &contents,
    std::string *diagnostic)
{
    std::error_code error;
    const auto file_status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        file_status.type() == std::filesystem::file_type::not_found) {
        set_diagnostic(diagnostic, "overlay manifest is missing");
        return NdsOverlayCatalogStatus::Missing;
    }
    if (error) {
        set_diagnostic(diagnostic, "could not inspect overlay manifest");
        return NdsOverlayCatalogStatus::IoError;
    }
    if (std::filesystem::is_symlink(file_status) ||
        !std::filesystem::is_regular_file(file_status)) {
        set_diagnostic(diagnostic, "overlay manifest is not a regular file");
        return NdsOverlayCatalogStatus::InvalidManifest;
    }

    const uintmax_t file_size = std::filesystem::file_size(path, error);
    if (error) {
        set_diagnostic(diagnostic, "could not read overlay manifest size");
        return NdsOverlayCatalogStatus::IoError;
    }
    if (file_size == 0u ||
        file_size > NdsOverlayCatalog::MaxManifestBytes) {
        set_diagnostic(diagnostic, "overlay manifest size is outside the accepted range");
        return NdsOverlayCatalogStatus::InvalidManifest;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        set_diagnostic(diagnostic, "could not open overlay manifest");
        return NdsOverlayCatalogStatus::IoError;
    }

    contents.resize(static_cast<std::size_t>(file_size));
    stream.read(
        contents.data(),
        static_cast<std::streamsize>(contents.size()));
    if (stream.gcount() != static_cast<std::streamsize>(contents.size())) {
        set_diagnostic(diagnostic, "could not read complete overlay manifest");
        return stream.bad()
            ? NdsOverlayCatalogStatus::IoError
            : NdsOverlayCatalogStatus::InvalidManifest;
    }
    if (stream.bad()) {
        set_diagnostic(diagnostic, "I/O error while reading overlay manifest");
        return NdsOverlayCatalogStatus::IoError;
    }

    char extra = 0;
    stream.read(&extra, 1);
    if (stream.gcount() != 0) {
        set_diagnostic(diagnostic, "overlay manifest changed while being read");
        return NdsOverlayCatalogStatus::InvalidManifest;
    }
    if (stream.bad()) {
        set_diagnostic(diagnostic, "I/O error after reading overlay manifest");
        return NdsOverlayCatalogStatus::IoError;
    }
    return NdsOverlayCatalogStatus::Ok;
}

NdsOverlayCatalogStatus read_overlay_file(
    const std::filesystem::path &path,
    const NdsOverlayCatalogRecord &record,
    std::vector<uint8_t> &contents,
    std::string *diagnostic)
{
    std::error_code error;
    const auto file_status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        file_status.type() == std::filesystem::file_type::not_found) {
        set_diagnostic(diagnostic, "overlay image is missing");
        return NdsOverlayCatalogStatus::ImageMissing;
    }
    if (error) {
        set_diagnostic(diagnostic, "could not inspect overlay image");
        return NdsOverlayCatalogStatus::IoError;
    }
    if (std::filesystem::is_symlink(file_status) ||
        !std::filesystem::is_regular_file(file_status)) {
        set_diagnostic(diagnostic, "overlay image is not a regular file");
        return NdsOverlayCatalogStatus::ImageInvalid;
    }

    const uintmax_t file_size = std::filesystem::file_size(path, error);
    if (error) {
        set_diagnostic(diagnostic, "could not read overlay image size");
        return NdsOverlayCatalogStatus::IoError;
    }
    if (file_size != record.image_size) {
        set_diagnostic(diagnostic, "overlay image size does not match its manifest record");
        return NdsOverlayCatalogStatus::ImageInvalid;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        set_diagnostic(diagnostic, "could not open overlay image");
        return NdsOverlayCatalogStatus::IoError;
    }

    std::vector<uint8_t> staged(record.image_size);
    stream.read(
        reinterpret_cast<char *>(staged.data()),
        static_cast<std::streamsize>(staged.size()));
    if (stream.gcount() != static_cast<std::streamsize>(staged.size())) {
        set_diagnostic(diagnostic, "could not read complete overlay image");
        return stream.bad()
            ? NdsOverlayCatalogStatus::IoError
            : NdsOverlayCatalogStatus::ImageInvalid;
    }
    if (stream.bad()) {
        set_diagnostic(diagnostic, "I/O error while reading overlay image");
        return NdsOverlayCatalogStatus::IoError;
    }

    char extra = 0;
    stream.read(&extra, 1);
    if (stream.gcount() != 0) {
        set_diagnostic(diagnostic, "overlay image changed while being read");
        return NdsOverlayCatalogStatus::ImageInvalid;
    }
    if (stream.bad()) {
        set_diagnostic(diagnostic, "I/O error after reading overlay image");
        return NdsOverlayCatalogStatus::IoError;
    }

    if (sha1_hex(staged) != record.image_sha1) {
        set_diagnostic(diagnostic, "overlay image SHA-1 does not match its manifest record");
        return NdsOverlayCatalogStatus::ImageHashMismatch;
    }

    contents = std::move(staged);
    return NdsOverlayCatalogStatus::Ok;
}

} // namespace

NdsOverlayCatalogStatus NdsOverlayCatalog::open(
    const std::filesystem::path &directory,
    std::string_view expected_rom_sha1,
    NdsOverlayCatalog &out,
    std::string *diagnostic) noexcept
{
    try {
        if (!is_hex_string(expected_rom_sha1, 40u)) {
            set_diagnostic(diagnostic, "expected ROM SHA-1 is malformed");
            return NdsOverlayCatalogStatus::InvalidManifest;
        }
        if (directory.empty()) {
            set_diagnostic(diagnostic, "overlay directory is empty");
            return NdsOverlayCatalogStatus::InvalidManifest;
        }

        std::error_code error;
        const auto directory_status =
            std::filesystem::symlink_status(directory, error);
        if (error == std::errc::no_such_file_or_directory ||
            directory_status.type() == std::filesystem::file_type::not_found) {
            set_diagnostic(diagnostic, "overlay directory is missing");
            return NdsOverlayCatalogStatus::Missing;
        }
        if (error) {
            set_diagnostic(diagnostic, "could not inspect overlay directory");
            return NdsOverlayCatalogStatus::IoError;
        }
        if (std::filesystem::is_symlink(directory_status) ||
            !std::filesystem::is_directory(directory_status)) {
            set_diagnostic(diagnostic, "overlay path is not a regular directory");
            return NdsOverlayCatalogStatus::InvalidManifest;
        }

        const std::filesystem::path root =
            std::filesystem::canonical(directory, error);
        if (error) {
            set_diagnostic(diagnostic, "could not canonicalize overlay directory");
            return NdsOverlayCatalogStatus::IoError;
        }

        std::string manifest_contents;
        const auto read_status = read_manifest(
            root / "manifest.json", manifest_contents, diagnostic);
        if (read_status != NdsOverlayCatalogStatus::Ok) return read_status;

        std::string schema;
        std::string rom_sha1;
        uint32_t declared_count = 0u;
        std::vector<NdsOverlayCatalogRecord> records;
        JsonReader reader(manifest_contents);
        if (!reader.parse_manifest(
                schema, rom_sha1, declared_count, records)) {
            set_diagnostic(diagnostic, "overlay manifest JSON or metadata is invalid");
            return NdsOverlayCatalogStatus::InvalidManifest;
        }
        if (rom_sha1 != expected_rom_sha1) {
            set_diagnostic(diagnostic, "overlay manifest belongs to a different ROM identity");
            return NdsOverlayCatalogStatus::RomIdentityMismatch;
        }

        NdsOverlayCatalog staged;
        staged.directory_ = root;
        staged.records_ = std::move(records);
        out = std::move(staged);
        set_diagnostic(diagnostic, "ok");
        return NdsOverlayCatalogStatus::Ok;
    } catch (const std::bad_alloc &) {
        set_diagnostic(diagnostic, "allocation failed while opening overlay catalog");
        return NdsOverlayCatalogStatus::IoError;
    } catch (const std::filesystem::filesystem_error &) {
        set_diagnostic(diagnostic, "filesystem error while opening overlay catalog");
        return NdsOverlayCatalogStatus::IoError;
    } catch (...) {
        set_diagnostic(diagnostic, "unexpected error while opening overlay catalog");
        return NdsOverlayCatalogStatus::IoError;
    }
}

const NdsOverlayCatalogRecord *NdsOverlayCatalog::find(
    NdsCpu cpu, uint32_t overlay_id) const noexcept
{
    const auto found = std::find_if(
        records_.begin(), records_.end(),
        [cpu, overlay_id](const NdsOverlayCatalogRecord &record) {
            return record.cpu == cpu && record.overlay_id == overlay_id;
        });
    return found == records_.end() ? nullptr : &*found;
}

NdsOverlayCatalogStatus NdsOverlayCatalog::read_image(
    const NdsOverlayCatalogRecord &record,
    std::vector<uint8_t> &contents,
    std::string *diagnostic) const noexcept
{
    try {
        const NdsOverlayCatalogRecord *owned =
            find(record.cpu, record.overlay_id);
        if (owned == nullptr ||
            owned->image_file != record.image_file ||
            owned->image_sha1 != record.image_sha1 ||
            owned->image_size != record.image_size ||
            owned->ram_address != record.ram_address ||
            owned->ram_size != record.ram_size ||
            owned->bss_size != record.bss_size ||
            owned->static_init_start != record.static_init_start ||
            owned->static_init_end != record.static_init_end) {
            set_diagnostic(diagnostic, "overlay record does not belong to this catalog");
            return NdsOverlayCatalogStatus::ImageInvalid;
        }
        if (!valid_record(*owned)) {
            set_diagnostic(diagnostic, "overlay record failed local validation");
            return NdsOverlayCatalogStatus::ImageInvalid;
        }

        std::vector<uint8_t> staged;
        const auto status = read_overlay_file(
            directory_ / owned->image_file, *owned, staged, diagnostic);
        if (status != NdsOverlayCatalogStatus::Ok) return status;
        contents = std::move(staged);
        return NdsOverlayCatalogStatus::Ok;
    } catch (const std::bad_alloc &) {
        set_diagnostic(diagnostic, "allocation failed while reading overlay image");
        return NdsOverlayCatalogStatus::IoError;
    } catch (const std::filesystem::filesystem_error &) {
        set_diagnostic(diagnostic, "filesystem error while reading overlay image");
        return NdsOverlayCatalogStatus::IoError;
    } catch (...) {
        set_diagnostic(diagnostic, "unexpected error while reading overlay image");
        return NdsOverlayCatalogStatus::IoError;
    }
}

const char *nds_overlay_catalog_status_name(
    NdsOverlayCatalogStatus status) noexcept
{
    switch (status) {
    case NdsOverlayCatalogStatus::Ok: return "ok";
    case NdsOverlayCatalogStatus::Missing: return "missing";
    case NdsOverlayCatalogStatus::IoError: return "io-error";
    case NdsOverlayCatalogStatus::InvalidManifest: return "invalid-manifest";
    case NdsOverlayCatalogStatus::RomIdentityMismatch: return "rom-identity-mismatch";
    case NdsOverlayCatalogStatus::OverlayNotFound: return "overlay-not-found";
    case NdsOverlayCatalogStatus::ImageMissing: return "image-missing";
    case NdsOverlayCatalogStatus::ImageInvalid: return "image-invalid";
    case NdsOverlayCatalogStatus::ImageHashMismatch: return "image-hash-mismatch";
    }
    return "unknown";
}

} // namespace oracis::host::detail
