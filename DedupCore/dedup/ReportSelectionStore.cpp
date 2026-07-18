#include "ReportSelectionStore.h"

#include "DuplicateReportService.h"

#include <Windows.h>

#include <algorithm>
#include <bitset>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace videosc::dedup {
namespace {

constexpr std::uint8_t kMetadataVersion = 1;
constexpr std::uint8_t kMemberVersion = 1;

std::string KindName(const DuplicateReportKind kind) {
    return kind == DuplicateReportKind::Exact ? "exact" : "similar";
}

std::string Number(const std::uint64_t value) {
    std::ostringstream output;
    output << std::setw(20) << std::setfill('0') << value;
    return output.str();
}

std::string ReportPrefix(const DuplicateReportKind kind, const std::uint64_t report_generation) {
    return "report-selection/" + KindName(kind) + "/" + Number(report_generation) + "/";
}

std::string SelectionPrefix(const DuplicateReportKind kind,
                            const std::uint64_t report_generation,
                            const std::uint64_t selection_generation) {
    return ReportPrefix(kind, report_generation) + Number(selection_generation) + "/";
}

std::string GroupPrefix(const DuplicateReportKind kind,
                        const std::uint64_t report_generation,
                        const std::uint64_t selection_generation,
                        const std::uint64_t group_id) {
    return SelectionPrefix(kind, report_generation, selection_generation) +
           "group/" + Number(group_id) + "/";
}

std::int64_t CurrentUtcMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void AppendU64(std::string& output, const std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
}

bool ReadU64(const std::string_view input, std::size_t& offset, std::uint64_t& value) {
    if (offset + 8 > input.size()) return false;
    value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8) | static_cast<unsigned char>(input[offset++]);
    }
    return true;
}

std::string EncodeSnapshot(const ReportSelectionSnapshot& value) {
    std::string output(1, static_cast<char>(kMetadataVersion));
    AppendU64(output, value.selection_generation);
    AppendU64(output, value.source_report_generation);
    AppendU64(output, value.selected_file_count);
    AppendU64(output, value.selected_total_bytes);
    AppendU64(output, value.selected_group_count);
    AppendU64(output, static_cast<std::uint64_t>(value.updated_utc_ms));
    return output;
}

bool DecodeSnapshot(const std::string_view input, ReportSelectionSnapshot& value) {
    if (input.size() != 49 || static_cast<std::uint8_t>(input[0]) != kMetadataVersion) return false;
    std::size_t offset = 1;
    std::uint64_t updated = 0;
    return ReadU64(input, offset, value.selection_generation) &&
           ReadU64(input, offset, value.source_report_generation) &&
           ReadU64(input, offset, value.selected_file_count) &&
           ReadU64(input, offset, value.selected_total_bytes) &&
           ReadU64(input, offset, value.selected_group_count) &&
           ReadU64(input, offset, updated) &&
           (value.updated_utc_ms = static_cast<std::int64_t>(updated), true);
}

std::string EncodeMember(const ReportSelectionMember& value) {
    std::string output(1, static_cast<char>(kMemberVersion));
    AppendU64(output, value.path_id);
    AppendU64(output, value.size_bytes);
    AppendU64(output, value.retained_path_id);
    output.push_back(value.has_measured_distance ? '\1' : '\0');
    std::uint64_t measured = 0;
    std::uint64_t limit = 0;
    static_assert(sizeof(measured) == sizeof(value.measured_distance));
    std::memcpy(&measured, &value.measured_distance, sizeof(measured));
    std::memcpy(&limit, &value.exclusive_limit, sizeof(limit));
    AppendU64(output, measured);
    AppendU64(output, limit);
    return output;
}

bool DecodeMember(const std::string_view input, ReportSelectionMember& value) {
    if (input.size() != 42 || static_cast<std::uint8_t>(input[0]) != kMemberVersion) return false;
    std::size_t offset = 1;
    std::uint64_t measured = 0;
    std::uint64_t limit = 0;
    if (!ReadU64(input, offset, value.path_id) ||
        !ReadU64(input, offset, value.size_bytes) ||
        !ReadU64(input, offset, value.retained_path_id)) {
        return false;
    }
    value.has_measured_distance = input[offset++] != '\0';
    if (!ReadU64(input, offset, measured) || !ReadU64(input, offset, limit)) return false;
    std::memcpy(&value.measured_distance, &measured, sizeof(measured));
    std::memcpy(&value.exclusive_limit, &limit, sizeof(limit));
    return std::isfinite(value.measured_distance) && std::isfinite(value.exclusive_limit);
}

