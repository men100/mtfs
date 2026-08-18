#include "mtfs_sealed_host.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
using mtfs::sealed::Status;

namespace
{
struct Test
{
    unsigned checks = 0, failures = 0, positive = 0, negative = 0;
    void check(bool ok, const std::string &name, bool neg = false)
    {
        ++checks;
        neg ? ++negative : ++positive;
        if (!ok)
        {
            ++failures;
            std::cerr << "FAIL: " << name << '\n';
        }
    }
};

std::vector<std::uint8_t> read_file(const fs::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
bool write_file(const fs::path &path, const std::vector<std::uint8_t> &bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return stream.good();
}
int hd(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
std::vector<std::uint8_t> decode_hex(std::string text)
{
    text.erase(std::remove_if(text.begin(),
                              text.end(),
                              [](char c)
                              {
                                  return c == '\r' || c == '\n' || c == ' ';
                              }),
               text.end());
    std::vector<std::uint8_t> out;
    if (text.size() % 2)
        return out;
    for (std::size_t i = 0; i < text.size(); i += 2)
    {
        int a = hd(text[i]), b = hd(text[i + 1]);
        if (a < 0 || b < 0)
            return {};
        out.push_back(static_cast<std::uint8_t>((a << 4) | b));
    }
    return out;
}
std::vector<std::uint8_t> read_hex(const fs::path &p)
{
    std::ifstream s(p);
    return decode_hex({std::istreambuf_iterator<char>(s), std::istreambuf_iterator<char>()});
}
void write_u32(std::uint8_t *output, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        output[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
void write_u64(std::uint8_t *output, std::uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i)
        output[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

mtfs::sealed::DeterministicMaterial make_deterministic_material(unsigned variant = 0)
{
    mtfs::sealed::DeterministicMaterial m;
    for (unsigned i = 0; i < m.model_key.size(); ++i)
        m.model_key[i] = static_cast<std::uint8_t>(0x20 + i);
    for (unsigned i = 0; i < m.package_id.size(); ++i)
        m.package_id[i] = static_cast<std::uint8_t>(0x40 + i);
    for (unsigned i = 0; i < m.key_nonce.size(); ++i)
        m.key_nonce[i] = static_cast<std::uint8_t>(0x60 + i);
    for (unsigned i = 0; i < m.payload_nonce_prefix.size(); ++i)
        m.payload_nonce_prefix[i] = static_cast<std::uint8_t>(0x70 + i);
    m.package_id[0] ^= static_cast<std::uint8_t>(variant);
    return m;
}
mtfs::sealed::SealOptions make_golden_options(const std::vector<std::uint8_t> &manifest)
{
    mtfs::sealed::SealOptions o;
    for (unsigned i = 0; i < o.model_id.size(); ++i)
        o.model_id[i] = static_cast<std::uint8_t>(0x50 + i);
    o.model_version = UINT64_C(0x0102030405060708);
    o.target_id = 0x11;
    o.accelerator_id = 0x22;
    o.model_format = 0x33;
    o.required_ram = 5000;
    o.chunk_size = 4096;
    if (manifest.size() > 160)
        o.metadata.assign(manifest.begin() + 160, manifest.end());
    return o;
}
bool no_temp(const fs::path &p)
{
    const std::string prefix = p.filename().string() + ".tmp.";
    for (const auto &e : fs::directory_iterator(p.parent_path()))
        if (e.path().filename().string().rfind(prefix, 0) == 0)
            return false;
    return true;
}
Status verify_mutated_package(const fs::path &directory,
                              const fs::path &key_path,
                              const std::vector<std::uint8_t> &package_bytes,
                              const std::string &name,
                              mtfs::sealed::OpenSslAead &aead)
{
    const fs::path package_path = directory / (name + ".mtfs");
    if (!write_file(package_path, package_bytes))
        return Status::io;
    return mtfs::sealed::verify_file(key_path.string(), package_path.string(), {}, nullptr, aead);
}

void test_basic_contracts(Test &t, mtfs::sealed::OpenSslAead &a)
{
    t.check(mtfs::sealed::kKeyDomain.size() == 12 && mtfs::sealed::kKeyDomain.back() == 0 &&
                mtfs::sealed::kKeyDomain[10] == '1',
            "key domain exact length");
    t.check(mtfs::sealed::kChunkDomain.size() == 14 && mtfs::sealed::kChunkDomain.back() == 0 &&
                mtfs::sealed::kChunkDomain[12] == '1',
            "chunk domain exact length");
    std::uint64_t n = 0;
    t.check(mtfs::sealed::checked_add(1, 2, &n) == Status::ok && n == 3, "checked add");
    t.check(mtfs::sealed::checked_add(UINT64_MAX, 1, &n) == Status::overflow, "add overflow", true);
    t.check(mtfs::sealed::checked_multiply(UINT64_MAX, 2, &n) == Status::overflow,
            "multiply overflow",
            true);
    std::array<std::uint8_t, 32> key{};
    std::array<std::uint8_t, 12> nonce{};
    std::array<std::uint8_t, 16> plain{}, cipher{}, tag{}, out{};
    auto ec = decode_hex("cea7403d4d606b6e074ec5d3baf39d18"),
         et = decode_hex("d0d1c8a799996bf0265b98b5d48ab919");
    Status s = a.encrypt(key.data(),
                         nonce.data(),
                         nullptr,
                         0,
                         plain.data(),
                         plain.size(),
                         cipher.data(),
                         tag.data());
    t.check(s == Status::ok && std::equal(cipher.begin(), cipher.end(), ec.begin()) &&
                std::equal(tag.begin(), tag.end(), et.begin()),
            "NIST AES-256-GCM encrypt KAT");
    s = a.decrypt(
        key.data(), nonce.data(), nullptr, 0, cipher.data(), cipher.size(), tag.data(), out.data());
    t.check(s == Status::ok && out == plain, "NIST AES-256-GCM decrypt KAT");
    tag[0] ^= 1;
    out.fill(0xa5);
    s = a.decrypt(
        key.data(), nonce.data(), nullptr, 0, cipher.data(), cipher.size(), tag.data(), out.data());
    t.check(s == Status::authentication && std::all_of(out.begin(),
                                                       out.end(),
                                                       [](auto x)
                                                       {
                                                           return x == 0;
                                                       }),
            "tag failure clears output",
            true);
    const std::vector<std::uint8_t> valid = {1, 0, 1, 0, 4, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    t.check(mtfs::sealed::validate_metadata(valid.data(), valid.size()) == Status::ok,
            "canonical TLV");
    auto v = valid;
    v.insert(v.end(), valid.begin(), valid.end());
    t.check(mtfs::sealed::validate_metadata(v.data(), v.size()) == Status::format,
            "duplicate TLV",
            true);
    v = valid;
    v[0] = 9;
    t.check(mtfs::sealed::validate_metadata(v.data(), v.size()) == Status::unsupported,
            "unknown critical TLV",
            true);
    v[2] = 0;
    t.check(mtfs::sealed::validate_metadata(v.data(), v.size()) == Status::ok,
            "unknown noncritical TLV");
    v = valid;
    v[15] = 1;
    t.check(mtfs::sealed::validate_metadata(v.data(), v.size()) == Status::format,
            "nonzero padding",
            true);
}

void test_golden_vector(Test &t,
                        const fs::path &vec,
                        const fs::path &dir,
                        mtfs::sealed::OpenSslAead &a)
{
    auto pkg = read_file(vec / "golden_package.mtfs"),
         pkghex = read_hex(vec / "golden_package.hex"),
         manifest = read_hex(vec / "golden_manifest.hex"),
         payload = read_file(vec / "golden_payload.bin");
    fs::path key = vec / "fleet_test.key";
    t.check(pkg == pkghex, "binary equals hex vector");
    t.check(pkg.size() >= manifest.size() &&
                std::equal(manifest.begin(), manifest.end(), pkg.begin()),
            "exact canonical manifest bytes");
    mtfs::sealed::PackageInfo info;
    Status s = mtfs::sealed::verify_file(
        key.string(), (vec / "golden_package.mtfs").string(), {}, &info, a);
    t.check(s == Status::ok && info.payload_plain_length == payload.size() && info.chunk_count == 2,
            "verify independent golden");
    mtfs::sealed::Policy wrong_policy;
    wrong_policy.expected_target_id = 0x99;
    t.check(mtfs::sealed::verify_file(
                key.string(), (vec / "golden_package.mtfs").string(), wrong_policy, nullptr, a) ==
                Status::unsupported,
            "authenticated target policy rejection",
            true);
    fs::path recovered = dir / "golden.out";
    s = mtfs::sealed::unseal_file(key.string(),
                                  (vec / "golden_package.mtfs").string(),
                                  recovered.string(),
                                  false,
                                  {},
                                  nullptr,
                                  a);
    t.check(s == Status::ok && read_file(recovered) == payload, "recover golden plaintext");
    mtfs::sealed::DeterministicRandom rng(make_deterministic_material());
    fs::path generated = dir / "golden.generated";
    s = mtfs::sealed::seal_file(key.string(),
                                (vec / "golden_payload.bin").string(),
                                generated.string(),
                                make_golden_options(manifest),
                                rng,
                                a);
    t.check(s == Status::ok && read_file(generated) == pkg, "writer byte-for-byte golden match");
    std::vector<std::uint8_t> keyaad(mtfs::sealed::kKeyDomain.begin(),
                                     mtfs::sealed::kKeyDomain.end());
    keyaad.insert(keyaad.end(), manifest.begin(), manifest.end());
    std::vector<std::uint8_t> chunkaad(mtfs::sealed::kChunkDomain.begin(),
                                       mtfs::sealed::kChunkDomain.end());
    chunkaad.insert(chunkaad.end(), manifest.begin(), manifest.end());
    std::array<std::uint8_t, 8> suffix{};
    write_u32(suffix.data(), 0);
    write_u32(suffix.data() + 4, 4096);
    chunkaad.insert(chunkaad.end(), suffix.begin(), suffix.end());
    auto fleet = read_file(key);
    std::array<std::uint8_t, 32> model{};
    s = a.decrypt(fleet.data(),
                  pkg.data() + 128,
                  keyaad.data(),
                  keyaad.size(),
                  pkg.data() + manifest.size(),
                  32,
                  pkg.data() + manifest.size() + 32,
                  model.data());
    t.check(s == Status::ok && model == make_deterministic_material().model_key,
            "envelope exact AAD/model key");
    const std::size_t at = manifest.size() + 48;
    std::vector<std::uint8_t> first(4096);
    std::array<std::uint8_t, 12> cn{};
    std::copy(pkg.begin() + 140, pkg.begin() + 148, cn.begin());
    s = a.decrypt(model.data(),
                  cn.data(),
                  chunkaad.data(),
                  chunkaad.size(),
                  pkg.data() + at,
                  4096,
                  pkg.data() + at + 4096,
                  first.data());
    t.check(s == Status::ok && std::equal(first.begin(), first.end(), payload.begin()),
            "chunk nonce/AAD/cipher/tag vector");
    chunkaad[chunkaad.size() - 8] = 1;
    s = a.decrypt(model.data(),
                  cn.data(),
                  chunkaad.data(),
                  chunkaad.size(),
                  pkg.data() + at,
                  4096,
                  pkg.data() + at + 4096,
                  first.data());
    t.check(s == Status::authentication, "chunk index AAD mutation", true);
    chunkaad[chunkaad.size() - 8] = 0;
    chunkaad.back() ^= 1;
    s = a.decrypt(model.data(),
                  cn.data(),
                  chunkaad.data(),
                  chunkaad.size(),
                  pkg.data() + at,
                  4096,
                  pkg.data() + at + 4096,
                  first.data());
    t.check(s == Status::authentication, "chunk length AAD mutation", true);
    mtfs::sealed::secure_zero(model.data(), model.size());
}

void test_round_trips(Test &t,
                      const fs::path &vec,
                      const fs::path &dir,
                      mtfs::sealed::OpenSslAead &a)
{
    fs::path key = vec / "fleet_test.key";
    unsigned serial = 1;
    for (std::uint32_t chunk : {4096U, 16384U, 65536U})
        for (std::size_t size : {std::size_t(0),
                                 std::size_t(1),
                                 std::size_t(chunk),
                                 std::size_t(chunk + 1),
                                 std::size_t(chunk * 2 + 17)})
        {
            std::vector<std::uint8_t> p(size);
            for (std::size_t i = 0; i < size; ++i)
                p[i] = static_cast<std::uint8_t>((i * 13 + serial) & 255);
            fs::path src = dir / ("src" + std::to_string(serial)),
                     pkg = dir / ("pkg" + std::to_string(serial));
            write_file(src, p);
            auto o = make_golden_options({});
            o.metadata.clear();
            o.chunk_size = chunk;
            o.required_ram = size;
            mtfs::sealed::DeterministicRandom rng(make_deterministic_material(serial++));
            Status s = mtfs::sealed::seal_file(key.string(), src.string(), pkg.string(), o, rng, a);
            std::vector<std::uint8_t> dest(size + 17, 0xa5);
            std::size_t loaded = 99;
            mtfs::sealed::PackageInfo info;
            if (s == Status::ok)
                s = mtfs::sealed::load_file(
                    key.string(), pkg.string(), dest.data(), dest.size(), {}, &loaded, &info, a);
            bool extra = std::all_of(dest.begin() + static_cast<std::ptrdiff_t>(size),
                                     dest.end(),
                                     [](auto x)
                                     {
                                         return x == 0xa5;
                                     });
            t.check(s == Status::ok && loaded == size && info.chunk_plain_size == chunk &&
                        std::equal(p.begin(), p.end(), dest.begin()) && extra,
                    "roundtrip chunk=" + std::to_string(chunk) + " size=" + std::to_string(size));
        }
}

void test_format_rejections(Test &t,
                            const fs::path &vec,
                            const fs::path &dir,
                            mtfs::sealed::OpenSslAead &a)
{
    const fs::path key_path = vec / "fleet_test.key";
    const auto golden_package = read_file(vec / "golden_package.mtfs");
    struct ByteMutation
    {
        const char *name;
        std::size_t offset;
        std::uint8_t replacement;
    };
    const ByteMutation fixed_field_mutations[] = {
        {"magic", 0, 'X'},
        {"major", 8, 2},
        {"minor", 10, 1},
        {"preamble", 12, 159},
        {"flags", 24, 3},
        {"payload-algorithm", 28, 2},
        {"envelope-algorithm", 30, 2},
        {"key-id", 32, 2},
        {"key-version", 36, 2},
        {"reserved0", 92, 1},
        {"reserved1", 148, 1},
        {"unknown-critical", 160, 9},
        {"duplicate", 176, 1},
        {"unsorted", 176, 0},
        {"padding", 174, 1},
        {"manifest-size", 16, 0},
        {"envelope-size", 124, 47},
        {"zero-count", 116, 0},
        {"count", 116, 3},
        {"chunk-size", 112, 1},
    };
    for (const auto &mutation : fixed_field_mutations)
    {
        auto mutated_package = golden_package;
        mutated_package[mutation.offset] = mutation.replacement;
        t.check(verify_mutated_package(dir, key_path, mutated_package, mutation.name, a) !=
                    Status::ok,
                std::string("reject ") + mutation.name,
                true);
    }
    auto mutated_package = golden_package;
    write_u32(mutated_package.data() + 120, 4097);
    t.check(verify_mutated_package(dir, key_path, mutated_package, "metadata-limit", a) !=
                Status::ok,
            "metadata limit",
            true);
    mutated_package = golden_package;
    std::fill(mutated_package.begin() + 128, mutated_package.begin() + 140, 0);
    t.check(verify_mutated_package(dir, key_path, mutated_package, "zero-key-nonce", a) ==
                Status::format,
            "all-zero key nonce",
            true);
    mutated_package = golden_package;
    std::fill(mutated_package.begin() + 140, mutated_package.begin() + 148, 0);
    t.check(verify_mutated_package(dir, key_path, mutated_package, "zero-prefix", a) ==
                Status::format,
            "all-zero payload nonce prefix",
            true);
    mutated_package = golden_package;
    write_u64(mutated_package.data() + 96, UINT64_MAX);
    write_u64(mutated_package.data() + 104, UINT64_MAX);
    write_u32(mutated_package.data() + 116, UINT32_MAX);
    t.check(verify_mutated_package(dir, key_path, mutated_package, "overflow", a) != Status::ok,
            "parser overflow",
            true);

    struct Truncation
    {
        const char *name;
        std::size_t size;
    };
    const Truncation truncations[] = {{"trunc-preamble", 159},
                                      {"trunc-metadata", 170},
                                      {"trunc-envelope", 239},
                                      {"trunc-cipher", 300},
                                      {"trunc-tag", golden_package.size() - 1}};
    for (const auto &truncation : truncations)
    {
        mutated_package = golden_package;
        mutated_package.resize(truncation.size);
        t.check(verify_mutated_package(dir, key_path, mutated_package, truncation.name, a) !=
                    Status::ok,
                std::string("reject ") + truncation.name,
                true);
    }
    mutated_package = golden_package;
    mutated_package.push_back(0);
    t.check(verify_mutated_package(dir, key_path, mutated_package, "trailing", a) != Status::ok,
            "trailing data",
            true);
}

void test_authentication_rejections(Test &t,
                                    const fs::path &vec,
                                    const fs::path &dir,
                                    mtfs::sealed::OpenSslAead &a)
{
    const fs::path key_path = vec / "fleet_test.key";
    const auto golden_package = read_file(vec / "golden_package.mtfs");
    struct AuthenticationMutation
    {
        const char *name;
        std::size_t offset;
    };
    const AuthenticationMutation mutations[] = {{"env-cipher", 192},
                                                {"env-tag", 239},
                                                {"key-nonce", 128},
                                                {"header-aad", 72},
                                                {"metadata-aad", 168},
                                                {"payload-cipher", 240},
                                                {"payload-tag", 4336},
                                                {"prefix", 140},
                                                {"last-tag", golden_package.size() - 1}};
    auto mutated_package = golden_package;
    for (const auto &mutation : mutations)
    {
        mutated_package = golden_package;
        mutated_package[mutation.offset] ^= 1;
        t.check(verify_mutated_package(dir, key_path, mutated_package, mutation.name, a) ==
                    Status::authentication,
                std::string("authenticate ") + mutation.name,
                true);
    }
    auto wrong_key = read_file(key_path);
    wrong_key[0] ^= 1;
    const fs::path wrong_key_path = dir / "wrong.key";
    write_file(wrong_key_path, wrong_key);
    t.check(mtfs::sealed::verify_file(
                wrong_key_path.string(), (vec / "golden_package.mtfs").string(), {}, nullptr, a) ==
                Status::authentication,
            "wrong fleet key",
            true);
    const std::size_t payload_offset = 240;
    mutated_package = golden_package;
    std::vector<std::uint8_t> first_chunk(mutated_package.begin() + payload_offset,
                                          mutated_package.begin() + payload_offset + 4112);
    std::vector<std::uint8_t> second_chunk(mutated_package.begin() + payload_offset + 4112,
                                           mutated_package.end());
    std::copy(second_chunk.begin(), second_chunk.end(), mutated_package.begin() + payload_offset);
    std::copy(first_chunk.begin(),
              first_chunk.end(),
              mutated_package.begin() + payload_offset + second_chunk.size());
    t.check(verify_mutated_package(dir, key_path, mutated_package, "swap", a) != Status::ok,
            "chunk swap",
            true);
    mutated_package = golden_package;
    mutated_package.insert(mutated_package.end(),
                           golden_package.begin() + payload_offset,
                           golden_package.begin() + payload_offset + 4112);
    t.check(verify_mutated_package(dir, key_path, mutated_package, "duplicate-chunk", a) !=
                Status::ok,
            "chunk duplicate",
            true);
    mutated_package = golden_package;
    mutated_package.erase(mutated_package.begin() + payload_offset,
                          mutated_package.begin() + payload_offset + 4112);
    t.check(verify_mutated_package(dir, key_path, mutated_package, "remove-chunk", a) != Status::ok,
            "chunk removal",
            true);
}

void test_seal_failure_cleanup(Test &t,
                               const fs::path &vec,
                               const fs::path &dir,
                               mtfs::sealed::OpenSslAead &a)
{
    fs::path key = vec / "fleet_test.key", src = vec / "golden_payload.bin";
    auto manifest = read_hex(vec / "golden_manifest.hex");
    fs::path failed_key = dir / "failed.key";
    mtfs::sealed::DeterministicRandom key_rng(make_deterministic_material(), 1);
    mtfs::sealed::FaultPlan key_faults;
    Status key_status =
        mtfs::sealed::generate_fleet_key(failed_key.string(), false, key_rng, &key_faults);
    t.check(key_status == Status::rng && !fs::exists(failed_key) && no_temp(failed_key) &&
                key_faults.zeroize_calls > 0,
            "keygen RNG cleanup",
            true);
    for (std::uint64_t call = 1; call <= 4; ++call)
    {
        fs::path out = dir / ("rngfail" + std::to_string(call));
        mtfs::sealed::DeterministicRandom rng(make_deterministic_material(), call);
        Status s = mtfs::sealed::seal_file(
            key.string(), src.string(), out.string(), make_golden_options(manifest), rng, a);
        t.check(s == Status::rng && !fs::exists(out) && no_temp(out),
                "RNG cleanup " + std::to_string(call),
                true);
    }
    struct SealFaultCase
    {
        const char *name;
        std::uint64_t fail_read_call;
        std::uint64_t fail_write_call;
        std::uint64_t fail_crypto_call;
        bool fail_flush;
    };
    const SealFaultCase cases[] = {{"read", 2, 0, 0, false},
                                   {"write", 0, 1, 0, false},
                                   {"flush", 0, 0, 0, true},
                                   {"crypto-start", 0, 0, 1, false},
                                   {"crypto-update", 0, 0, 2, false}};
    unsigned serial = 50;
    for (const auto &fault_case : cases)
    {
        const fs::path output_path = dir / fault_case.name;
        mtfs::sealed::FaultPlan faults;
        faults.fail_read_call = fault_case.fail_read_call;
        faults.fail_write_call = fault_case.fail_write_call;
        faults.fail_crypto_call = fault_case.fail_crypto_call;
        faults.fail_flush = fault_case.fail_flush;
        auto seal_options = make_golden_options(manifest);
        seal_options.faults = &faults;
        mtfs::sealed::DeterministicRandom rng(make_deterministic_material(serial++));
        const Status status = mtfs::sealed::seal_file(
            key.string(), src.string(), output_path.string(), seal_options, rng, a);
        t.check(status != Status::ok && !fs::exists(output_path) && no_temp(output_path) &&
                    faults.zeroize_calls > 0,
                std::string(fault_case.name) + " cleanup/zeroize",
                true);
    }
}

void test_load_failure_cleanup(Test &t,
                               const fs::path &vec,
                               const fs::path &dir,
                               mtfs::sealed::OpenSslAead &a)
{
    const fs::path key_path = vec / "fleet_test.key";
    auto corrupted_package = read_file(vec / "golden_package.mtfs");
    corrupted_package.back() ^= 1;
    const fs::path corrupted_package_path = dir / "bad.mtfs";
    write_file(corrupted_package_path, corrupted_package);
    std::vector<std::uint8_t> destination(5017, 0xa5);
    std::size_t loaded = 99;
    mtfs::sealed::FaultPlan authentication_faults;
    Status status = mtfs::sealed::load_file(key_path.string(),
                                            corrupted_package_path.string(),
                                            destination.data(),
                                            destination.size(),
                                            {},
                                            &loaded,
                                            nullptr,
                                            a,
                                            &authentication_faults);
    bool payload_was_zeroized = std::all_of(destination.begin(),
                                            destination.begin() + 5000,
                                            [](auto byte)
                                            {
                                                return byte == 0;
                                            });
    bool extra_space_was_preserved = std::all_of(destination.begin() + 5000,
                                                 destination.end(),
                                                 [](auto byte)
                                                 {
                                                     return byte == 0xa5;
                                                 });
    t.check(status == Status::authentication && loaded == 0 && payload_was_zeroized &&
                extra_space_was_preserved && authentication_faults.zeroize_calls > 0,
            "auth failure exact destination zeroization",
            true);

    destination.assign(4999, 0xa5);
    loaded = 99;
    status = mtfs::sealed::load_file(key_path.string(),
                                     (vec / "golden_package.mtfs").string(),
                                     destination.data(),
                                     destination.size(),
                                     {},
                                     &loaded,
                                     nullptr,
                                     a);
    t.check(status == Status::destination_too_small && loaded == 0 &&
                std::all_of(destination.begin(),
                            destination.end(),
                            [](auto byte)
                            {
                                return byte == 0xa5;
                            }),
            "size check leaves destination",
            true);

    destination.assign(5017, 0xa5);
    mtfs::sealed::FaultPlan read_faults;
    read_faults.fail_read_call = 5;
    status = mtfs::sealed::load_file(key_path.string(),
                                     (vec / "golden_package.mtfs").string(),
                                     destination.data(),
                                     destination.size(),
                                     {},
                                     &loaded,
                                     nullptr,
                                     a,
                                     &read_faults);
    payload_was_zeroized = std::all_of(destination.begin(),
                                       destination.begin() + 5000,
                                       [](auto byte)
                                       {
                                           return byte == 0;
                                       });
    extra_space_was_preserved = std::all_of(destination.begin() + 5000,
                                            destination.end(),
                                            [](auto byte)
                                            {
                                                return byte == 0xa5;
                                            });
    t.check(status != Status::ok && payload_was_zeroized && extra_space_was_preserved,
            "payload read failure exact zeroization",
            true);

    destination.assign(5017, 0xa5);
    mtfs::sealed::FaultPlan allocation_faults;
    allocation_faults.fail_allocation = true;
    status = mtfs::sealed::load_file(key_path.string(),
                                     (vec / "golden_package.mtfs").string(),
                                     destination.data(),
                                     destination.size(),
                                     {},
                                     &loaded,
                                     nullptr,
                                     a,
                                     &allocation_faults);
    t.check(status == Status::allocation && std::all_of(destination.begin(),
                                                        destination.end(),
                                                        [](auto byte)
                                                        {
                                                            return byte == 0xa5;
                                                        }),
            "allocation failure leaves destination",
            true);
}

void test_unseal_failure_cleanup(Test &t,
                                 const fs::path &vec,
                                 const fs::path &dir,
                                 mtfs::sealed::OpenSslAead &a)
{
    const fs::path key = vec / "fleet_test.key";
    auto bad = read_file(vec / "golden_package.mtfs");
    bad.back() ^= 1;
    const fs::path bp = dir / "bad-unseal.mtfs";
    write_file(bp, bad);
    fs::path out = dir / "no-plain";
    Status s =
        mtfs::sealed::unseal_file(key.string(), bp.string(), out.string(), false, {}, nullptr, a);
    t.check(s == Status::authentication && !fs::exists(out) && no_temp(out),
            "no partial plaintext after auth failure",
            true);
    mtfs::sealed::FaultPlan wf;
    wf.fail_write_call = 1;
    s = mtfs::sealed::unseal_file(key.string(),
                                  (vec / "golden_package.mtfs").string(),
                                  out.string(),
                                  false,
                                  {},
                                  nullptr,
                                  a,
                                  &wf);
    t.check(
        s != Status::ok && !fs::exists(out) && no_temp(out), "plaintext write temp cleanup", true);
}
} // namespace

int main()
{
    Test test_suite;
    mtfs::sealed::OpenSslAead aead;
    const fs::path vector_directory(MTFS_SEALED_VECTOR_DIR);
    std::array<char, 64> temporary_path{};
    std::snprintf(temporary_path.data(), temporary_path.size(), "/tmp/mtfs-sealed-tests-XXXXXX");
    char *created_directory = ::mkdtemp(temporary_path.data());
    if (created_directory == nullptr)
        return 2;
    const fs::path temporary_directory(created_directory);

    test_basic_contracts(test_suite, aead);
    test_golden_vector(test_suite, vector_directory, temporary_directory, aead);
    test_round_trips(test_suite, vector_directory, temporary_directory, aead);
    test_format_rejections(test_suite, vector_directory, temporary_directory, aead);
    test_authentication_rejections(test_suite, vector_directory, temporary_directory, aead);
    test_seal_failure_cleanup(test_suite, vector_directory, temporary_directory, aead);
    test_load_failure_cleanup(test_suite, vector_directory, temporary_directory, aead);
    test_unseal_failure_cleanup(test_suite, vector_directory, temporary_directory, aead);

    fs::remove_all(temporary_directory);
    std::cout << "checks=" << test_suite.checks << " positive_cases=" << test_suite.positive
              << " negative_cases=" << test_suite.negative << " failures=" << test_suite.failures
              << '\n';
    return test_suite.failures == 0 ? 0 : 1;
}
