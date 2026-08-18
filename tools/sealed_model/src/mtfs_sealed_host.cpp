#include "mtfs_sealed_host.hpp"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace mtfs::sealed
{
namespace
{

constexpr std::array<std::uint8_t, 8> kMagic = {'M', 'T', 'F', 'S', 'M', 'O', 'D', 0};
constexpr std::uint16_t kMajor = 1;
constexpr std::uint16_t kMinor = 0;
constexpr std::uint32_t kObjectModel = 1;
constexpr std::uint32_t kFlagChunked = 1;
constexpr std::uint16_t kAes256Gcm16 = 1;
constexpr std::uint16_t kTlvCritical = 1;

using FilePtr = std::unique_ptr<std::FILE, int (*)(std::FILE *)>;

void put_u16(std::uint8_t *p, std::uint16_t value)
{
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8U);
}

void put_u32(std::uint8_t *p, std::uint32_t value)
{
    for (unsigned int i = 0; i < 4; ++i)
        p[i] = static_cast<std::uint8_t>(value >> (8U * i));
}

void put_u64(std::uint8_t *p, std::uint64_t value)
{
    for (unsigned int i = 0; i < 8; ++i)
        p[i] = static_cast<std::uint8_t>(value >> (8U * i));
}

std::uint16_t get_u16(const std::uint8_t *p)
{
    return static_cast<std::uint16_t>(p[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8U);
}

std::uint32_t get_u32(const std::uint8_t *p)
{
    std::uint32_t value = 0;
    for (unsigned int i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(p[i]) << (8U * i);
    return value;
}

std::uint64_t get_u64(const std::uint8_t *p)
{
    std::uint64_t value = 0;
    for (unsigned int i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(p[i]) << (8U * i);
    return value;
}

bool all_zero(const std::uint8_t *p, std::size_t size)
{
    std::uint8_t value = 0;
    for (std::size_t i = 0; i < size; ++i)
        value = static_cast<std::uint8_t>(value | p[i]);
    return value == 0;
}

bool valid_chunk_size(std::uint32_t size)
{
    return size >= 4096U && size <= 65536U && (size & (size - 1U)) == 0U;
}

bool valid_utf8_no_nul(const std::uint8_t *data, std::size_t size)
{
    for (std::size_t i = 0; i < size;)
    {
        const std::uint8_t c = data[i];
        if (c == 0)
            return false;
        std::size_t need = 0;
        std::uint32_t code = 0;
        if (c < 0x80U)
        {
            ++i;
            continue;
        }
        if ((c & 0xe0U) == 0xc0U)
        {
            need = 1;
            code = c & 0x1fU;
        }
        else if ((c & 0xf0U) == 0xe0U)
        {
            need = 2;
            code = c & 0x0fU;
        }
        else if ((c & 0xf8U) == 0xf0U)
        {
            need = 3;
            code = c & 0x07U;
        }
        else
            return false;
        if (i + need >= size)
            return false;
        for (std::size_t j = 1; j <= need; ++j)
        {
            if ((data[i + j] & 0xc0U) != 0x80U)
                return false;
            code = (code << 6U) | (data[i + j] & 0x3fU);
        }
        if ((need == 1 && code < 0x80U) || (need == 2 && code < 0x800U) ||
            (need == 3 && code < 0x10000U) || code > 0x10ffffU ||
            (code >= 0xd800U && code <= 0xdfffU))
            return false;
        i += need + 1;
    }
    return true;
}

Status maybe_fail_read(FaultPlan *faults)
{
    if (faults == nullptr)
        return Status::ok;
    ++faults->read_calls;
    return faults->fail_read_call == faults->read_calls ? Status::injected_failure : Status::ok;
}

Status maybe_fail_write(FaultPlan *faults)
{
    if (faults == nullptr)
        return Status::ok;
    ++faults->write_calls;
    return faults->fail_write_call == faults->write_calls ? Status::injected_failure : Status::ok;
}

Status read_exact(std::FILE *file, void *data, std::size_t size, FaultPlan *faults)
{
    const Status injected = maybe_fail_read(faults);
    if (injected != Status::ok)
        return injected;
    return std::fread(data, 1, size, file) == size ? Status::ok : Status::io;
}

Status write_exact(std::FILE *file, const void *data, std::size_t size, FaultPlan *faults)
{
    const Status injected = maybe_fail_write(faults);
    if (injected != Status::ok)
        return injected;
    return std::fwrite(data, 1, size, file) == size ? Status::ok : Status::io;
}

Status flush_file(std::FILE *file, FaultPlan *faults)
{
    if (faults != nullptr && faults->fail_flush)
        return Status::injected_failure;
    if (std::fflush(file) != 0)
        return Status::io;
    return ::fsync(::fileno(file)) == 0 ? Status::ok : Status::io;
}

Status file_size(const std::string &path, std::uint64_t *size)
{
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || st.st_size < 0 || !S_ISREG(st.st_mode))
        return Status::io;
    *size = static_cast<std::uint64_t>(st.st_size);
    return Status::ok;
}

Status read_key(const std::string &path, std::array<std::uint8_t, 32> *key, FaultPlan *faults)
{
    FilePtr file(std::fopen(path.c_str(), "rb"), &std::fclose);
    if (!file)
        return Status::io;
    Status status = read_exact(file.get(), key->data(), key->size(), faults);
    if (status == Status::ok)
    {
        std::uint8_t extra = 0;
        if (std::fread(&extra, 1, 1, file.get()) != 0 || std::ferror(file.get()))
            status = Status::format;
    }
    if (status != Status::ok)
        secure_zero(key->data(), key->size(), faults);
    return status;
}

class TempFile
{
  public:
    TempFile() = default;
    ~TempFile()
    {
        cleanup();
    }
    Status create(const std::string &destination)
    {
        std::vector<char> name;
        try
        {
            path_ = destination + ".tmp.XXXXXX";
            name.assign(path_.begin(), path_.end());
            name.push_back('\0');
        }
        catch (const std::bad_alloc &)
        {
            path_.clear();
            return Status::allocation;
        }
        const int fd = ::mkstemp(name.data());
        if (fd < 0)
            return Status::io;
        path_ = name.data();
        if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0)
        {
            ::close(fd);
            cleanup();
            return Status::io;
        }
        file_ = ::fdopen(fd, "w+b");
        if (file_ == nullptr)
        {
            ::close(fd);
            cleanup();
            return Status::io;
        }
        return Status::ok;
    }
    std::FILE *get() const
    {
        return file_;
    }
    const std::string &path() const
    {
        return path_;
    }
    Status close_synced(FaultPlan *faults)
    {
        if (file_ == nullptr)
            return Status::io;
        Status status = flush_file(file_, faults);
        if (std::fclose(file_) != 0 && status == Status::ok)
            status = Status::io;
        file_ = nullptr;
        return status;
    }
    Status commit(const std::string &destination, bool overwrite)
    {
        int result = 0;
        if (overwrite)
            result = ::rename(path_.c_str(), destination.c_str());
        else
        {
            result = ::link(path_.c_str(), destination.c_str());
            if (result == 0 && ::unlink(path_.c_str()) != 0)
            {
                ::unlink(destination.c_str());
                return Status::io;
            }
        }
        if (result != 0)
            return errno == EEXIST ? Status::already_exists : Status::io;
        path_.clear();
        return Status::ok;
    }

  private:
    void cleanup()
    {
        if (file_ != nullptr)
        {
            std::fclose(file_);
            file_ = nullptr;
        }
        if (!path_.empty())
        {
            ::unlink(path_.c_str());
            path_.clear();
        }
    }
    std::FILE *file_ = nullptr;
    std::string path_;
};

Status crypto_gate(FaultPlan *faults)
{
    if (faults == nullptr)
        return Status::ok;
    ++faults->crypto_calls;
    return faults->fail_crypto_call == faults->crypto_calls ? Status::injected_failure : Status::ok;
}

Status encrypt_call(AeadProvider &aead,
                    FaultPlan *faults,
                    const std::uint8_t key[32],
                    const std::uint8_t nonce[12],
                    const std::uint8_t *aad,
                    std::size_t aad_size,
                    const std::uint8_t *plain,
                    std::size_t plain_size,
                    std::uint8_t *cipher,
                    std::uint8_t tag[16])
{
    const Status gate = crypto_gate(faults);
    return gate == Status::ok
               ? aead.encrypt(key, nonce, aad, aad_size, plain, plain_size, cipher, tag)
               : gate;
}

Status decrypt_call(AeadProvider &aead,
                    FaultPlan *faults,
                    const std::uint8_t key[32],
                    const std::uint8_t nonce[12],
                    const std::uint8_t *aad,
                    std::size_t aad_size,
                    const std::uint8_t *cipher,
                    std::size_t cipher_size,
                    const std::uint8_t tag[16],
                    std::uint8_t *plain)
{
    const Status gate = crypto_gate(faults);
    return gate == Status::ok
               ? aead.decrypt(key, nonce, aad, aad_size, cipher, cipher_size, tag, plain)
               : gate;
}

std::vector<std::uint8_t> make_aad(const std::uint8_t *domain,
                                   std::size_t domain_size,
                                   const std::vector<std::uint8_t> &manifest,
                                   std::uint32_t index = 0,
                                   std::uint32_t length = 0,
                                   bool chunk = false)
{
    std::vector<std::uint8_t> aad;
    aad.reserve(domain_size + manifest.size() + (chunk ? 8U : 0U));
    aad.insert(aad.end(), domain, domain + domain_size);
    aad.insert(aad.end(), manifest.begin(), manifest.end());
    if (chunk)
    {
        std::array<std::uint8_t, 8> suffix{};
        put_u32(suffix.data(), index);
        put_u32(suffix.data() + 4, length);
        aad.insert(aad.end(), suffix.begin(), suffix.end());
    }
    return aad;
}

struct Parsed
{
    FilePtr file{nullptr, &std::fclose};
    std::vector<std::uint8_t> manifest;
    PackageInfo info;
    std::array<std::uint8_t, 8> prefix{};
    std::array<std::uint8_t, 32> model_key{};
    FaultPlan *faults = nullptr;
    ~Parsed()
    {
        secure_zero(model_key.data(), model_key.size(), faults);
    }
};

struct PreambleFields
{
    std::uint32_t manifest_size = 0;
    std::uint32_t metadata_size = 0;
    std::array<std::uint8_t, 12> key_nonce{};
};

class ScopedZeroizer
{
  public:
    ScopedZeroizer(void *data, std::size_t size, FaultPlan *faults)
        : data_(data), size_(size), faults_(faults)
    {
    }

    ~ScopedZeroizer()
    {
        secure_zero(data_, size_, faults_);
    }

    ScopedZeroizer(const ScopedZeroizer &) = delete;
    ScopedZeroizer &operator=(const ScopedZeroizer &) = delete;

  private:
    void *data_;
    std::size_t size_;
    FaultPlan *faults_;
};

Status check_policy(const PackageInfo &info, const Policy &policy)
{
    if (policy.max_chunk_size < 4096U || info.chunk_plain_size > policy.max_chunk_size)
        return Status::unsupported;
    if (policy.expected_target_id != 0 && info.target_id != policy.expected_target_id)
        return Status::unsupported;
    if (policy.expected_accelerator_id != 0 &&
        info.accelerator_id != policy.expected_accelerator_id)
        return Status::unsupported;
    if (policy.expected_model_format != 0 && info.model_format != policy.expected_model_format)
        return Status::unsupported;
    return Status::ok;
}

Status decode_preamble(Parsed *parsed, PreambleFields *fields)
{
    const std::uint8_t *preamble = parsed->manifest.data();
    if (!std::equal(kMagic.begin(), kMagic.end(), preamble) || get_u16(preamble + 8) != kMajor ||
        get_u16(preamble + 10) != kMinor || get_u32(preamble + 12) != kPreambleSize)
    {
        return Status::format;
    }

    fields->manifest_size = get_u32(preamble + 16);
    fields->metadata_size = get_u32(preamble + 120);
    std::copy(preamble + 128, preamble + 140, fields->key_nonce.begin());

    const bool invalid_fixed_fields =
        fields->manifest_size != kPreambleSize + fields->metadata_size ||
        fields->metadata_size > kMaxMetadataSize || get_u32(preamble + 20) != kObjectModel ||
        get_u32(preamble + 24) != kFlagChunked || get_u16(preamble + 28) != kAes256Gcm16 ||
        get_u16(preamble + 30) != kAes256Gcm16 || get_u32(preamble + 32) != 1 ||
        get_u32(preamble + 36) != 1 || get_u32(preamble + 124) != kEnvelopeSize ||
        !all_zero(preamble + 92, 4) || !all_zero(preamble + 148, 12) ||
        all_zero(preamble + 40, 16) || all_zero(preamble + 128, 12) || all_zero(preamble + 140, 8);
    if (invalid_fixed_fields)
    {
        return Status::format;
    }

    PackageInfo &info = parsed->info;
    std::copy(preamble + 40, preamble + 56, info.package_id.begin());
    std::copy(preamble + 56, preamble + 72, info.model_id.begin());
    info.model_version = get_u64(preamble + 72);
    info.target_id = get_u32(preamble + 80);
    info.accelerator_id = get_u32(preamble + 84);
    info.model_format = get_u32(preamble + 88);
    info.required_ram = get_u64(preamble + 96);
    info.payload_plain_length = get_u64(preamble + 104);
    info.chunk_plain_size = get_u32(preamble + 112);
    info.chunk_count = get_u32(preamble + 116);
    std::copy(preamble + 140, preamble + 148, parsed->prefix.begin());

    if (!valid_chunk_size(info.chunk_plain_size) || info.required_ram < info.payload_plain_length)
    {
        return Status::format;
    }
    return Status::ok;
}

Status validate_chunk_count(const PackageInfo &info)
{
    std::uint64_t expected_count = 0;
    if (info.payload_plain_length != 0)
    {
        std::uint64_t rounded_length = 0;
        Status status =
            checked_add(info.payload_plain_length, info.chunk_plain_size - 1U, &rounded_length);
        if (status != Status::ok)
        {
            return status;
        }
        expected_count = rounded_length / info.chunk_plain_size;
    }

    if (expected_count > UINT32_MAX || info.chunk_count != expected_count)
    {
        return Status::format;
    }
    return Status::ok;
}

Status validate_file_layout(const std::string &package_path,
                            std::uint32_t manifest_size,
                            const PackageInfo &info)
{
    std::uint64_t tag_bytes = 0;
    Status status = checked_multiply(info.chunk_count, kTagSize, &tag_bytes);

    std::uint64_t expected_size = 0;
    if (status == Status::ok)
        status = checked_add(manifest_size, kEnvelopeSize, &expected_size);
    if (status == Status::ok)
        status = checked_add(expected_size, info.payload_plain_length, &expected_size);
    if (status == Status::ok)
        status = checked_add(expected_size, tag_bytes, &expected_size);
    if (status != Status::ok)
        return status;

    std::uint64_t actual_size = 0;
    status = file_size(package_path, &actual_size);
    if (status != Status::ok)
        return status;
    return actual_size == expected_size ? Status::ok : Status::format;
}

Status read_and_authenticate_manifest(Parsed *parsed,
                                      const PreambleFields &fields,
                                      const std::uint8_t fleet_key[kFleetKeySize],
                                      AeadProvider &aead,
                                      FaultPlan *faults)
{
    try
    {
        parsed->manifest.resize(fields.manifest_size);
        parsed->info.metadata.resize(fields.metadata_size);
    }
    catch (const std::bad_alloc &)
    {
        return Status::allocation;
    }

    Status status = Status::ok;
    if (fields.metadata_size != 0)
    {
        status = read_exact(parsed->file.get(),
                            parsed->manifest.data() + kPreambleSize,
                            fields.metadata_size,
                            faults);
        if (status == Status::ok)
        {
            std::copy(parsed->manifest.begin() + kPreambleSize,
                      parsed->manifest.end(),
                      parsed->info.metadata.begin());
        }
    }

    std::array<std::uint8_t, kEnvelopeSize> envelope{};
    ScopedZeroizer clear_envelope(envelope.data(), envelope.size(), faults);
    if (status == Status::ok)
        status = read_exact(parsed->file.get(), envelope.data(), envelope.size(), faults);
    if (status != Status::ok)
        return status;

    try
    {
        const auto aad = make_aad(kKeyDomain.data(), kKeyDomain.size(), parsed->manifest);
        return decrypt_call(aead,
                            faults,
                            fleet_key,
                            fields.key_nonce.data(),
                            aad.data(),
                            aad.size(),
                            envelope.data(),
                            kModelKeySize,
                            envelope.data() + kModelKeySize,
                            parsed->model_key.data());
    }
    catch (const std::bad_alloc &)
    {
        return Status::allocation;
    }
}

Status open_package(const std::string &key_path,
                    const std::string &package_path,
                    const Policy &policy,
                    AeadProvider &aead,
                    FaultPlan *faults,
                    Parsed *parsed)
{
    std::array<std::uint8_t, 32> fleet_key{};
    ScopedZeroizer clear_fleet_key(fleet_key.data(), fleet_key.size(), faults);
    Status status = read_key(key_path, &fleet_key, faults);
    if (status != Status::ok)
        return status;

    parsed->faults = faults;
    try
    {
        if (faults != nullptr && faults->fail_allocation)
        {
            status = Status::allocation;
            throw std::bad_alloc();
        }
        parsed->manifest.resize(kPreambleSize);
    }
    catch (const std::bad_alloc &)
    {
        return Status::allocation;
    }

    parsed->file.reset(std::fopen(package_path.c_str(), "rb"));
    if (!parsed->file)
        return Status::io;

    status = read_exact(parsed->file.get(), parsed->manifest.data(), kPreambleSize, faults);
    if (status != Status::ok)
        return status;

    PreambleFields fields;
    status = decode_preamble(parsed, &fields);
    if (status == Status::ok)
        status = validate_chunk_count(parsed->info);
    if (status == Status::ok)
        status = validate_file_layout(package_path, fields.manifest_size, parsed->info);
    if (status != Status::ok)
        return status;

    status = read_and_authenticate_manifest(parsed, fields, fleet_key.data(), aead, faults);
    if (status != Status::ok)
        return status;

    status = validate_metadata(parsed->info.metadata.data(), parsed->info.metadata.size());
    if (status != Status::ok)
        return status;
    return check_policy(parsed->info, policy);
}

enum class SinkKind
{
    discard,
    memory,
    file
};

Status process_payload(Parsed &parsed,
                       AeadProvider &aead,
                       SinkKind kind,
                       std::uint8_t *memory,
                       std::size_t memory_size,
                       std::FILE *output,
                       std::array<std::uint8_t, 32> *digest)
{
    const std::uint64_t payload_size = parsed.info.payload_plain_length;
    if (kind == SinkKind::memory &&
        (payload_size > memory_size || parsed.info.required_ram > memory_size))
        return Status::destination_too_small;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> md(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!md || EVP_DigestInit_ex(md.get(), EVP_sha256(), nullptr) != 1)
        return Status::crypto;
    std::vector<std::uint8_t> cipher;
    std::vector<std::uint8_t> scratch;
    try
    {
        cipher.resize(parsed.info.chunk_plain_size);
        scratch.resize(parsed.info.chunk_plain_size);
    }
    catch (const std::bad_alloc &)
    {
        return Status::allocation;
    }
    Status status = Status::ok;
    std::uint64_t offset = 0;
    bool payload_io_started = false;
    for (std::uint32_t i = 0; i < parsed.info.chunk_count && status == Status::ok; ++i)
    {
        const std::uint64_t remaining = payload_size - offset;
        const std::uint32_t length = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(remaining, parsed.info.chunk_plain_size));
        std::array<std::uint8_t, 16> tag{};
        payload_io_started = true;
        status = read_exact(parsed.file.get(), cipher.data(), length, parsed.faults);
        if (status == Status::ok)
            status = read_exact(parsed.file.get(), tag.data(), tag.size(), parsed.faults);
        std::array<std::uint8_t, 12> nonce{};
        std::copy(parsed.prefix.begin(), parsed.prefix.end(), nonce.begin());
        put_u32(nonce.data() + 8, i);
        if (status == Status::ok)
        {
            try
            {
                const auto aad = make_aad(
                    kChunkDomain.data(), kChunkDomain.size(), parsed.manifest, i, length, true);
                status = decrypt_call(aead,
                                      parsed.faults,
                                      parsed.model_key.data(),
                                      nonce.data(),
                                      aad.data(),
                                      aad.size(),
                                      cipher.data(),
                                      length,
                                      tag.data(),
                                      scratch.data());
            }
            catch (const std::bad_alloc &)
            {
                status = Status::allocation;
            }
        }
        if (status == Status::ok && EVP_DigestUpdate(md.get(), scratch.data(), length) != 1)
            status = Status::crypto;
        if (status == Status::ok && kind == SinkKind::memory)
            std::memcpy(memory + static_cast<std::size_t>(offset), scratch.data(), length);
        if (status == Status::ok && kind == SinkKind::file)
            status = write_exact(output, scratch.data(), length, parsed.faults);
        secure_zero(scratch.data(), length, parsed.faults);
        offset += length;
    }
    if (status == Status::ok)
    {
        unsigned int digest_size = 0;
        if (EVP_DigestFinal_ex(md.get(), digest->data(), &digest_size) != 1 ||
            digest_size != digest->size())
            status = Status::crypto;
    }
    secure_zero(cipher.data(), cipher.size(), parsed.faults);
    secure_zero(scratch.data(), scratch.size(), parsed.faults);
    if (status != Status::ok && kind == SinkKind::memory && payload_io_started)
        secure_zero(memory, static_cast<std::size_t>(payload_size), parsed.faults);
    return status;
}

Status digest_package(const std::string &key_path,
                      const std::string &package_path,
                      const Policy &policy,
                      PackageInfo *info,
                      AeadProvider &aead,
                      FaultPlan *faults,
                      std::array<std::uint8_t, 32> *digest)
{
    Parsed parsed;
    Status status = open_package(key_path, package_path, policy, aead, faults, &parsed);
    if (status == Status::ok)
        status = process_payload(parsed, aead, SinkKind::discard, nullptr, 0, nullptr, digest);
    if (status == Status::ok && info != nullptr)
    {
        try
        {
            *info = parsed.info;
        }
        catch (const std::bad_alloc &)
        {
            status = Status::allocation;
        }
    }
    return status;
}

} // namespace