RocksStatus ReadActiveGeneration(RocksStore& store,
                                 const DuplicateReportKind kind,
                                 const std::uint64_t report_generation,
                                 std::uint64_t& selection_generation) {
    std::string value;
    const RocksStatus loaded = store.Get(
        RocksColumnFamily::Default, ReportPrefix(kind, report_generation) + "active", value);
    if (!loaded.succeeded) return loaded;
    std::size_t offset = 0;
    if (value.size() != 8 || !ReadU64(value, offset, selection_generation)) {
        return {false, "selection_active_corrupted"};
    }
    return {true, {}};
}

std::string EncodeGeneration(const std::uint64_t generation) {
    std::string output;
    AppendU64(output, generation);
    return output;
}

std::string EncodeGroupSummary(const std::vector<ReportSelectionMember>& members,
                               const std::uint64_t selected_bytes) {
    std::string output(1, '\1');
    AppendU64(output, members.size());
    AppendU64(output, selected_bytes);
    AppendU64(output, members.empty() ? 0 : members.front().retained_path_id);
    return output;
}

RocksStatus ReadSnapshot(RocksStore& store,
                         const DuplicateReportKind kind,
                         const std::uint64_t report_generation,
                         const std::uint64_t selection_generation,
                         ReportSelectionSnapshot& snapshot) {
    std::string value;
    const RocksStatus loaded = store.Get(
        RocksColumnFamily::Default,
        SelectionPrefix(kind, report_generation, selection_generation) + "metadata",
        value);
    if (!loaded.succeeded) return loaded;
    if (!DecodeSnapshot(value, snapshot) ||
        snapshot.selection_generation != selection_generation ||
        snapshot.source_report_generation != report_generation) {
        return {false, "selection_metadata_corrupted"};
    }
    return {true, {}};
}

bool AddWithoutOverflow(std::uint64_t& target, const std::uint64_t value) {
    if (target > (std::numeric_limits<std::uint64_t>::max)() - value) return false;
    target += value;
    return true;
}

}  // namespace

ReportSelectionDecision ReportSelectionRules::Evaluate(
    const DuplicateGroup& group,
    const DuplicateMember& retained,
    const DuplicateMember& candidate,
    const ReportSelectionConfig& selection_config,
    const std::uint32_t report_image_maximum_distance,
    const std::uint32_t report_video_maximum_average_distance,
    const bool image_report_already_three_stage_verified) noexcept {
    if (candidate.path_id == retained.path_id) return {false, false, 0.0, 0.0, "retained_member"};
    if (group.kind == DuplicateGroupKind::ExactSha512) return {true, false, 0.0, 0.0, {}};

    if (group.kind == DuplicateGroupKind::SimilarImage) {
        if (image_report_already_three_stage_verified) {
            return {true, false, 0.0, 0.0, {}};
        }
        const double limit = static_cast<double>(
            selection_config.image_dhash_distance_exclusive_limit.value_or(
                report_image_maximum_distance));
        if (!retained.image_dhash.has_value() || !candidate.image_dhash.has_value() ||
            *retained.image_dhash == 0 || *candidate.image_dhash == 0) {
            return {false, false, 0.0, limit, "image_dhash_invalid"};
        }
        const double distance = static_cast<double>(DHashRules::HammingDistance(
            *retained.image_dhash, *candidate.image_dhash));
        return {distance < limit, true, distance, limit,
                distance < limit ? std::string{} : "image_distance_not_below_limit"};
    }

    const double limit = selection_config.video_dhash_average_distance_exclusive_limit.value_or(
        static_cast<double>(report_video_maximum_average_distance));
    if (!retained.has_video_dhashes || !candidate.has_video_dhashes) {
        return {false, false, 0.0, limit, "video_dhash_missing"};
    }
    std::uint32_t total = 0;
    for (std::size_t index = 0; index < retained.video_dhashes.size(); ++index) {
        if (retained.video_dhashes[index] == 0 || candidate.video_dhashes[index] == 0) {
            return {false, false, 0.0, limit, "video_dhash_zero_frame"};
        }
        total += DHashRules::HammingDistance(retained.video_dhashes[index],
                                             candidate.video_dhashes[index]);
    }
    const double distance = static_cast<double>(total) /
                            static_cast<double>(retained.video_dhashes.size());
    return {distance < limit, true, distance, limit,
            distance < limit ? std::string{} : "video_distance_not_below_limit"};
}

