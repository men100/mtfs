#include "cli.hpp"

#include <string>

int main(int argc, char **argv)
{
    std::string key, input, output;
    bool overwrite = false;
    mtfs::sealed::Policy policy;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg(argv[i]);
        if (arg == "--key" && i + 1 < argc)
            key = argv[++i];
        else if (arg == "--input" && i + 1 < argc)
            input = argv[++i];
        else if (arg == "--output" && i + 1 < argc)
            output = argv[++i];
        else if (arg == "--overwrite")
            overwrite = true;
        else if (arg == "--target-id" && i + 1 < argc &&
                 mtfs_parse_u32(argv[i + 1], &policy.expected_target_id))
            ++i;
        else if (arg == "--accelerator-id" && i + 1 < argc &&
                 mtfs_parse_u32(argv[i + 1], &policy.expected_accelerator_id))
            ++i;
        else if (arg == "--model-format" && i + 1 < argc &&
                 mtfs_parse_u32(argv[i + 1], &policy.expected_model_format))
            ++i;
        else
        {
            std::cerr << "invalid argument: " << arg << '\n';
            return 2;
        }
    }
    if (key.empty() || input.empty() || output.empty())
    {
        std::cerr << "usage: mtfs-unseal --key FILE --input FILE --output FILE [--overwrite] "
                     "[--target-id N] [--accelerator-id N] [--model-format N]\n";
        return 2;
    }
    mtfs::sealed::OpenSslAead aead;
    mtfs::sealed::PackageInfo info;
    const auto status =
        mtfs::sealed::unseal_file(key, input, output, overwrite, policy, &info, aead);
    if (status == mtfs::sealed::Status::ok)
        std::cout << "authenticated payload recovered\n";
    return mtfs_report(status, "unseal");
}