const char *status_string(Status status) noexcept
{
    switch (status)
    {
    case Status::ok:
        return "success";
    case Status::invalid_argument:
        return "invalid argument";
    case Status::already_exists:
        return "output already exists";
    case Status::io:
        return "I/O failure";
    case Status::rng:
        return "random generator failure";
    case Status::crypto:
        return "cryptographic operation failure";
    case Status::authentication:
        return "package authentication failed";
    case Status::format:
        return "invalid package format";
    case Status::unsupported:
        return "unsupported package policy";
    case Status::overflow:
        return "integer overflow";
    case Status::destination_too_small:
        return "destination is too small";
    case Status::allocation:
        return "allocation failure";
    case Status::injected_failure:
        return "injected test failure";
    }
    return "unknown failure";
}

Status checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t *result) noexcept
{
    if (result == nullptr)
        return Status::invalid_argument;
    if (a > std::numeric_limits<std::uint64_t>::max() - b)
        return Status::overflow;
    *result = a + b;
    return Status::ok;
}

Status checked_multiply(std::uint64_t a, std::uint64_t b, std::uint64_t *result) noexcept
{
    if (result == nullptr)
        return Status::invalid_argument;
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a)
        return Status::overflow;
    *result = a * b;
    return Status::ok;
}

void secure_zero(void *data, std::size_t size, FaultPlan *faults) noexcept
{
    if (data != nullptr && size != 0)
        OPENSSL_cleanse(data, size);
    if (faults != nullptr)
        ++faults->zeroize_calls;
}