ReportSelectionStore::ReportSelectionStore(RocksStore& store) : store_(store) {}

RocksStatus ReportSelectionStore::LoadSnapshot(const DuplicateReportKind kind,
                                                const std::uint64_t report_generation,
                                                ReportSelectionSnapshot& snapshot) const {
    snapshot = {};
    snapshot.source_report_generation = report_generation;
    std::uint64_t selectionGeneration = 0;
    const RocksStatus active = ReadActiveGeneration(store_, kind, report_generation, selectionGeneration);
    if (!active.succeeded && active.message == "not_found") return {true, {}};
    if (!active.succeeded) return active;
    return ReadSnapshot(store_, kind, report_generation, selectionGeneration, snapshot);
}

RocksStatus ReportSelectionStore::LoadGroup(
    const DuplicateReportKind kind,
    const std::uint64_t report_generation,
    const std::uint64_t group_id,
    std::vector<ReportSelectionMember>& members) const {
    members.clear();
    std::uint64_t selectionGeneration = 0;
    const RocksStatus active = ReadActiveGeneration(store_, kind, report_generation, selectionGeneration);
    if (!active.succeeded && active.message == "not_found") return {true, {}};
    if (!active.succeeded) return active;
    const std::string prefix = GroupPrefix(kind, report_generation, selectionGeneration, group_id) + "member/";
    bool corrupted = false;
    const RocksStatus listed = store_.ForEachPrefix(
        RocksColumnFamily::Default, prefix, 0,
        [&](const std::string_view, const std::string_view value) {
            ReportSelectionMember member;
            if (!DecodeMember(value, member)) {
                corrupted = true;
                return false;
            }
            members.push_back(member);
            return true;
        });
    if (!listed.succeeded) return listed;
    if (corrupted) return {false, "selection_member_corrupted"};
    return {true, {}};
}

RocksStatus ReportSelectionStore::ForEachSelectedGroup(
    const DuplicateReportKind kind,
    const std::uint64_t report_generation,
    const std::function<bool(std::uint64_t group_id)>& callback) const {
    std::uint64_t selectionGeneration = 0;
    const RocksStatus active = ReadActiveGeneration(
        store_, kind, report_generation, selectionGeneration);
    if (!active.succeeded && active.message == "not_found") return {true, {}};
    if (!active.succeeded) return active;

    const std::string prefix =
        SelectionPrefix(kind, report_generation, selectionGeneration) + "group/";
    bool corrupted = false;
    const RocksStatus listed = store_.ForEachPrefix(
        RocksColumnFamily::Default,
        prefix,
        0,
        [&](const std::string_view key, const std::string_view) {
            constexpr std::string_view suffix = "/summary";
            if (key.size() != prefix.size() + 20 + suffix.size() ||
                key.substr(key.size() - suffix.size()) != suffix) {
                return true;
            }
            std::uint64_t groupId = 0;
            const char* const begin = key.data() + prefix.size();
            const char* const end = begin + 20;
            const auto parsed = std::from_chars(begin, end, groupId);
            if (parsed.ec != std::errc{} || parsed.ptr != end || groupId == 0) {
                corrupted = true;
                return false;
            }
            return callback(groupId);
        });
    if (!listed.succeeded) return listed;
    return corrupted ? RocksStatus{false, "selection_group_index_corrupted"}
                     : RocksStatus{true, {}};
}

