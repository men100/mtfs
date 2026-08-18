#ifndef MTFS_SEALED_HOST_HPP
#define MTFS_SEALED_HOST_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mtfs::sealed
{

constexpr std::size_t kFleetKeySize = 32;
constexpr std::size_t kModelKeySize = 32;
constexpr std::size_t kPreambleSize = 160;
constexpr std::size_t kMaxMetadataSize = 4096;
constexpr std::size_t kEnvelopeSize = 48;
constexpr std::size_t kTagSize = 16;
constexpr std::size_t kNonceSize = 12;
constexpr std::uint32_t kDefaultChunkSize = 65536;
constexpr std::array<std::uint8_t, 12> kKeyDomain = {
    'M', 'T', 'F', 'S', '-', 'K', 'E', 'Y', '-', 'v', '1', 0};
constexpr std::array<std::uint8_t, 14> kChunkDomain = {
    'M', 'T', 'F', 'S', '-', 'C', 'H', 'U', 'N', 'K', '-', 'v', '1', 0};

enum class Status
{
    ok = 0,
    invalid_argument,
    already_exists,
    io,
    rng,
    crypto,
    authentication,
    format,
    unsupported,
    overflow,
    destination_too_small,
    allocation,
    injected_failure
};

const char *status_string(Status status) noexcept;

struct PackageInfo
{
    std::array<std::uint8_t, 16> package_id{};
    std::array<std::uint8_t, 16> model_id{};
    std::uint64_t model_version = 0;
    std::uint32_t target_id = 0;
    std::uint32_t accelerator_id = 0;
    std::uint32_t model_format = 0;
    std::uint64_t required_ram = 0;
    std::uint64_t payload_plain_length = 0;
    std::uint32_t chunk_plain_size = 0;
    std::uint32_t chunk_count = 0;
    std::vector<std::uint8_t> metadata;
};

struct Policy
{
    std::uint32_t expected_target_id = 0;
    std::uint32_t expected_accelerator_id = 0;
    std::uint32_t expected_model_format = 0;
    std::uint32_t max_chunk_size = 65536;
};

struct FaultPlan
{
    std::uint64_t fail_read_call = 0;
    std::uint64_t fail_write_call = 0;
    std::uint64_t fail_crypto_call = 0;
    bool fail_flush = false;
    bool fail_allocation = false;
    std::uint64_t read_calls = 0;
    std::uint64_t write_calls = 0;
    std::uint64_t crypto_calls = 0;
    std::uint64_t zeroize_calls = 0;
};

class RandomSource
{
  public:
    virtual ~RandomSource() = default;
    virtual Status fill(std::uint8_t *output, std::size_t size) = 0;
};

class OpenSslRandom final : public RandomSource
{
  public:
    Status fill(std::uint8_t *output, std::size_t size) override;
};

class AeadProvider
{
  public:
    virtual ~AeadProvider() = default;
    virtual Status encrypt(const std::uint8_t key[32],
                           const std::uint8_t nonce[12],
                           const std::uint8_t *aad,
                           std::size_t aad_size,
                           const std::uint8_t *plaintext,
                           std::size_t plaintext_size,
                           std::uint8_t *ciphertext,
                           std::uint8_t tag[16]) = 0;
    virtual Status decrypt(const std::uint8_t key[32],
                           const std::uint8_t nonce[12],
                           const std::uint8_t *aad,
                           std::size_t aad_size,
                           const std::uint8_t *ciphertext,
                           std::size_t ciphertext_size,
                           const std::uint8_t tag[16],
                           std::uint8_t *plaintext) = 0;
};

class OpenSslAead final : public AeadProvider
{
  public:
    Status encrypt(const std::uint8_t key[32],
                   const std::uint8_t nonce[12],
                   const std::uint8_t *aad,
                   std::size_t aad_size,
                   const std::uint8_t *plaintext,
                   std::size_t plaintext_size,
                   std::uint8_t *ciphertext,
                   std::uint8_t tag[16]) override;
    Status decrypt(const std::uint8_t key[32],
                   const std::uint8_t nonce[12],
                   const std::uint8_t *aad,
                   std::size_t aad_size,
                   const std::uint8_t *ciphertext,
                   std::size_t ciphertext_size,
                   const std::uint8_t tag[16],
                   std::uint8_t *plaintext) override;
};

struct SealOptions
{
    std::array<std::uint8_t, 16> model_id{};
    std::uint64_t model_version = 0;
    std::uint32_t target_id = 0;
    std::uint32_t accelerator_id = 0;
    std::uint32_t model_format = 0;
    std::uint64_t required_ram = 0;
    std::uint32_t chunk_size = kDefaultChunkSize;
    std::vector<std::uint8_t> metadata;
    bool overwrite = false;
    FaultPlan *faults = nullptr;
};

struct DeterministicMaterial
{
    std::array<std::uint8_t, 32> model_key{};
    std::array<std::uint8_t, 16> package_id{};
    std::array<std::uint8_t, 12> key_nonce{};
    std::array<std::uint8_t, 8> payload_nonce_prefix{};
};

class DeterministicRandom final : public RandomSource
{
  public:
    explicit DeterministicRandom(const DeterministicMaterial &material,
                                 std::uint64_t fail_call = 0);
    Status fill(std::uint8_t *output, std::size_t size) override;

  private:
    DeterministicMaterial material_;
    std::uint64_t call_ = 0;
    std::uint64_t fail_call_ = 0;
};

Status validate_metadata(const std::uint8_t *data, std::size_t size) noexcept;
Status checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t *result) noexcept;
Status checked_multiply(std::uint64_t a, std::uint64_t b, std::uint64_t *result) noexcept;
void secure_zero(void *data, std::size_t size, FaultPlan *faults = nullptr) noexcept;

Status generate_fleet_key(const std::string &output_path,
                          bool overwrite,
                          RandomSource &rng,
                          FaultPlan *faults = nullptr);
Status seal_file(const std::string &key_path,
                 const std::string &input_path,
                 const std::string &output_path,
                 const SealOptions &options,
                 RandomSource &rng,
                 AeadProvider &aead);
Status verify_file(const std::string &key_path,
                   const std::string &package_path,
                   const Policy &policy,
                   PackageInfo *info,
                   AeadProvider &aead,
                   FaultPlan *faults = nullptr);
Status unseal_file(const std::string &key_path,
                   const std::string &package_path,
                   const std::string &output_path,
                   bool overwrite,
                   const Policy &policy,
                   PackageInfo *info,
                   AeadProvider &aead,
                   FaultPlan *faults = nullptr);
Status load_file(const std::string &key_path,
                 const std::string &package_path,
                 void *destination,
                 std::size_t destination_size,
                 const Policy &policy,
                 std::size_t *loaded_size,
                 PackageInfo *info,
                 AeadProvider &aead,
                 FaultPlan *faults = nullptr);

Status parse_uuid(const std::string &text, std::array<std::uint8_t, 16> *uuid) noexcept;

} // namespace mtfs::sealed

#endif