Status validate_metadata(const std::uint8_t *data, std::size_t size) noexcept
{
    if (size > kMaxMetadataSize || (size != 0 && data == nullptr))
        return Status::format;
    std::size_t offset = 0;
    std::uint16_t previous = 0;
    while (offset < size)
    {
        if (size - offset < 8)
            return Status::format;
        const std::uint16_t type = get_u16(data + offset);
        const std::uint16_t flags = get_u16(data + offset + 2);
        const std::uint32_t length = get_u32(data + offset + 4);
        if (type == 0 || type <= previous || (flags & ~kTlvCritical) != 0)
            return Status::format;
        if (length > size - offset - 8)
            return Status::format;
        const std::uint8_t *value = data + offset + 8;
        bool known_valid = true;
        switch (type)
        {
        case 1:
            known_valid = length == 4;
            break;
        case 2:
            known_valid = length == 8;
            break;
        case 3:
            known_valid = length >= 1 && length <= 1024 && valid_utf8_no_nul(value, length);
            break;
        case 4:
            known_valid = length >= 1 && length <= 255 && valid_utf8_no_nul(value, length);
            break;
        default:
            if ((flags & kTlvCritical) != 0)
                return Status::unsupported;
            break;
        }
        if (!known_valid)
            return Status::format;
        std::uint64_t raw = 0;
        if (checked_add(8, length, &raw) != Status::ok)
            return Status::overflow;
        const std::uint64_t padded = (raw + 7U) & ~UINT64_C(7);
        if (padded > size - offset)
            return Status::format;
        for (std::size_t i = static_cast<std::size_t>(raw); i < padded; ++i)
            if (data[offset + i] != 0)
                return Status::format;
        offset += static_cast<std::size_t>(padded);
        previous = type;
    }
    return Status::ok;
}

