#include "state.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>

namespace vanacore::soulspire {
namespace {

std::string to_lower_ascii(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c >= 'A' && c <= 'Z')
            out.push_back(static_cast<char>(c + ('a' - 'A')));
        else if (std::isalnum(c) || c == '_' || c == '-')
            out.push_back(static_cast<char>(c));
    }
    return out;
}

std::string normalize_soul_key(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c >= 'A' && c <= 'Z')
            out.push_back(static_cast<char>(c + ('a' - 'A')));
        else if (std::isalnum(c))
            out.push_back(static_cast<char>(c));
    }
    return out;
}

// Minimal, zero-dependency, fail-closed JSON scanner for soul-identities.json and soul-weapon-config.json.
struct JsonScan {
    const char* p = nullptr;
    const char* end = nullptr;

    void ws() noexcept
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            ++p;
    }

    bool take(char ch) noexcept
    {
        ws();
        if (p < end && *p == ch) {
            ++p;
            return true;
        }
        return false;
    }

    bool peek(char ch) noexcept
    {
        ws();
        return p < end && *p == ch;
    }

    bool parse_string(std::string& out)
    {
        ws();
        if (p >= end || *p != '"')
            return false;
        ++p;
        out.clear();
        while (p < end) {
            const char c = *p++;
            if (c == '"')
                return true;
            if (c == '\\') {
                if (p >= end)
                    return false;
                const char e = *p++;
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                    if (end - p < 4)
                        return false;
                    p += 4;
                    out.push_back('?');
                    break;
                default:
                    return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool parse_int(std::int64_t& out) noexcept
    {
        ws();
        if (p >= end)
            return false;
        const char* start = p;
        if (*p == '-')
            ++p;
        const char* digits = p;
        while (p < end && *p >= '0' && *p <= '9')
            ++p;
        if (p == digits) {
            p = start;
            return false;
        }
        std::int64_t v = 0;
        const auto res = std::from_chars(start, p, v);
        if (res.ec != std::errc{}) {
            p = start;
            return false;
        }
        // Skip optional fractional/exponent part if present.
        if (p < end && (*p == '.' || *p == 'e' || *p == 'E')) {
            while (p < end && (*p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-' || (*p >= '0' && *p <= '9')))
                ++p;
        }
        out = v;
        return true;
    }

    bool skip_value(int depth = 0)
    {
        if (depth > 32)
            return false;
        ws();
        if (p >= end)
            return false;
        if (*p == '"') {
            std::string tmp;
            return parse_string(tmp);
        }
        if (*p == '{') {
            ++p;
            if (take('}'))
                return true;
            while (p < end) {
                std::string k;
                if (!parse_string(k) || !take(':') || !skip_value(depth + 1))
                    return false;
                if (take(','))
                    continue;
                return take('}');
            }
            return false;
        }
        if (*p == '[') {
            ++p;
            if (take(']'))
                return true;
            while (p < end) {
                if (!skip_value(depth + 1))
                    return false;
                if (take(','))
                    continue;
                return take(']');
            }
            return false;
        }
        if (*p == 't' && end - p >= 4 && std::memcmp(p, "true", 4) == 0) {
            p += 4;
            return true;
        }
        if (*p == 'f' && end - p >= 5 && std::memcmp(p, "false", 5) == 0) {
            p += 5;
            return true;
        }
        if (*p == 'n' && end - p >= 4 && std::memcmp(p, "null", 4) == 0) {
            p += 4;
            return true;
        }
        std::int64_t num = 0;
        return parse_int(num);
    }
};

} // namespace

std::string sanitize_ascii(std::string_view src, std::size_t max_len)
{
    std::string out;
    out.reserve((std::min)(src.size(), max_len));
    std::size_t i = 0;
    while (i < src.size() && out.size() < max_len) {
        const unsigned char ch = static_cast<unsigned char>(src[i]);
        if (ch == 0xE2u && i + 2 < src.size() && static_cast<unsigned char>(src[i + 1]) == 0x80u) {
            const unsigned char b2 = static_cast<unsigned char>(src[i + 2]);
            if (b2 == 0x98u || b2 == 0x99u) {
                out.push_back('\'');
                i += 3;
                continue;
            }
            if (b2 == 0x9Cu || b2 == 0x9Du) {
                out.push_back('"');
                i += 3;
                continue;
            }
            if (b2 == 0x93u || b2 == 0x94u) {
                out.push_back('-');
                i += 3;
                continue;
            }
            if (b2 == 0xA6u) {
                for (int k = 0; k < 3 && out.size() < max_len; ++k)
                    out.push_back('.');
                i += 3;
                continue;
            }
        }
        if (ch < 0x20u || ch >= 0x7Fu) {
            if (out.empty() || out.back() != ' ')
                out.push_back(' ');
            ++i;
            while (i < src.size() && (static_cast<unsigned char>(src[i]) & 0xC0u) == 0x80u)
                ++i;
            continue;
        }
        out.push_back(static_cast<char>(ch));
        ++i;
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

std::string compact_action_row_name(std::string_view full, std::size_t max_chars)
{
    if (full.empty() || max_chars < 8)
        return std::string(full);
    if (full.size() <= max_chars)
        return std::string(full);

    // Preserve tier suffixes such as ": Ichi", ": Ni", ": San", ": II", ": III".
    const std::size_t suffix = full.rfind(": ");
    if (suffix != std::string_view::npos && (full.size() - suffix) < max_chars / 2) {
        const std::size_t tail = full.size() - suffix;
        const std::size_t reserved = tail < 6 ? 6 : tail;
        if (max_chars > reserved + 4) {
            std::size_t end = max_chars - reserved - 1;
            while (end > 0 && full[end - 1] == ' ')
                --end;
            if (end >= 3) {
                std::string out(full.substr(0, end));
                out.push_back('.');
                out.append(full.substr(suffix));
                return out;
            }
        }
    }
    std::string out(full.substr(0, max_chars - 3));
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    out.append("...");
    return out;
}

std::string format_action_description(std::string_view full, std::size_t max_chars)
{
    if (full.empty() || max_chars < 16 || full.size() <= max_chars)
        return std::string(full);
    std::size_t cut = max_chars - 3;
    std::size_t word_cut = cut;
    while (word_cut > max_chars / 2 && full[word_cut] != ' ')
        --word_cut;
    if (word_cut > max_chars / 2)
        cut = word_cut;
    while (cut > 0 && full[cut - 1] == ' ')
        --cut;
    std::string out(full.substr(0, cut));
    out.append("...");
    return out;
}

std::uint64_t EngineState::make_action_key(std::uint8_t job, std::uint8_t branch, ActionKind kind, std::uint16_t id) noexcept
{
    return (static_cast<std::uint64_t>(job) << 40) |
           (static_cast<std::uint64_t>(branch) << 32) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(kind)) << 16) |
           static_cast<std::uint64_t>(id);
}

void EngineState::reset_all()
{
    std::lock_guard lock(mutex_);
    identities_ = {};
    key_to_job_branch_.clear();
    registry_soul_count_ = 0;
    registry_hash_.clear();
    registry_source_ = "unloaded";
    active_ = {};
    actions_.clear();
    action_generation_ = 0;
    visibility_.clear();
    weapon_rules_.clear();
    weapon_config_hash_.clear();
    hud_ = {};
    mobs_.clear();
    packets_1ff_ = packets_1fc_ = packets_1fd_ = packets_1fe_ = 0;
    packets_oxhud_ = packets_oxmob_ = packets_oxtal_ = suppressed_chat_frames_ = 0;
    hooks_.clear();
    capabilities_.store(0, std::memory_order_release);
    pending_core_opens_.store(0, std::memory_order_release);
}

void EngineState::reset_session()
{
    std::lock_guard lock(mutex_);
    active_ = {};
    hud_.soul_key.clear();
    hud_.kv.clear();
    hud_.updated_ms = 0;
    mobs_.clear();
}

bool EngineState::load_identities_json(std::string_view json_text, std::string_view source_label)
{
    if (json_text.empty() || json_text.size() > 256 * 1024)
        return false;
    JsonScan s{json_text.data(), json_text.data() + json_text.size()};
    if (!s.take('{'))
        return false;

    std::array<std::array<SoulIdentityEntry, 4>, 23> next_ident{};
    std::unordered_map<std::string, std::uint16_t> next_keys;
    std::string next_hash;
    std::uint32_t count = 0;

    if (!s.peek('}')) {
        while (true) {
            std::string field;
            if (!s.parse_string(field) || !s.take(':'))
                return false;
            if (field == "registryHash") {
                if (!s.parse_string(next_hash))
                    return false;
            } else if (field == "souls") {
                if (!s.take('['))
                    return false;
                if (!s.peek(']')) {
                    while (true) {
                        if (!s.take('{'))
                            return false;
                        std::int64_t job = 0, branch = 0;
                        std::string key, name, abbr;
                        if (!s.peek('}')) {
                            while (true) {
                                std::string k;
                                if (!s.parse_string(k) || !s.take(':'))
                                    return false;
                                if (k == "jobId" || k == "job") {
                                    if (!s.parse_int(job)) return false;
                                } else if (k == "branchId" || k == "branch") {
                                    if (!s.parse_int(branch)) return false;
                                } else if (k == "key" || k == "soulKey" || k == "id") {
                                    if (!s.parse_string(key)) return false;
                                } else if (k == "name" || k == "displayName") {
                                    if (!s.parse_string(name)) return false;
                                } else if (k == "abbr" || k == "shortName" || k == "abbreviation") {
                                    if (!s.parse_string(abbr)) return false;
                                } else {
                                    if (!s.skip_value()) return false;
                                }
                                if (s.take(','))
                                    continue;
                                if (!s.take('}'))
                                    return false;
                                break;
                            }
                        } else {
                            s.take('}');
                        }
                        if (job >= 1 && job <= 22 && branch >= 1 && branch <= 3 && !name.empty() && !abbr.empty()) {
                            auto& slot = next_ident[static_cast<std::size_t>(job)][static_cast<std::size_t>(branch)];
                            if (!slot.present)
                                ++count;
                            slot.present = true;
                            slot.name = sanitize_ascii(name, 31);
                            slot.abbr = sanitize_ascii(abbr, 3);
                            slot.key = key.empty() ? normalize_soul_key(slot.name) : normalize_soul_key(key);
                            if (!slot.key.empty()) {
                                next_keys[slot.key] = static_cast<std::uint16_t>((job << 8) | branch);
                            }
                            const std::string norm_name = normalize_soul_key(slot.name);
                            if (!norm_name.empty()) {
                                next_keys[norm_name] = static_cast<std::uint16_t>((job << 8) | branch);
                            }
                        }
                        if (s.take(','))
                            continue;
                        if (!s.take(']'))
                            return false;
                        break;
                    }
                } else {
                    s.take(']');
                }
            } else {
                if (!s.skip_value())
                    return false;
            }
            if (s.take(','))
                continue;
            if (!s.take('}'))
                return false;
            break;
        }
    }
    if (count == 0)
        return false;

    std::lock_guard lock(mutex_);
    identities_ = std::move(next_ident);
    key_to_job_branch_ = std::move(next_keys);
    registry_soul_count_ = count;
    registry_hash_ = std::move(next_hash);
    registry_source_ = std::string(source_label);
    return true;
}

bool EngineState::load_weapon_config_json(std::string_view json_text)
{
    if (json_text.empty() || json_text.size() > 512 * 1024)
        return false;
    JsonScan s{json_text.data(), json_text.data() + json_text.size()};
    if (!s.take('{'))
        return false;

    std::vector<WeaponRuleRecord> next_rules;
    std::string next_hash;

    if (!s.peek('}')) {
        while (true) {
            std::string field;
            if (!s.parse_string(field) || !s.take(':'))
                return false;
            if (field == "weaponConfigHash" || field == "configHash" || field == "hash") {
                if (!s.parse_string(next_hash))
                    return false;
            } else if (field == "rules" || field == "nativeRules" || field == "weaponRules") {
                if (!s.take('['))
                    return false;
                if (!s.peek(']')) {
                    while (true) {
                        if (!s.take('{'))
                            return false;
                        WeaponRuleRecord rec{};
                        std::int64_t job = 0, branch = 0, min_lv = 10, mode_i = 0;
                        std::int64_t main_mask = 0, sub_mask = 0, main_id = 0, sub_id = 0;
                        std::string mode_str;
                        if (!s.peek('}')) {
                            while (true) {
                                std::string k;
                                if (!s.parse_string(k) || !s.take(':'))
                                    return false;
                                if (k == "jobId" || k == "job") {
                                    if (!s.parse_int(job)) return false;
                                } else if (k == "branchId" || k == "branch") {
                                    if (!s.parse_int(branch)) return false;
                                } else if (k == "minEffectiveLevel" || k == "minLevel") {
                                    if (!s.parse_int(min_lv)) return false;
                                } else if (k == "mode") {
                                    s.ws();
                                    if (s.p < s.end && *s.p == '"') {
                                        if (!s.parse_string(mode_str)) return false;
                                    } else {
                                        if (!s.parse_int(mode_i)) return false;
                                    }
                                } else if (k == "mainSkillMask") {
                                    if (!s.parse_int(main_mask)) return false;
                                } else if (k == "subSkillMask") {
                                    if (!s.parse_int(sub_mask)) return false;
                                } else if (k == "mainItemId") {
                                    if (!s.parse_int(main_id)) return false;
                                } else if (k == "subItemId") {
                                    if (!s.parse_int(sub_id)) return false;
                                } else {
                                    if (!s.skip_value()) return false;
                                }
                                if (s.take(','))
                                    continue;
                                if (!s.take('}'))
                                    return false;
                                break;
                            }
                        } else {
                            s.take('}');
                        }

                        if (!mode_str.empty()) {
                            const std::string ml = to_lower_ascii(mode_str);
                            if (ml == "dual_2h" || ml == "dual2h") mode_i = 2;
                            else if (ml == "twohand_shield" || ml == "2h_shield") mode_i = 3;
                            else mode_i = 1;
                        }
                        if (job >= 1 && job <= 22 && branch >= 1 && branch <= 3 &&
                            min_lv >= 1 && min_lv <= 99 && mode_i >= 1 && mode_i <= 3 &&
                            main_mask != 0 && sub_mask != 0) {
                            rec.job = static_cast<std::uint8_t>(job);
                            rec.branch = static_cast<std::uint8_t>(branch);
                            rec.min_effective_level = static_cast<std::uint8_t>(min_lv);
                            rec.mode = static_cast<WeaponRuleMode>(mode_i);
                            rec.main_skill_mask = static_cast<std::uint32_t>(main_mask);
                            rec.sub_skill_mask = static_cast<std::uint32_t>(sub_mask);
                            rec.main_item_id = static_cast<std::uint16_t>(main_id);
                            rec.sub_item_id = static_cast<std::uint16_t>(sub_id);
                            next_rules.push_back(rec);
                        }
                        if (s.take(','))
                            continue;
                        if (!s.take(']'))
                            return false;
                        break;
                    }
                } else {
                    s.take(']');
                }
            } else {
                if (!s.skip_value())
                    return false;
            }
            if (s.take(','))
                continue;
            if (!s.take('}'))
                return false;
            break;
        }
    }

    std::lock_guard lock(mutex_);
    weapon_rules_ = std::move(next_rules);
    if (!next_hash.empty())
        weapon_config_hash_ = std::move(next_hash);
    return true;
}

bool EngineState::resolve_label(std::uint8_t job, std::uint8_t branch, bool full, std::string& out) const
{
    if (job < 1 || job > 22 || branch < 1 || branch > 3)
        return false;
    std::lock_guard lock(mutex_);
    const auto& entry = identities_[job][branch];
    if (!entry.present)
        return false;
    out = full ? entry.name : entry.abbr;
    return !out.empty();
}

bool EngineState::soul_key_to_pair(std::string_view key, std::uint8_t& job, std::uint8_t& branch) const
{
    const std::string norm = normalize_soul_key(key);
    if (norm.empty())
        return false;
    std::lock_guard lock(mutex_);
    const auto it = key_to_job_branch_.find(norm);
    if (it == key_to_job_branch_.end())
        return false;
    job = static_cast<std::uint8_t>(it->second >> 8);
    branch = static_cast<std::uint8_t>(it->second & 0xFFu);
    return true;
}

bool EngineState::pair_to_soul_key(std::uint8_t job, std::uint8_t branch, std::string& out_key) const
{
    if (job < 1 || job > 22 || branch < 1 || branch > 3)
        return false;
    std::lock_guard lock(mutex_);
    const auto& entry = identities_[job][branch];
    if (!entry.present || entry.key.empty())
        return false;
    out_key = entry.key;
    return true;
}

void EngineState::set_active_soul(std::uint32_t char_id, std::uint8_t job, std::uint8_t branch,
                                  std::uint8_t effective_level, bool known, std::string_view source,
                                  std::uint64_t now_ms)
{
    std::lock_guard lock(mutex_);
    if (char_id != 0)
        active_.character_id = char_id;
    if (job >= 1 && job <= 22)
        active_.job = job;
    active_.branch = (branch <= 3) ? branch : 0;
    if (effective_level > 0 && effective_level <= 99)
        active_.effective_level = effective_level;
    active_.known = known;
    active_.source = std::string(source);
    active_.updated_ms = now_ms;
}

void EngineState::update_live_job_level(std::uint8_t job, std::uint8_t effective_level)
{
    std::lock_guard lock(mutex_);
    if (job >= 1 && job <= 22) {
        if (active_.job != 0 && active_.job != job) {
            // Job changed: clear stale Soul branch until server confirms new Soul.
            active_.branch = 0;
            active_.known = false;
        }
        active_.job = job;
    }
    if (effective_level > 0 && effective_level <= 99)
        active_.effective_level = effective_level;
}

void EngineState::clear_active_soul(std::string_view reason)
{
    std::lock_guard lock(mutex_);
    active_.branch = 0;
    active_.known = false;
    active_.source = std::string(reason);
}

ActiveSoul EngineState::active_soul() const
{
    std::lock_guard lock(mutex_);
    return active_;
}

std::uint32_t EngineState::replace_actions(std::vector<ActionRecord> records)
{
    std::unordered_map<std::uint64_t, ActionRecord> next;
    next.reserve(records.size());
    for (auto& rec : records) {
        if (rec.job < 1 || rec.job > 22 || rec.branch < 1 || rec.branch > 3)
            continue;
        rec.name = sanitize_ascii(rec.name, 79);
        rec.description = sanitize_ascii(rec.description, 191);
        if (rec.name.empty() && rec.description.empty() && rec.icon_id == 0xFFFFu && rec.icon_id_v2 == 0xFFFFu)
            continue;
        const std::uint64_t k = make_action_key(rec.job, rec.branch, rec.kind, rec.action_id);
        next[k] = std::move(rec);
    }
    std::lock_guard lock(mutex_);
    actions_ = std::move(next);
    ++action_generation_;
    return static_cast<std::uint32_t>(actions_.size());
}

bool EngineState::upsert_action(ActionRecord rec)
{
    if (rec.job < 1 || rec.job > 22 || rec.branch < 1 || rec.branch > 3)
        return false;
    rec.name = sanitize_ascii(rec.name, 79);
    rec.description = sanitize_ascii(rec.description, 191);
    if (rec.name.empty() && rec.description.empty() && rec.icon_id == 0xFFFFu && rec.icon_id_v2 == 0xFFFFu)
        return false;
    const std::uint64_t k = make_action_key(rec.job, rec.branch, rec.kind, rec.action_id);
    std::lock_guard lock(mutex_);
    actions_[k] = std::move(rec);
    ++action_generation_;
    return true;
}

void EngineState::clear_actions()
{
    std::lock_guard lock(mutex_);
    actions_.clear();
    ++action_generation_;
}

bool EngineState::set_action_row_limit(std::uint32_t limit)
{
    if (limit < 8 || limit > 48)
        return false;
    action_row_limit_.store(limit, std::memory_order_relaxed);
    return true;
}

bool EngineState::lookup_action(ActionKind kind, std::uint16_t action_id, ActionRecord& out) const
{
    std::lock_guard lock(mutex_);
    if (!active_.known || active_.job < 1 || active_.job > 22 || active_.branch < 1 || active_.branch > 3)
        return false;
    const auto it = actions_.find(make_action_key(active_.job, active_.branch, kind, action_id));
    if (it == actions_.end())
        return false;
    out = it->second;
    return true;
}

std::uint32_t EngineState::replace_visibility(std::vector<VisibilityRecord> records)
{
    std::unordered_map<std::uint64_t, VisibilityRecord> next;
    next.reserve(records.size());
    for (const auto& rec : records) {
        if (rec.job < 1 || rec.job > 22 || rec.branch < 1 || rec.branch > 3)
            continue;
        next[make_action_key(rec.job, rec.branch, rec.kind, rec.action_id)] = rec;
    }
    std::lock_guard lock(mutex_);
    visibility_ = std::move(next);
    return static_cast<std::uint32_t>(visibility_.size());
}

void EngineState::clear_visibility()
{
    std::lock_guard lock(mutex_);
    visibility_.clear();
}

VisibilityDecision EngineState::decide_visibility(ActionKind kind, std::uint16_t action_id, std::uint8_t level_override) const
{
    std::lock_guard lock(mutex_);
    if (!active_.known || active_.job < 1 || active_.job > 22 || active_.branch < 1 || active_.branch > 3)
        return VisibilityDecision::Unmanaged;
    const std::uint8_t level = level_override > 0 ? level_override : active_.effective_level;
    if (level == 0)
        return VisibilityDecision::Unmanaged;
    const auto it = visibility_.find(make_action_key(active_.job, active_.branch, kind, action_id));
    if (it == visibility_.end())
        return VisibilityDecision::Unmanaged;
    const auto& rule = it->second;
    if (rule.action == VisibilityRuleAction::Hide) {
        return level >= rule.min_effective_level ? VisibilityDecision::Hide : VisibilityDecision::Unmanaged;
    }
    return level >= rule.min_effective_level ? VisibilityDecision::Show : VisibilityDecision::Hide;
}

bool EngineState::allows_sub_weapon(std::uint16_t main_skill, std::uint16_t main_item_id,
                                    std::uint16_t sub_skill, std::uint16_t sub_item_id) const
{
    if (main_skill == 0 || main_skill >= 32 || sub_skill == 0 || sub_skill >= 32)
        return false;
    std::lock_guard lock(mutex_);
    if (!active_.known || active_.job < 1 || active_.branch < 1 || active_.effective_level == 0)
        return false;
    const std::uint32_t mbit = 1u << main_skill;
    const std::uint32_t sbit = 1u << sub_skill;
    for (const auto& r : weapon_rules_) {
        if (r.job != active_.job || r.branch != active_.branch) continue;
        if (active_.effective_level < r.min_effective_level) continue;
        if (r.mode != WeaponRuleMode::Dual1H && r.mode != WeaponRuleMode::Dual2H) continue;
        if ((r.main_skill_mask & mbit) == 0 || (r.sub_skill_mask & sbit) == 0) continue;
        if (r.main_item_id != 0 && r.main_item_id != main_item_id) continue;
        if (r.sub_item_id != 0 && r.sub_item_id != sub_item_id) continue;
        return true;
    }
    return false;
}

bool EngineState::allows_dual_2h_menu_item(std::uint16_t sub_skill, std::uint16_t sub_item_id) const
{
    if (sub_skill == 0 || sub_skill >= 32)
        return false;
    std::lock_guard lock(mutex_);
    if (!active_.known || active_.job < 1 || active_.branch < 1 || active_.effective_level == 0)
        return false;
    const std::uint32_t sbit = 1u << sub_skill;
    for (const auto& r : weapon_rules_) {
        if (r.job != active_.job || r.branch != active_.branch) continue;
        if (active_.effective_level < r.min_effective_level) continue;
        if (r.mode != WeaponRuleMode::Dual2H) continue;
        if ((r.sub_skill_mask & sbit) == 0) continue;
        if (r.sub_item_id != 0 && r.sub_item_id != sub_item_id) continue;
        return true;
    }
    return false;
}

bool EngineState::allows_2h_main_with_sub(std::uint16_t main_skill, std::uint16_t main_item_id,
                                          std::uint16_t sub_skill, std::uint16_t sub_item_id) const
{
    if (main_skill == 0 || main_skill >= 32 || sub_skill == 0 || sub_skill >= 32)
        return false;
    std::lock_guard lock(mutex_);
    if (!active_.known || active_.job < 1 || active_.branch < 1 || active_.effective_level == 0)
        return false;
    const std::uint32_t mbit = 1u << main_skill;
    const std::uint32_t sbit = 1u << sub_skill;
    for (const auto& r : weapon_rules_) {
        if (r.job != active_.job || r.branch != active_.branch) continue;
        if (active_.effective_level < r.min_effective_level) continue;
        if (r.mode != WeaponRuleMode::Dual2H && r.mode != WeaponRuleMode::TwoHandShield) continue;
        if ((r.main_skill_mask & mbit) == 0 || (r.sub_skill_mask & sbit) == 0) continue;
        if (r.main_item_id != 0 && r.main_item_id != main_item_id) continue;
        if (r.sub_item_id != 0 && r.sub_item_id != sub_item_id) continue;
        return true;
    }
    return false;
}

bool EngineState::observe_oxhud_frame(std::string_view payload, std::uint64_t now_ms)
{
    if (payload.empty())
        return false;
    std::size_t pos = payload.find('|');
    const std::string soul = std::string(pos == std::string_view::npos ? payload : payload.substr(0, pos));
    if (soul.empty())
        return false;

    std::unordered_map<std::string, std::string> kv;
    while (pos != std::string_view::npos && pos + 1 < payload.size()) {
        const std::size_t next = payload.find('|', pos + 1);
        const std::string_view tok = (next == std::string_view::npos)
            ? payload.substr(pos + 1)
            : payload.substr(pos + 1, next - (pos + 1));
        const std::size_t eq = tok.find('=');
        if (eq != std::string_view::npos && eq > 0) {
            kv[std::string(tok.substr(0, eq))] = std::string(tok.substr(eq + 1));
        }
        pos = next;
    }

    std::lock_guard lock(mutex_);
    ++packets_oxhud_;
    hud_.soul_key = soul;
    hud_.kv = std::move(kv);
    hud_.updated_ms = now_ms;
    hud_.packet_count = packets_oxhud_;
    // If active Soul wasn't yet confirmed by 0x1FF/0x1FC, infer pair from registry safely.
    if (!active_.known) {
        const auto it = key_to_job_branch_.find(normalize_soul_key(soul));
        if (it != key_to_job_branch_.end()) {
            const std::uint8_t j = static_cast<std::uint8_t>(it->second >> 8);
            const std::uint8_t b = static_cast<std::uint8_t>(it->second & 0xFFu);
            if (active_.job == 0 || active_.job == j) {
                active_.job = j;
                active_.branch = b;
                active_.known = true;
                active_.source = "oxhud";
                active_.updated_ms = now_ms;
            }
        }
    }
    return true;
}

bool EngineState::observe_oxbst_frame(std::string_view payload, std::uint64_t now_ms)
{
    std::lock_guard lock(mutex_);
    ++hud_.boost_packet_count;
    hud_.last_boost_payload = std::string(payload);
    hud_.updated_ms = now_ms;
    return true;
}

bool EngineState::observe_oxmob_frame(std::string_view payload, std::uint64_t now_ms)
{
    if (payload.empty())
        return false;
    const std::size_t p1 = payload.find('|');
    if (p1 == std::string_view::npos || p1 == 0)
        return false;
    std::uint32_t server_id = 0;
    const auto res = std::from_chars(payload.data(), payload.data() + p1, server_id);
    if (res.ec != std::errc{})
        return false;

    std::lock_guard lock(mutex_);
    ++packets_oxmob_;
    if (server_id == 0)
        return true;
    auto& entry = mobs_[server_id];
    entry.server_id = server_id;
    entry.payload = std::string(payload.substr(p1 + 1));
    entry.updated_ms = now_ms;
    ++entry.packet_count;
    if (mobs_.size() > 256) {
        // Bound memory cleanly by pruning the oldest record.
        auto oldest = mobs_.begin();
        for (auto it = mobs_.begin(); it != mobs_.end(); ++it) {
            if (it->second.updated_ms < oldest->second.updated_ms)
                oldest = it;
        }
        if (oldest != mobs_.end() && oldest->first != server_id)
            mobs_.erase(oldest);
    }
    return true;
}

void EngineState::note_packet_1ff() { std::lock_guard lock(mutex_); ++packets_1ff_; }
void EngineState::note_packet_1fc() { std::lock_guard lock(mutex_); ++packets_1fc_; }
void EngineState::note_packet_1fd() { std::lock_guard lock(mutex_); ++packets_1fd_; }
void EngineState::note_packet_1fe() { std::lock_guard lock(mutex_); ++packets_1fe_; }
void EngineState::note_oxtal_frame() { std::lock_guard lock(mutex_); ++packets_oxtal_; }
void EngineState::note_suppressed_chat_frame() { std::lock_guard lock(mutex_); ++suppressed_chat_frames_; }

HudSnapshot EngineState::hud_snapshot() const
{
    std::lock_guard lock(mutex_);
    return hud_;
}

bool EngineState::mob_status(std::uint32_t server_id, MobStatusSnapshot& out) const
{
    std::lock_guard lock(mutex_);
    const auto it = mobs_.find(server_id);
    if (it == mobs_.end())
        return false;
    out = it->second;
    return true;
}

std::uint32_t EngineState::tracked_mob_count() const
{
    std::lock_guard lock(mutex_);
    return static_cast<std::uint32_t>(mobs_.size());
}

void EngineState::set_hook_statuses(std::vector<HookStatus> hooks, std::uint32_t caps)
{
    {
        std::lock_guard lock(mutex_);
        hooks_ = std::move(hooks);
    }
    capabilities_.store(caps, std::memory_order_release);
}

void EngineState::increment_hook_hit(std::string_view id)
{
    std::lock_guard lock(mutex_);
    for (auto& h : hooks_) {
        if (h.id == id) {
            ++h.hit_count;
            return;
        }
    }
}

DiagnosticsSnapshot EngineState::diagnostics() const
{
    DiagnosticsSnapshot snap{};
    snap.enabled = enabled();
    snap.api_version = kNativeApiVersion;
    snap.capabilities = capabilities();
    snap.action_row_limit = action_row_limit();
    std::lock_guard lock(mutex_);
    snap.registry_soul_count = registry_soul_count_;
    snap.registry_hash = registry_hash_;
    snap.registry_source = registry_source_;
    snap.weapon_config_hash = weapon_config_hash_;
    snap.action_count = static_cast<std::uint32_t>(actions_.size());
    snap.action_generation = action_generation_;
    snap.visibility_count = static_cast<std::uint32_t>(visibility_.size());
    snap.weapon_rule_count = static_cast<std::uint32_t>(weapon_rules_.size());
    snap.packets_1ff = packets_1ff_;
    snap.packets_1fc = packets_1fc_;
    snap.packets_1fd = packets_1fd_;
    snap.packets_1fe = packets_1fe_;
    snap.packets_oxhud = packets_oxhud_;
    snap.packets_oxmob = packets_oxmob_;
    snap.packets_oxtal = packets_oxtal_;
    snap.suppressed_chat_frames = suppressed_chat_frames_;
    snap.active_soul = active_;
    snap.hooks = hooks_;
    snap.hooks_total = static_cast<std::uint32_t>(hooks_.size());
    for (const auto& h : hooks_) {
        if (h.installed)
            ++snap.hooks_installed;
    }
    return snap;
}

EngineState& engine()
{
    static EngineState instance;
    return instance;
}

} // namespace vanacore::soulspire
