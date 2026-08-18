#include "cli.hpp"

#include <array>
#include <string>

namespace
{

std::string format_uuid(const std::array<std::uint8_t, 16> &uuid)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve(36);

    for (std::size_t index = 0; index < uuid.size(); ++index)
    {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            text.push_back('-');
        text.push_back(hex[uuid[index] >> 4U]);
        text.push_back(hex[uuid[index] & 0x0fU]);
    }
    return text;
}

} // namespace

int main(int argc, char **argv)
{
    std::string key, input, output, model_id;
    mtfs::sealed::SealOptions options;
    options.model_version = 1;
    bool have_target = false, have_accelerator = false, have_format = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg(argv[i]);
        if (arg == "--key" && i + 1 < argc)
            key = argv[++i];
        else if (arg == "--input" && i + 1 < argc)
            input = argv[++i];
        else if (arg == "--output" && i + 1 < argc)
            output = argv[++i];
        else if (arg == "--model-id" && i + 1 < argc)
            model_id = argv[++i];
        else if (arg == "--model-version" && i + 1 < argc)
        {
            if (!mtfs_parse_u64(argv[++i], &options.model_version))
            {
                std::cerr << "invalid model version\n";
                return 2;
            }
        }
        else if (arg == "--target-id" && i + 1 < argc)
            have_target = mtfs_parse_u32(argv[++i], &options.target_id);
        else if (arg == "--accelerator-id" && i + 1 < argc)
            have_accelerator = mtfs_parse_u32(argv[++i], &options.accelerator_id);
        else if (arg == "--model-format" && i + 1 < argc)
            have_format = mtfs_parse_u32(argv[++i], &options.model_format);
        else if (arg == "--required-ram" && i + 1 < argc)
        {
            if (!mtfs_parse_u64(argv[++i], &options.required_ram))
            {
                std::cerr << "invalid required RAM size\n";
                return 2;
            }
        }
        else if (arg == "--chunk-size" && i + 1 < argc)
        {
            if (!mtfs_parse_u32(argv[++i], &options.chunk_size))
            {
                std::cerr << "invalid chunk size\n";
                return 2;
            }
        }
        else if (arg == "--overwrite")
            options.overwrite = true;
        else
        {
            std::cerr << "invalid argument: " << arg << '\n';
            return 2;
        }
    }
    if (key.empty() || input.empty() || output.empty() || !have_target || !have_accelerator ||
        !have_format)
    {
        std::cerr
            << "usage: mtfs-seal --key FILE --input FILE --output FILE --target-id N "
               "--accelerator-id N --model-format N [--model-id UUID] [--model-version N] "
               "[--required-ram N] [--chunk-size 4096|8192|16384|32768|65536] [--overwrite]\n";
        return 2;
    }

    mtfs::sealed::OpenSslRandom rng;
    const bool generated_model_id = model_id.empty();
    if (generated_model_id)
    {
        const auto status = rng.fill(options.model_id.data(), options.model_id.size());
        if (status != mtfs::sealed::Status::ok)
            return mtfs_report(status, "model ID generation");

        options.model_id[6] = static_cast<std::uint8_t>((options.model_id[6] & 0x0fU) | 0x40U);
        options.model_id[8] = static_cast<std::uint8_t>((options.model_id[8] & 0x3fU) | 0x80U);
    }
    else if (mtfs::sealed::parse_uuid(model_id, &options.model_id) != mtfs::sealed::Status::ok)
    {
        std::cerr << "invalid model ID\n";
        return 2;
    }

    mtfs::sealed::OpenSslAead aead;
    const auto status = mtfs::sealed::seal_file(key, input, output, options, rng, aead);
    if (status == mtfs::sealed::Status::ok)
    {
        std::cout << "sealed package created and self-verified\n";
        if (generated_model_id)
            std::cout << "model-id: " << format_uuid(options.model_id) << '\n';
        std::cout << "model-version: " << options.model_version << '\n';
    }
    return mtfs_report(status, "seal");
}