Status OpenSslRandom::fill(std::uint8_t *output, std::size_t size)
{
    if ((output == nullptr && size != 0) || size > INT_MAX)
        return Status::invalid_argument;
    ERR_clear_error();
    const int result = RAND_bytes(output, static_cast<int>(size));
    ERR_clear_error();
    return result == 1 ? Status::ok : Status::rng;
}

DeterministicRandom::DeterministicRandom(const DeterministicMaterial &material,
                                         std::uint64_t fail_call)
    : material_(material), fail_call_(fail_call)
{
}

Status DeterministicRandom::fill(std::uint8_t *output, std::size_t size)
{
    ++call_;
    if (call_ == fail_call_)
        return Status::rng;
    const std::uint8_t *source = nullptr;
    std::size_t expected = 0;
    if (call_ == 1)
    {
        source = material_.model_key.data();
        expected = material_.model_key.size();
    }
    else if (call_ == 2)
    {
        source = material_.package_id.data();
        expected = material_.package_id.size();
    }
    else if (call_ == 3)
    {
        source = material_.key_nonce.data();
        expected = material_.key_nonce.size();
    }
    else if (call_ == 4)
    {
        source = material_.payload_nonce_prefix.data();
        expected = material_.payload_nonce_prefix.size();
    }
    else
        return Status::rng;
    if (output == nullptr || size != expected)
        return Status::invalid_argument;
    std::memcpy(output, source, size);
    return Status::ok;
}

