#ifndef MTFS_SEALED_CLI_HPP
#define MTFS_SEALED_CLI_HPP

#include "mtfs_sealed_host.hpp"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <string>

inline bool mtfs_parse_u64(const char *text, std::uint64_t *value)
{
    if (text == nullptr || value == nullptr || *text == '-' || *text == '\0')
        return false;
    errno = 0;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0')
        return false;
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

inline bool mtfs_parse_u32(const char *text, std::uint32_t *value)
{
    std::uint64_t parsed = 0;
    if (!mtfs_parse_u64(text, &parsed) || parsed > UINT32_MAX)
        return false;
    *value = static_cast<std::uint32_t>(parsed);
    return true;
}

inline int mtfs_report(mtfs::sealed::Status status, const char *operation)
{
    if (status == mtfs::sealed::Status::ok)
        return 0;
    std::cerr << operation << " failed: " << mtfs::sealed::status_string(status) << '\n';
    return 1;
}

#endif
