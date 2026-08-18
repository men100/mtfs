#include "cli.hpp"

#include <string>

int main(int argc, char **argv)
{
    std::string output;
    bool overwrite = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg(argv[i]);
        if (arg == "--output" && i + 1 < argc)
            output = argv[++i];
        else if (arg == "--overwrite")
            overwrite = true;
        else
        {
            std::cerr << "usage: mtfs-keygen --output FILE [--overwrite]\n";
            return 2;
        }
    }
    if (output.empty())
    {
        std::cerr << "usage: mtfs-keygen --output FILE [--overwrite]\n";
        return 2;
    }
    mtfs::sealed::OpenSslRandom rng;
    const auto status = mtfs::sealed::generate_fleet_key(output, overwrite, rng);
    if (status == mtfs::sealed::Status::ok)
        std::cout << "fleet key created (key_id=1, key_version=1)\n";
    return mtfs_report(status, "key generation");
}