namespace
{
Status openssl_cipher(bool encrypt,
                      const std::uint8_t key[32],
                      const std::uint8_t nonce[12],
                      const std::uint8_t *aad,
                      std::size_t aad_size,
                      const std::uint8_t *input,
                      std::size_t input_size,
                      const std::uint8_t *input_tag,
                      std::uint8_t *output,
                      std::uint8_t *output_tag)
{
    if (key == nullptr || nonce == nullptr || (aad_size != 0 && aad == nullptr) ||
        (input_size != 0 && (input == nullptr || output == nullptr)) ||
        (encrypt && output_tag == nullptr) || (!encrypt && input_tag == nullptr))
        return Status::invalid_argument;
    if (aad_size > INT_MAX || input_size > INT_MAX)
        return Status::overflow;
    ERR_clear_error();
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),
                                                                        &EVP_CIPHER_CTX_free);
    if (!ctx)
    {
        ERR_clear_error();
        return Status::crypto;
    }
    int length = 0;
    int total = 0;
    int ok =
        EVP_CipherInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr, encrypt ? 1 : 0);
    ok = ok && EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
    ok = ok && EVP_CipherInit_ex(ctx.get(), nullptr, nullptr, key, nonce, -1);
    if (aad_size != 0)
        ok = ok && EVP_CipherUpdate(ctx.get(), nullptr, &length, aad, static_cast<int>(aad_size));
    if (input_size != 0)
    {
        ok =
            ok && EVP_CipherUpdate(ctx.get(), output, &length, input, static_cast<int>(input_size));
        total = length;
    }
    if (!encrypt)
        ok = ok && EVP_CIPHER_CTX_ctrl(
                       ctx.get(), EVP_CTRL_GCM_SET_TAG, 16, const_cast<std::uint8_t *>(input_tag));
    if (!ok)
    {
        ERR_clear_error();
        return Status::crypto;
    }
    std::uint8_t empty_output = 0;
    std::uint8_t *final_output = output == nullptr ? &empty_output : output + total;
    const int final_result = EVP_CipherFinal_ex(ctx.get(), final_output, &length);
    if (!encrypt && final_result != 1)
    {
        if (input_size != 0)
            OPENSSL_cleanse(output, input_size);
        ERR_clear_error();
        return Status::authentication;
    }
    if (encrypt && (final_result != 1 ||
                    EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, output_tag) != 1))
    {
        ERR_clear_error();
        return Status::crypto;
    }
    if (static_cast<std::size_t>(total + length) != input_size)
    {
        ERR_clear_error();
        return Status::crypto;
    }
    ERR_clear_error();
    return Status::ok;
}
} // namespace

