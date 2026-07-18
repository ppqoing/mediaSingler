#include "dedup/PdqCandidateBuilder.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

std::uint64_t ParseCount(const char* value) {
    if (value == nullptr || *value == '\0') return 1'000'000;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0) {
        throw std::runtime_error("count must be a positive integer");
    }
    return static_cast<std::uint64_t>(parsed);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::uint64_t count = ParseCount(argc > 1 ? argv[1] : nullptr);
        videosc::dedup::ImageSimilarityConfig config;
        config.candidate_memory_mib = 512;
        config.hot_signature_max_members = 2048;
        config.hot_signature_max_pairs = 2'000'000;
        const std::uint64_t allocationBudget =
            static_cast<std::uint64_t>(config.candidate_memory_mib) * 1024ULL * 1024ULL;
        const std::uint64_t rawBytes =
            count > std::numeric_limits<std::uint64_t>::max() /
                        sizeof(videosc::dedup::PdqCandidateRecord)
                ? std::numeric_limits<std::uint64_t>::max()
                : count * sizeof(videosc::dedup::PdqCandidateRecord);
        if (rawBytes > allocationBudget) {
            std::cout << "status=budget_rejected_before_allocation"
                      << "; records=" << count
                      << "; raw_bytes=" << rawBytes
                      << "; budget_bytes=" << allocationBudget << '\n';
            return 0;
        }

        const auto allocationStart = std::chrono::steady_clock::now();
        std::vector<videosc::dedup::PdqCandidateRecord> records(
            static_cast<std::size_t>(count));
        for (std::uint64_t index = 0; index < count; ++index) {
            auto& record = records[static_cast<std::size_t>(index)];
            for (std::size_t byte = 0; byte < sizeof(index); ++byte) {
                record.digest[byte] =
                    static_cast<std::uint8_t>((index >> (byte * 8U)) & 0xffU);
            }
            record.pdq.fill(0x5a);
            record.dhash = 0x55aa55aa55aa55aaULL;
            record.has_dhash = true;
            record.width = 1920;
            record.height = 1080;
            record.quality = 100;
        }
        const auto buildStart = std::chrono::steady_clock::now();
        std::atomic_bool cancelled{false};
        std::uint64_t visited = 0;
        const auto result = videosc::dedup::PdqCandidateBuilder(config).Build(
            records,
            cancelled,
            [&](const videosc::dedup::Sha512Digest&,
                const videosc::dedup::Sha512Digest&) {
                ++visited;
                return true;
            });
        const auto finished = std::chrono::steady_clock::now();
        const auto allocationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            buildStart - allocationStart).count();
        const auto buildMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            finished - buildStart).count();
        std::cout << "status="
                  << (!result.succeeded && result.deferred_hot_signatures != 0
                          ? "hot_signature_deferred"
                          : (result.succeeded ? "completed" : "failed"))
                  << "; records=" << count
                  << "; allocation_ms=" << allocationMs
                  << "; build_ms=" << buildMs
                  << "; peak_bytes=" << result.peak_memory_bytes
                  << "; candidate_pairs=" << result.candidate_pairs
                  << "; visited_pairs=" << visited
                  << "; deferred_hot_signatures="
                  << result.deferred_hot_signatures
                  << "; message=" << result.message << '\n';
        return !result.succeeded && result.deferred_hot_signatures != 0 ? 0 : 1;
    } catch (const std::exception& exception) {
        std::cerr << "status=exception; message=" << exception.what() << '\n';
        return 2;
    }
}

