#include "cli.hpp"

#include <openssl/evp.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace
{

constexpr std::size_t kPayloadBytes = 5000;
constexpr std::uint32_t kChunkBytes = 4096;

class TemporaryFile
{
  public:
    explicit TemporaryFile(std::filesystem::path path) : path_(std::move(path)) {}
    ~TemporaryFile()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    const std::filesystem::path &path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

std::string hex_string(const std::uint8_t *data, std::size_t size)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index)
        output << std::setw(2) << static_cast<unsigned int>(data[index]);
    return output.str();
}

bool sha256_file(const std::filesystem::path &path, std::array<std::uint8_t, 32> *digest)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;

    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (context == nullptr)
        return false;
    bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
    std::array<char, 4096> buffer{};
    while (ok && input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0)
            ok = EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(count)) == 1;
    }
    ok = ok && input.eof();
    unsigned int size = 0;
    ok = ok && EVP_DigestFinal_ex(context, digest->data(), &size) == 1 && size == digest->size();
    EVP_MD_CTX_free(context);
    return ok;
}

bool write_payload(const std::filesystem::path &path)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    for (std::size_t offset = 0; offset < kPayloadBytes; ++offset)
    {
        const auto value = static_cast<char>((offset * 7U + 3U) & 0xffU);
        output.write(&value, 1);
    }
    output.flush();
    return output.good();
}

bool write_descriptor(const std::filesystem::path &path,
                      const std::filesystem::path &package,
                      std::uintmax_t package_bytes,
                      const std::array<std::uint8_t, 32> &package_sha,
                      const std::array<std::uint8_t, 32> &payload_sha)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output << "MTFS-KAT-v1\n"
           << "package=" << package.filename().string() << '\n'
           << "package_bytes=" << package_bytes << '\n'
           << "payload_bytes=" << kPayloadBytes << '\n'
           << "chunk_bytes=" << kChunkBytes << '\n'
           << "chunks=2\n"
           << "payload_pattern=(offset*7+3)&0xff\n"
           << "package_sha256=" << hex_string(package_sha.data(), package_sha.size()) << '\n'
           << "payload_sha256=" << hex_string(payload_sha.data(), payload_sha.size()) << '\n'
           << "secret_material=none\n";
    output.flush();
    return output.good();
}

} // namespace

int main(int argc, char **argv)
{
    std::string key;
    std::filesystem::path package;
    std::filesystem::path descriptor;
    bool overwrite = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument(argv[index]);
        if (argument == "--key" && index + 1 < argc)
            key = argv[++index];
        else if (argument == "--package" && index + 1 < argc)
            package = argv[++index];
        else if (argument == "--descriptor" && index + 1 < argc)
            descriptor = argv[++index];
        else if (argument == "--overwrite")
            overwrite = true;
        else
        {
            std::cerr << "invalid argument: " << argument << '\n';
            return 2;
        }
    }
    if (key.empty() || package.empty() || descriptor.empty() || package == descriptor)
    {
        std::cerr << "usage: mtfs-kat-package --key fleet.key --package MTFSKAT.MTF "
                     "--descriptor MTFSKAT.TXT [--overwrite]\n";
        return 2;
    }
    std::error_code path_error;
    const bool package_exists = std::filesystem::exists(package, path_error);
    if (path_error)
    {
        std::cerr << "package path check failed\n";
        return 1;
    }
    const bool descriptor_exists = std::filesystem::exists(descriptor, path_error);
    if (path_error)
    {
        std::cerr << "descriptor path check failed\n";
        return 1;
    }
    if (!overwrite && (package_exists || descriptor_exists))
    {
        std::cerr << "output already exists; use --overwrite\n";
        return 1;
    }

    mtfs::sealed::OpenSslRandom random;
    std::array<std::uint8_t, 8> suffix{};
    if (random.fill(suffix.data(), suffix.size()) != mtfs::sealed::Status::ok)
        return mtfs_report(mtfs::sealed::Status::rng, "temporary name generation");
    TemporaryFile payload(std::filesystem::temp_directory_path() /
                          ("mtfs-kat-" + hex_string(suffix.data(), suffix.size()) + ".bin"));
    if (!write_payload(payload.path()))
    {
        std::cerr << "payload staging failed\n";
        return 1;
    }

    mtfs::sealed::SealOptions options;
    options.model_version = UINT64_C(0x0102030405060708);
    options.target_id = 0x11U;
    options.accelerator_id = 0x22U;
    options.model_format = 0x33U;
    options.required_ram = kPayloadBytes;
    options.chunk_size = kChunkBytes;
    options.overwrite = overwrite;
    options.metadata = {
        0x01, 0x00, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00,
        'f', 'l', 'e', 'e', 't', '-', 'k', 'a', 't', 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    if (random.fill(options.model_id.data(), options.model_id.size()) != mtfs::sealed::Status::ok)
        return mtfs_report(mtfs::sealed::Status::rng, "model ID generation");
    options.model_id[6] = static_cast<std::uint8_t>((options.model_id[6] & 0x0fU) | 0x40U);
    options.model_id[8] = static_cast<std::uint8_t>((options.model_id[8] & 0x3fU) | 0x80U);

    mtfs::sealed::OpenSslAead aead;
    const auto status = mtfs::sealed::seal_file(
        key, payload.path().string(), package.string(), options, random, aead);
    if (status != mtfs::sealed::Status::ok)
        return mtfs_report(status, "KAT package creation");

    std::array<std::uint8_t, 32> package_sha{};
    std::array<std::uint8_t, 32> payload_sha{};
    std::error_code size_error;
    const auto package_bytes = std::filesystem::file_size(package, size_error);
    TemporaryFile descriptor_staging(
        descriptor.string() + ".tmp-" + hex_string(suffix.data(), suffix.size()));
    if (size_error || !sha256_file(package, &package_sha) ||
        !sha256_file(payload.path(), &payload_sha) ||
        !write_descriptor(descriptor_staging.path(), package, package_bytes,
                          package_sha, payload_sha))
    {
        std::cerr << "nonsecret descriptor creation failed; package remains valid\n";
        return 1;
    }
    std::error_code rename_error;
    std::filesystem::rename(descriptor_staging.path(), descriptor, rename_error);
    if (rename_error)
    {
        std::cerr << "nonsecret descriptor commit failed; package remains valid\n";
        return 1;
    }

    std::cout << "fleet-specific KAT package created and self-verified\n"
              << "copy " << package.string() << " and " << descriptor.string()
              << " to the SD card root; fleet.key is not included\n";
    return 0;
}