Status OpenSslAead::encrypt(const std::uint8_t key[32],
                            const std::uint8_t nonce[12],
                            const std::uint8_t *aad,
                            std::size_t aad_size,
                            const std::uint8_t *plaintext,
                            std::size_t plaintext_size,
                            std::uint8_t *ciphertext,
                            std::uint8_t tag[16])
{
    return openssl_cipher(
        true, key, nonce, aad, aad_size, plaintext, plaintext_size, nullptr, ciphertext, tag);
}

Status OpenSslAead::decrypt(const std::uint8_t key[32],
                            const std::uint8_t nonce[12],
                            const std::uint8_t *aad,
                            std::size_t aad_size,
                            const std::uint8_t *ciphertext,
                            std::size_t ciphertext_size,
                            const std::uint8_t tag[16],
                            std::uint8_t *plaintext)
{
    return openssl_cipher(
        false, key, nonce, aad, aad_size, ciphertext, ciphertext_size, tag, plaintext, nullptr);
}

Status generate_fleet_key(const std::string &output_path,
                          bool overwrite,
                          RandomSource &rng,
                          FaultPlan *faults)
{
    if (output_path.empty())
        return Status::invalid_argument;
    if (!overwrite && ::access(output_path.c_str(), F_OK) == 0)
        return Status::already_exists;
    std::array<std::uint8_t, 32> key{};
    Status status = rng.fill(key.data(), key.size());
    if (status != Status::ok || all_zero(key.data(), key.size()))
        status = Status::rng;
    TempFile temp;
    if (status == Status::ok)
        status = temp.create(output_path);
    if (status == Status::ok)
        status = write_exact(temp.get(), key.data(), key.size(), faults);
    if (status == Status::ok)
        status = temp.close_synced(faults);
    if (status == Status::ok)
        status = temp.commit(output_path, overwrite);
    secure_zero(key.data(), key.size(), faults);
    return status;
}

namespace
{
struct SealLayout
{
    std::uint64_t payload_size = 0;
    std::uint64_t required_ram = 0;
    std::uint32_t chunk_count = 0;
};

struct SealMaterial
{
    explicit SealMaterial(FaultPlan *fault_plan) : faults(fault_plan)
    {
    }

    ~SealMaterial()
    {
        secure_zero(fleet_key.data(), fleet_key.size(), faults);
        secure_zero(model_key.data(), model_key.size(), faults);
    }