RocksStatus ReportSelectionStore::SetGroup(
    const DuplicateReportKind kind,
    const std::uint64_t report_generation,
    const std::uint64_t group_id,
    const std::vector<ReportSelectionMember>& members,
    ReportSelectionSnapshot& snapshot) {
    std::uint64_t selectionGeneration = 0;
    RocksStatus active = ReadActiveGeneration(store_, kind, report_generation, selectionGeneration);
    if (!active.succeeded && active.message == "not_found") {
        selectionGeneration = (std::max<std::uint64_t>)(1, static_cast<std::uint64_t>(CurrentUtcMilliseconds()));
        snapshot = {selectionGeneration, report_generation, 0, 0, 0, CurrentUtcMilliseconds()};
    } else if (!active.succeeded) {
        return active;
    } else {
        const RocksStatus loaded = ReadSnapshot(
            store_, kind, report_generation, selectionGeneration, snapshot);
        if (!loaded.succeeded) return loaded;
    }

    std::vector<ReportSelectionMember> previous;
    const RocksStatus previousStatus = LoadGroup(kind, report_generation, group_id, previous);
    if (!previousStatus.succeeded) return previousStatus;
    std::uint64_t previousBytes = 0;
    std::uint64_t newBytes = 0;
    for (const auto& member : previous) {
        if (!AddWithoutOverflow(previousBytes, member.size_bytes)) return {false, "selection_size_overflow"};
    }
    for (const auto& member : members) {
        if (member.path_id == 0 || member.retained_path_id == 0 || member.path_id == member.retained_path_id ||
            !std::isfinite(member.measured_distance) || !std::isfinite(member.exclusive_limit) ||
            !AddWithoutOverflow(newBytes, member.size_bytes)) {
            return {false, "selection_member_invalid"};
        }
    }
    if (snapshot.selected_file_count < previous.size() || snapshot.selected_total_bytes < previousBytes) {
        return {false, "selection_summary_inconsistent"};
    }
    snapshot.selected_file_count -= previous.size();
    snapshot.selected_total_bytes -= previousBytes;
    if (previous.empty() != members.empty()) {
        if (members.empty()) {
            if (snapshot.selected_group_count == 0) return {false, "selection_group_summary_inconsistent"};
            --snapshot.selected_group_count;
        } else {
            ++snapshot.selected_group_count;
        }
    }
    if (!AddWithoutOverflow(snapshot.selected_file_count, members.size()) ||
        !AddWithoutOverflow(snapshot.selected_total_bytes, newBytes)) {
        return {false, "selection_summary_overflow"};
    }
    snapshot.updated_utc_ms = CurrentUtcMilliseconds();

    const std::string groupPrefix = GroupPrefix(kind, report_generation, selectionGeneration, group_id);
    std::vector<RocksMutation> mutations;
    mutations.reserve(previous.size() + members.size() + 3);
    for (const auto& member : previous) {
        mutations.push_back({RocksColumnFamily::Default,
                             groupPrefix + "member/" + Number(member.path_id), std::nullopt});
    }
    for (const auto& member : members) {
        mutations.push_back({RocksColumnFamily::Default,
                             groupPrefix + "member/" + Number(member.path_id), EncodeMember(member)});
    }
    mutations.push_back({RocksColumnFamily::Default,
                         groupPrefix + "summary",
                         members.empty()
                             ? std::optional<std::string>{}
                             : std::optional<std::string>{EncodeGroupSummary(members, newBytes)}});
    mutations.push_back({RocksColumnFamily::Default,
                         SelectionPrefix(kind, report_generation, selectionGeneration) + "metadata",
                         EncodeSnapshot(snapshot)});
    mutations.push_back({RocksColumnFamily::Default,
                         ReportPrefix(kind, report_generation) + "active",
                         EncodeGeneration(selectionGeneration)});
    return store_.WriteBatch(mutations, true);
}

RocksStatus ReportSelectionStore::BeginReplacement(
    const DuplicateReportKind kind,
    const std::uint64_t report_generation,
    const std::uint64_t selection_generation) {
    if (selection_generation == 0) return {false, "selection_generation_required"};
    std::uint64_t activeGeneration = 0;
    const RocksStatus active = ReadActiveGeneration(
        store_, kind, report_generation, activeGeneration);
    if (active.succeeded && activeGeneration == selection_generation) {
        return {false, "selection_generation_conflicts_with_active"};
    }
    if (!active.succeeded && active.message != "not_found") return active;
    const RocksStatus cleared = DiscardReplacement(kind, report_generation, selection_generation);
    if (!cleared.succeeded) return cleared;
    ReportSelectionSnapshot snapshot{
        selection_generation, report_generation, 0, 0, 0, CurrentUtcMilliseconds()};
    return store_.Put(RocksColumnFamily::Default,
                      SelectionPrefix(kind, report_generation, selection_generation) + "metadata",
                      EncodeSnapshot(snapshot), true);
}

