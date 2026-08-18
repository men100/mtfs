#include "cli.hpp"

#include <string>

int main(int argc, char **argv)
{
    std::string key, input;
    mtfs::sealed::Policy policy;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg(argv[i]);
        if (arg == "--key" && i + 1 < argc)
            key = argv[++i];
        else if (arg == "--input" && i + 1 < argc)
            input = argv[++i];
        else if (arg == "--target-id" && i + 1 < argc &&
                 mtfs_parse_u32(argv[i + 1], &policy.expected_target_id))
            ++i;
        else if (arg == "--accelerator-id" && i + 1 < argc &&
                 mtfs_parse_u32(argv[i + 1], &policy.expected_accelerator_id))
            ++i;
        else if (arg == "--model-format" && i + 1 < argc &&
                 mtfs_parse_u32(argv[i + 1], &policy.expected_model_format))
            ++i;
        else if (arg == "--max-chunk-size" && i + 1 < argc &&
                 mtfs_parse_u32(argv[i + 1], &policy.max_chunk_size))
            ++i;
        else
        {
            std::cerr << "invalid argument: " << arg << '\n';
            return 2;
        }
    }
    if (key.empty() || input.empty())
    {
        std::cerr << "usage: mtfs-verify --key FILE --input FILE [--target-id N] "
                     "[--accelerator-id N] [--model-format N] [--max-chunk-size N]\n";
        return 2;
    }
    mtfs::sealed::OpenSslAead aead;
    mtfs::sealed::PackageInfo info;
    const auto status = mtfs::sealed::verify_file(key, input, policy, &info, aead);
    if (status == mtfs::sealed::Status::ok)
        std::cout << "verified: payload_bytes=" << info.payload_plain_length
                  << " chunks=" << info.chunk_count << "\n";
    return mtfs_report(status, "verify");
}