    std::array<std::uint8_t, 32> fleet_key{};
    std::array<std::uint8_t, 32> model_key{};
    std::array<std::uint8_t, 16> package_id{};
    std::array<std::uint8_t, 12> key_nonce{};
    std::array<std::uint8_t, 8> payload_nonce_prefix{};
    FaultPlan *faults;
};

Status
prepare_seal_layout(const std::string &input_path, const SealOptions &options, SealLayout *layout)
{
    Status status = validate_metadata(options.metadata.data(), options.metadata.size());
    if (status != Status::ok)
        return status;

    status = file_size(input_path, &layout->payload_size);
    if (status != Status::ok)
        return status;

    std::uint64_t chunk_count = 0;
    if (layout->payload_size != 0)
    {
        std::uint64_t rounded_length = 0;
        status = checked_add(layout->payload_size, options.chunk_size - 1U, &rounded_length);
        if (status != Status::ok)
            return status;
        chunk_count = rounded_length / options.chunk_size;
    }
    if (chunk_count > UINT32_MAX)
        return Status::overflow;

    layout->required_ram = options.required_ram == 0 ? layout->payload_size : options.required_ram;
    if (layout->required_ram < layout->payload_size)
        return Status::invalid_argument;

    layout->chunk_count = static_cast<std::uint32_t>(chunk_count);
    return Status::ok;
}

Status generate_seal_material(const std::string &key_path,
                              const SealOptions &options,
                              RandomSource &rng,
                              SealMaterial *material)
{
    if (options.faults != nullptr && options.faults->fail_allocation)
        return Status::allocation;

    Status status = read_key(key_path, &material->fleet_key, options.faults);
    if (status == Status::ok)
        status = rng.fill(material->model_key.data(), material->model_key.size());
    if (status == Status::ok)
        status = rng.fill(material->package_id.data(), material->package_id.size());
    if (status == Status::ok)
        status = rng.fill(material->key_nonce.data(), material->key_nonce.size());
    if (status == Status::ok)
    {
        status =
            rng.fill(material->payload_nonce_prefix.data(), material->payload_nonce_prefix.size());
    }
    if (status != Status::ok)
        return status;

    const bool generated_zero_value =
        all_zero(material->model_key.data(), material->model_key.size()) ||
        all_zero(material->package_id.data(), material->package_id.size()) ||
        all_zero(material->key_nonce.data(), material->key_nonce.size()) ||
        all_zero(material->payload_nonce_prefix.data(), material->payload_nonce_prefix.size());
    return generated_zero_value ? Status::rng : Status::ok;
}

Status encode_manifest(const SealOptions &options,
                       const SealLayout &layout,
                       const SealMaterial &material,
                       std::vector<std::uint8_t> *manifest)
{
    try
    {
        manifest->assign(kPreambleSize + options.metadata.size(), 0);
    }
    catch (const std::bad_alloc &)
    {
        return Status::allocation;
    }

    std::uint8_t *preamble = manifest->data();
    std::copy(kMagic.begin(), kMagic.end(), preamble);
    put_u16(preamble + 8, kMajor);
    put_u16(preamble + 10, kMinor);
    put_u32(preamble + 12, kPreambleSize);
    put_u32(preamble + 16, static_cast<std::uint32_t>(manifest->size()));
    put_u32(preamble + 20, kObjectModel);
    put_u32(preamble + 24, kFlagChunked);
    put_u16(preamble + 28, kAes256Gcm16);
    put_u16(preamble + 30, kAes256Gcm16);
    put_u32(preamble + 32, 1);
    put_u32(preamble + 36, 1);
    std::copy(material.package_id.begin(), material.package_id.end(), preamble + 40);
    std::copy(options.model_id.begin(), options.model_id.end(), preamble + 56);
    put_u64(preamble + 72, options.model_version);
    put_u32(preamble + 80, options.target_id);
    put_u32(preamble + 84, options.accelerator_id);
    put_u32(preamble + 88, options.model_format);
    put_u64(preamble + 96, layout.required_ram);
    put_u64(preamble + 104, layout.payload_size);
    put_u32(preamble + 112, options.chunk_size);
    put_u32(preamble + 116, layout.chunk_count);
    put_u32(preamble + 120, static_cast<std::uint32_t>(options.metadata.size()));
    put_u32(preamble + 124, kEnvelopeSize);
    std::copy(material.key_nonce.begin(), material.key_nonce.end(), preamble + 128);
    std::copy(
        material.payload_nonce_prefix.begin(), material.payload_nonce_prefix.end(), preamble + 140);
    std::copy(options.metadata.begin(), options.metadata.end(), preamble + kPreambleSize);
    return Status::ok;
}

Status write_manifest_and_envelope(std::FILE *output,
                                   const std::vector<std::uint8_t> &manifest,
                                   const SealMaterial &material,
                                   const SealOptions &options,
                                   AeadProvider &aead)
{
    std::array<std::uint8_t, kEnvelopeSize> envelope{};
    ScopedZeroizer clear_envelope(envelope.data(), envelope.size(), options.faults);

    Status status = Status::ok;
    try
    {
        const auto aad = make_aad(kKeyDomain.data(), kKeyDomain.size(), manifest);
        status = encrypt_call(aead,
                              options.faults,
                              material.fleet_key.data(),
                              material.key_nonce.data(),
                              aad.data(),
                              aad.size(),
                              material.model_key.data(),
                              material.model_key.size(),
                              envelope.data(),
                              envelope.data() + kModelKeySize);
    }
    catch (const std::bad_alloc &)
    {
        return Status::allocation;
    }
    if (status != Status::ok)
        return status;

    status = write_exact(output, manifest.data(), manifest.size(), options.faults);
    if (status == Status::ok)
        status = write_exact(output, envelope.data(), envelope.size(), options.faults);
    return status;
}

Status encrypt_payload_stream(const std::string &input_path,
                              std::FILE *output,
                              const std::vector<std::uint8_t> &manifest,
                              const SealLayout &layout,
                              const SealMaterial &material,
                              const SealOptions &options,
                              AeadProvider &aead,
                              std::array<std::uint8_t, 32> *source_digest)
{
    FilePtr input(std::fopen(input_path.c_str(), "rb"), &std::fclose);
    if (!input)
        return Status::io;

    std::vector<std::uint8_t> plaintext;
    std::vector<std::uint8_t> ciphertext;
    try
    {
        plaintext.resize(options.chunk_size);
        ciphertext.resize(options.chunk_size);
    }
    catch (const std::bad_alloc &)
    {
        return Status::allocation;
    }
    ScopedZeroizer clear_plaintext(plaintext.data(), plaintext.size(), options.faults);
    ScopedZeroizer clear_ciphertext(ciphertext.data(), ciphertext.size(), options.faults);

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest_context(EVP_MD_CTX_new(),
                                                                           &EVP_MD_CTX_free);
    if (!digest_context || EVP_DigestInit_ex(digest_context.get(), EVP_sha256(), nullptr) != 1)
        return Status::crypto;

    Status status = Status::ok;
    std::uint64_t offset = 0;
    for (std::uint32_t index = 0; index < layout.chunk_count && status == Status::ok; ++index)
    {
        const std::uint32_t chunk_length = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(layout.payload_size - offset, options.chunk_size));
        status = read_exact(input.get(), plaintext.data(), chunk_length, options.faults);
        if (status == Status::ok &&
            EVP_DigestUpdate(digest_context.get(), plaintext.data(), chunk_length) != 1)
        {
            status = Status::crypto;
        }

        std::array<std::uint8_t, 12> nonce{};
        std::copy(material.payload_nonce_prefix.begin(),
                  material.payload_nonce_prefix.end(),
                  nonce.begin());
        put_u32(nonce.data() + 8, index);
        std::array<std::uint8_t, 16> tag{};

        if (status == Status::ok)
        {
            try
            {
                const auto aad = make_aad(
                    kChunkDomain.data(), kChunkDomain.size(), manifest, index, chunk_length, true);
                status = encrypt_call(aead,
                                      options.faults,
                                      material.model_key.data(),
                                      nonce.data(),
                                      aad.data(),
                                      aad.size(),
                                      plaintext.data(),
                                      chunk_length,
                                      ciphertext.data(),
                                      tag.data());
            }
            catch (const std::bad_alloc &)
            {
                status = Status::allocation;
            }
        }
        if (status == Status::ok)
            status = write_exact(output, ciphertext.data(), chunk_length, options.faults);
        if (status == Status::ok)
            status = write_exact(output, tag.data(), tag.size(), options.faults);

        secure_zero(plaintext.data(), chunk_length, options.faults);
        offset += chunk_length;
    }

    if (status == Status::ok)
    {
        std::uint8_t extra = 0;
        if (std::fread(&extra, 1, 1, input.get()) != 0 || std::ferror(input.get()))
            status = Status::io;
    }

    unsigned int digest_size = 0;
    if (status == Status::ok &&
        (EVP_DigestFinal_ex(digest_context.get(), source_digest->data(), &digest_size) != 1 ||
         digest_size != source_digest->size()))
    {
        status = Status::crypto;
    }
    if (std::fclose(input.release()) != 0 && status == Status::ok)
        status = Status::io;
    return status;
}