RocksStatus ReportSelectionStore::SaveReplacementGroup(
    const DuplicateReportKind kind,
    const std::uint64_t report_generation,
    const std::uint64_t selection_generation,
    const std::uint64_t group_id,
    const std::vector<ReportSelectionMember>& members) {
    if (members.empty()) return {true, {}};
    ReportSelectionSnapshot snapshot;
    const RocksStatus loaded = ReadSnapshot(
        store_, kind, report_generation, selection_generation, snapshot);
    if (!loaded.succeeded) return loaded;
    std::uint64_t bytes = 0;
    std::vector<RocksMutation> mutations;
    mutations.reserve(members.size() + 1);
    const std::string prefix = GroupPrefix(kind, report_generation, selection_generation, group_id);
    for (const auto& member : members) {
        if (member.path_id == 0 || member.retained_path_id == 0 || member.path_id == member.retained_path_id ||
            !std::isfinite(member.measured_distance) || !std::isfinite(member.exclusive_limit) ||
            !AddWithoutOverflow(bytes, member.size_bytes)) {
            return {false, "selection_member_invalid"};
        }
        mutations.push_back({RocksColumnFamily::Default,
                             prefix + "member/" + Number(member.path_id), EncodeMember(member)});
    }
    mutations.push_back({RocksColumnFamily::Default,
                         prefix + "summary",
                         EncodeGroupSummary(members, bytes)});
    if (!AddWithoutOverflow(snapshot.selected_file_count, members.size()) ||
        !AddWithoutOverflow(snapshot.selected_total_bytes, bytes) ||
        !AddWithoutOverflow(snapshot.selected_group_count, 1)) {
        return {false, "selection_summary_overflow"};
    }
    snapshot.updated_utc_ms = CurrentUtcMilliseconds();
    mutations.push_back({RocksColumnFamily::Default,
                         SelectionPrefix(kind, report_generation, selection_generation) + "metadata",
                         EncodeSnapshot(snapshot)});
    return store_.WriteBatch(mutations, false);
}

RocksStatus ReportSelectionStore::PublishReplacement(
    const DuplicateReportKind kind,
    const std::uint64_t report_generation,
    const std::uint64_t selection_generation,
    ReportSelectionSnapshot& snapshot) {
    const RocksStatus loaded = ReadSnapshot(
        store_, kind, report_generation, selection_generation, snapshot);
    if (!loaded.succeeded) return loaded;
    return store_.Put(RocksColumnFamily::Default,
                      ReportPrefix(kind, report_generation) + "active",
                      EncodeGeneration(selection_generation), true);
}

RocksStatus ReportSelectionStore::DiscardReplacement(
    const DuplicateReportKind kind,
    const std::uint64_t report_generation,
    const std::uint64_t selection_generation) {
    return store_.DeletePrefix(RocksColumnFamily::Default,
                               SelectionPrefix(kind, report_generation, selection_generation),
                               512, false);
}

RocksStatus ReportSelectionStore::Clear(const DuplicateReportKind kind,
                                        const std::uint64_t report_generation) {
    return store_.DeletePrefix(RocksColumnFamily::Default,
                               ReportPrefix(kind, report_generation), 512, true);
}

RocksStatus ReportSelectionStore::CleanupInterruptedStaging(
    const DuplicateReportKind kind,
    const std::uint64_t report_generation) {
    std::uint64_t activeGeneration = 0;
    const RocksStatus active = ReadActiveGeneration(
        store_, kind, report_generation, activeGeneration);
    if (!active.succeeded && active.message != "not_found") return active;
    const std::string prefix = ReportPrefix(kind, report_generation);
    std::vector<std::uint64_t> stale;
    const RocksStatus listed = store_.ForEachPrefix(
        RocksColumnFamily::Default,
        prefix,
        0,
        [&](const std::string_view key, const std::string_view) {
            const std::size_t begin = prefix.size();
            const std::size_t slash = key.find('/', begin);
            if (slash == std::string_view::npos || slash - begin != 20) return true;
            std::uint64_t generation = 0;
            const auto parsed = std::from_chars(
                key.data() + begin, key.data() + slash, generation);
            if (parsed.ec == std::errc{} && parsed.ptr == key.data() + slash &&
                generation != activeGeneration &&
                std::find(stale.begin(), stale.end(), generation) == stale.end()) {
                stale.push_back(generation);
            }
            return true;
        });
    if (!listed.succeeded) return listed;
    for (const std::uint64_t generation : stale) {
        const RocksStatus deleted = DiscardReplacement(kind, report_generation, generation);
        if (!deleted.succeeded) return deleted;
    }
    return {true, {}};
}

}  // namespace videosc::dedup