Status verify_sealed_output(const std::string &key_path,
                            const std::string &package_path,
                            const SealOptions &options,
                            const std::array<std::uint8_t, 32> &source_digest,
                            AeadProvider &aead,
                            FaultPlan *faults)
{
    Policy policy{};
    policy.expected_target_id = options.target_id;
    policy.expected_accelerator_id = options.accelerator_id;
    policy.expected_model_format = options.model_format;

    std::array<std::uint8_t, 32> verified_digest{};
    ScopedZeroizer clear_digest(verified_digest.data(), verified_digest.size(), faults);
    Status status =
        digest_package(key_path, package_path, policy, nullptr, aead, faults, &verified_digest);
    if (status == Status::ok &&
        CRYPTO_memcmp(source_digest.data(), verified_digest.data(), source_digest.size()) != 0)
    {
        status = Status::authentication;
    }
    return status;
}
} // namespace

Status seal_file(const std::string &key_path,
                 const std::string &input_path,
                 const std::string &output_path,
                 const SealOptions &options,
                 RandomSource &rng,
                 AeadProvider &aead)
{
    if (key_path.empty() || input_path.empty() || output_path.empty() ||
        !valid_chunk_size(options.chunk_size))
        return Status::invalid_argument;
    if (!options.overwrite && ::access(output_path.c_str(), F_OK) == 0)
        return Status::already_exists;

    SealLayout layout;
    Status status = prepare_seal_layout(input_path, options, &layout);

    SealMaterial material(options.faults);
    if (status == Status::ok)
        status = generate_seal_material(key_path, options, rng, &material);

    std::vector<std::uint8_t> manifest;
    if (status == Status::ok)
        status = encode_manifest(options, layout, material, &manifest);

    TempFile temp;
    if (status == Status::ok)
        status = temp.create(output_path);
    if (status == Status::ok)
        status = write_manifest_and_envelope(temp.get(), manifest, material, options, aead);

    std::array<std::uint8_t, 32> source_digest{};
    ScopedZeroizer clear_source_digest(source_digest.data(), source_digest.size(), options.faults);
    if (status == Status::ok)
    {
        status = encrypt_payload_stream(
            input_path, temp.get(), manifest, layout, material, options, aead, &source_digest);
    }
    if (status == Status::ok)
        status = temp.close_synced(options.faults);
    if (status == Status::ok)
        status = verify_sealed_output(
            key_path, temp.path(), options, source_digest, aead, options.faults);
    if (status == Status::ok)
        status = temp.commit(output_path, options.overwrite);
    return status;
}

Status verify_file(const std::string &key_path,
                   const std::string &package_path,
                   const Policy &policy,
                   PackageInfo *info,
                   AeadProvider &aead,
                   FaultPlan *faults)
{
    std::array<std::uint8_t, 32> digest{};
    const Status status =
        digest_package(key_path, package_path, policy, info, aead, faults, &digest);
    secure_zero(digest.data(), digest.size(), faults);
    return status;
}

Status unseal_file(const std::string &key_path,
                   const std::string &package_path,
                   const std::string &output_path,
                   bool overwrite,
                   const Policy &policy,
                   PackageInfo *info,
                   AeadProvider &aead,
                   FaultPlan *faults)
{
    if (output_path.empty())
        return Status::invalid_argument;
    if (!overwrite && ::access(output_path.c_str(), F_OK) == 0)
        return Status::already_exists;
    Parsed parsed;
    Status status = open_package(key_path, package_path, policy, aead, faults, &parsed);
    TempFile temp;
    if (status == Status::ok)
        status = temp.create(output_path);
    std::array<std::uint8_t, 32> digest{};
    if (status == Status::ok)
        status = process_payload(parsed, aead, SinkKind::file, nullptr, 0, temp.get(), &digest);
    if (status == Status::ok && info != nullptr)
    {
        try
        {
            *info = parsed.info;
        }
        catch (const std::bad_alloc &)
        {
            status = Status::allocation;
        }
    }
    if (status == Status::ok)
        status = temp.close_synced(faults);
    if (status == Status::ok)
        status = temp.commit(output_path, overwrite);
    secure_zero(digest.data(), digest.size(), faults);
    return status;
}

Status load_file(const std::string &key_path,
                 const std::string &package_path,
                 void *destination,
                 std::size_t destination_size,
                 const Policy &policy,
                 std::size_t *loaded_size,
                 PackageInfo *info,
                 AeadProvider &aead,
                 FaultPlan *faults)
{
    if ((destination == nullptr && destination_size != 0) || loaded_size == nullptr)
        return Status::invalid_argument;
    *loaded_size = 0;
    Parsed parsed;
    Status status = open_package(key_path, package_path, policy, aead, faults, &parsed);
    if (status == Status::ok && parsed.info.payload_plain_length > SIZE_MAX)
        status = Status::overflow;
    std::array<std::uint8_t, 32> digest{};
    if (status == Status::ok)
        status = process_payload(parsed,
                                 aead,
                                 SinkKind::memory,
                                 static_cast<std::uint8_t *>(destination),
                                 destination_size,
                                 nullptr,
                                 &digest);
    if (status == Status::ok)
    {
        *loaded_size = static_cast<std::size_t>(parsed.info.payload_plain_length);
        if (info != nullptr)
        {
            try
            {
                *info = parsed.info;
            }
            catch (const std::bad_alloc &)
            {
                secure_zero(destination,
                            static_cast<std::size_t>(parsed.info.payload_plain_length),
                            faults);
                *loaded_size = 0;
                status = Status::allocation;
            }
        }
    }
    secure_zero(digest.data(), digest.size(), faults);
    return status;
}

Status parse_uuid(const std::string &text, std::array<std::uint8_t, 16> *uuid) noexcept
{
    if (uuid == nullptr)
        return Status::invalid_argument;
    auto digit = [](char c) -> int
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::size_t nibble = 0;
    uuid->fill(0);
    for (const char c : text)
    {
        if (c == '-')
            continue;
        const int value = digit(c);
        if (value < 0 || nibble >= 32)
            return Status::invalid_argument;
        const std::size_t index = nibble / 2;
        if ((nibble & 1U) == 0)
            (*uuid)[index] = static_cast<std::uint8_t>(value << 4);
        else
            (*uuid)[index] = static_cast<std::uint8_t>((*uuid)[index] | value);
        ++nibble;
    }
    return nibble == 32 ? Status::ok : Status::invalid_argument;
}

} // namespace mtfs::sealed
