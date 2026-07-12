// usage: fixspec-gen <spec.xml>... -d <out-dir>
// Emits detail/fields.hpp, names.hpp into <out-dir>.
// Reads QuickFIX-format XML; FIX 5.0 needs two (FIX50SP2.xml + FIXT11.xml).
// Dups merge; conflicts emit a warning on stderr (name clash -> alias,
// type/delimiter mismatch -> first wins).

#include <pugixml.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

struct Field {
    int tag = 0;
    std::string name;
    std::string type;
    std::vector<std::string> aliases;
    std::vector<std::pair<std::string, std::string>> values;  // (enum, description)
};

struct MessageType {
    std::string name;
    std::string value;
    std::vector<std::string> aliases;
};

std::string right_pad(std::size_t width, char pad, std::string_view text) {
    std::string out(text);
    if (out.size() < width)
        out.append(width - out.size(), pad);
    return out;
}

// Escape for a C++ string literal (enum descriptions can carry punctuation).
std::string c_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\' || c == '"')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// QuickFIX uppercases its type names; map to FIX spec CamelCase.
std::string normalize_type(std::string_view qf) {
    static std::map<std::string_view, char const*> const table = {
        {"INT", "int"},
        {"LENGTH", "Length"},
        {"NUMINGROUP", "NumInGroup"},
        {"SEQNUM", "SeqNum"},
        {"TAGNUM", "TagNum"},
        {"FLOAT", "float"},
        {"QTY", "Qty"},
        {"PRICE", "Price"},
        {"PRICEOFFSET", "PriceOffset"},
        {"AMT", "Amt"},
        {"PERCENTAGE", "Percentage"},
        {"CHAR", "char"},
        {"BOOLEAN", "Boolean"},
        {"STRING", "String"},
        {"MULTIPLECHARVALUE", "MultipleCharValue"},
        {"MULTIPLESTRINGVALUE", "MultipleStringValue"},
        {"COUNTRY", "Country"},
        {"CURRENCY", "Currency"},
        {"EXCHANGE", "Exchange"},
        {"MONTHYEAR", "MonthYear"},
        {"UTCTIMESTAMP", "UTCTimestamp"},
        {"UTCTIMEONLY", "UTCTimeOnly"},
        {"UTCDATEONLY", "UTCDateOnly"},
        {"LOCALMKTDATE", "LocalMktDate"},
        {"LOCALMKTTIME", "LocalMktTime"},
        {"TZTIMEONLY", "TZTimeOnly"},
        {"TZTIMESTAMP", "TZTimestamp"},
        {"DATA", "data"},
        {"XMLDATA", "XMLData"},
        {"LANGUAGE", "Language"},
        {"XID", "XID"},
        {"XIDREF", "XIDREF"},
    };
    if (auto it = table.find(qf); it != table.end())
        return it->second;
    return std::string(qf);
}

// Map a normalized FIX type to the nanofix::fix_type enumerator name. Unmapped
// (custom/venue) types fall through to "unknown", which enables every accessor.
char const* category(std::string const& type) {
    static std::map<std::string, char const*> const table = {
        {"float", "decimal"},
        {"Qty", "decimal"},
        {"Price", "decimal"},
        {"PriceOffset", "decimal"},
        {"Amt", "decimal"},
        {"Percentage", "decimal"},
        {"int", "integer"},
        {"Length", "integer"},
        {"NumInGroup", "integer"},
        {"SeqNum", "integer"},
        {"TagNum", "integer"},
        {"char", "character"},
        {"Boolean", "character"},
        {"MultipleCharValue", "string"},
        {"UTCTimestamp", "timestamp"},
        {"UTCTimeOnly", "timeonly"},
        {"UTCDateOnly", "date"},
        {"String", "string"},
        {"MultipleStringValue", "string"},
        {"Country", "string"},
        {"Currency", "string"},
        {"Exchange", "string"},
        {"MonthYear", "monthyear"},
        {"LocalMktDate", "date"},
        {"LocalMktTime", "timeonly"},
        {"TZTimeOnly", "string"},
        {"TZTimestamp", "string"},
        {"data", "string"},
        {"XMLData", "string"},
        {"Language", "string"},
        {"XID", "string"},
        {"XIDREF", "string"},
    };
    if (auto it = table.find(type); it != table.end())
        return it->second;
    return "unknown";
}

void load_fields(pugi::xml_document const& doc, char const* source, std::map<int, Field>& out) {
    for (auto f : doc.child("fix").child("fields").children("field")) {
        char const* num = f.attribute("number").value();
        char const* name = f.attribute("name").value();
        char const* type = f.attribute("type").value();
        if (!num[0] || !name[0] || !type[0])
            continue;
        int tag = static_cast<int>(std::strtol(num, nullptr, 10));
        if (tag <= 0)
            continue;
        std::string normalized = normalize_type(type);
        auto [it, inserted] = out.try_emplace(tag, Field{tag, name, normalized, {}, {}});
        if (inserted) {
            for (auto v : f.children("value")) {
                char const* en = v.attribute("enum").value();
                char const* desc = v.attribute("description").value();
                if (en[0] && desc[0])
                    it->second.values.emplace_back(en, desc);
            }
            continue;
        }
        Field& existing = it->second;
        if (existing.name != name &&
            std::find(existing.aliases.begin(), existing.aliases.end(), name) ==
                existing.aliases.end()) {
            std::cerr << "warning: tag " << tag << " has alias " << name << " (primary "
                      << existing.name << ", in " << source << ")\n";
            existing.aliases.emplace_back(name);
        }
        if (existing.type != normalized) {
            std::cerr << "warning: tag " << tag << " (" << existing.name << ") type "
                      << existing.type << " vs " << normalized << " in " << source << ", keeping "
                      << existing.type << "\n";
        }
    }
}

void load_messages(pugi::xml_document const& doc,
                   char const* source,
                   std::vector<MessageType>& out,
                   std::unordered_map<std::string, std::size_t>& seen_by_value) {
    for (auto m : doc.child("fix").child("messages").children("message")) {
        char const* name = m.attribute("name").value();
        char const* value = m.attribute("msgtype").value();
        if (!name[0] || !value[0])
            continue;
        if (auto it = seen_by_value.find(value); it != seen_by_value.end()) {
            MessageType& existing = out[it->second];
            if (existing.name != name &&
                std::find(existing.aliases.begin(), existing.aliases.end(), name) ==
                    existing.aliases.end()) {
                std::cerr << "warning: msgtype '" << value << "' has alias " << name << " (primary "
                          << existing.name << ", in " << source << ")\n";
                existing.aliases.emplace_back(name);
            }
            continue;
        }
        seen_by_value.emplace(value, out.size());
        out.push_back(MessageType{name, value, {}});
    }
}

bool is_data_length(Field const& f) {
    if (f.type != "Length")
        return false;
    // 9 = BodyLength (frame), 383 = MaxMessageSize (session limit); see upstream #44.
    if (f.tag == 9 || f.tag == 383)
        return false;
    return true;
}

void emit_fields(std::ostream& os,
                 std::map<int, Field> const& fields,
                 std::vector<MessageType> const& messages) {
    os << "// Generated by utils/fixspec-gen. Do not edit.\n"
       << "\n"
       << "#pragma once\n"
       << "\n"
       << "#include <cstdint>\n"
       << "\n"
       << "#include <nanofix/detail/typed.hpp>\n"
       << "\n"
       << "namespace nanofix {\n"
       << "\n"
       << "// Each tag is a typed field_tag handle: it converts to its int number\n"
       << "// (operator int()) for writer/compare/group call sites, and selects the\n"
       << "// typed find()/find_with_hint() overloads for compile-time-checked reads.\n"
       << "namespace tag {\n";

    for (auto const& [_, f] : fields) {
        os << "inline constexpr field_tag<" << f.tag << ", fix_type::" << category(f.type) << "> "
           << right_pad(40, ' ', f.name) << "{};  // " << f.tag << " (" << f.type << ")\n";
        for (auto const& alias : f.aliases) {
            os << "inline constexpr field_tag<" << f.tag << ", fix_type::" << category(f.type)
               << "> " << right_pad(40, ' ', alias) << "{};  // " << f.tag << " alias of " << f.name
               << "\n";
        }
    }

    os << "} // namespace tag\n"
       << "\n"
       << "namespace {\n"
       << "inline constexpr int length_fields[] = {\n";

    std::vector<Field const*> lengths;
    for (auto const& [_, f] : fields) {
        if (is_data_length(f))
            lengths.push_back(&f);
    }
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        Field const& f = *lengths[i];
        bool is_last = i + 1 == lengths.size();
        std::string entry = "tag::" + f.name;
        if (!is_last)
            entry += ",";
        os << right_pad(35, ' ', entry) << " // " << f.tag << "\n";
    }
    os << "};\n"
       << "}\n"
       << "\n";

    // Tag-membership bitmap: one bit per integer in [0, max_tag]. Drives
    // nanofix::is_known_tag() for app-layer whitelist on untrusted feeds.
    int max_tag = 0;
    for (auto const& [t, _] : fields)
        if (t > max_tag)
            max_tag = t;
    std::size_t const bitmap_words = static_cast<std::size_t>(max_tag) / 64u + 1u;
    std::vector<std::uint64_t> bitmap(bitmap_words, 0);
    for (auto const& [t, _] : fields) {
        if (t > 0)
            bitmap[static_cast<std::size_t>(t) / 64u] |= std::uint64_t{1}
                                                         << (static_cast<std::size_t>(t) % 64u);
    }
    os << "namespace detail {\n"
       << "inline constexpr int known_tag_max = " << max_tag << ";\n"
       << "inline constexpr std::uint64_t known_tag_bitmap[" << bitmap_words << "] = {\n";
    for (std::size_t i = 0; i < bitmap.size(); ++i) {
        if (i % 4 == 0)
            os << "    ";
        char buf[24];
        std::snprintf(buf, sizeof(buf), "0x%016llxULL", static_cast<unsigned long long>(bitmap[i]));
        os << buf;
        if (i + 1 < bitmap.size())
            os << ", ";
        if (i % 4 == 3)
            os << "\n";
    }
    if (bitmap.size() % 4 != 0)
        os << "\n";
    os << "};\n"
       << "} // namespace detail\n"
       << "\n"
       << "/// True if `tag` appears in the loaded FIX spec dictionary.\n"
       << "/// O(1) bitmap lookup; intended as app-layer whitelist on untrusted feeds.\n"
       << "[[nodiscard]] inline constexpr bool is_known_tag(int tag) noexcept {\n"
       << "    unsigned const u = static_cast<unsigned>(tag);\n"
       << "    if (u > static_cast<unsigned>(detail::known_tag_max)) return false;\n"
       << "    return (detail::known_tag_bitmap[u >> 6] >> (u & 63u)) & 1u;\n"
       << "}\n"
       << "\n"
       << "namespace msg_type {\n";

    for (auto const& m : messages) {
        os << "static constexpr char const* const " << right_pad(40, ' ', m.name) << " = "
           << "\"" << m.value << "\";\n";
        for (auto const& alias : m.aliases) {
            os << "static constexpr char const* const " << right_pad(40, ' ', alias) << " = "
               << m.name << "; // alias\n";
        }
    }

    os << "} // namespace msg_type\n"
       << "} // namespace nanofix\n";
}

// Opt-in name lookups (field / message-type / enum value) into a separate
// header, so the large value tables never reach a TU that doesn't include it.
std::size_t emit_names(std::ostream& os,
                       std::map<int, Field> const& fields,
                       std::vector<MessageType> const& messages) {
    os << "// Generated by utils/fixspec-gen. Do not edit.\n"
       << "//\n"
       << "// Opt-in human-readable names (logging, tooling). No <string>/<map>/\n"
       << "// <iostream>; every result is a std::string_view into static storage.\n"
       << "\n"
       << "#pragma once\n"
       << "\n"
       << "#include <cstddef>\n"
       << "#include <string_view>\n"
       << "\n"
       << "#include <nanofix/detail/fields.hpp>\n"
       << "\n"
       << "namespace nanofix {\n"
       << "\n"
       << "/// FIX field name for `tag` (e.g. 54 -> \"Side\"); empty if unknown.\n"
       << "[[nodiscard]] constexpr std::string_view field_name(int tag) noexcept {\n"
       << "    switch (tag) {\n";
    for (auto const& [t, f] : fields)
        os << "        case " << t << ": return \"" << f.name << "\";\n";
    os << "        default: return {};\n"
       << "    }\n"
       << "}\n"
       << "\n"
       << "namespace detail {\n"
       << "struct msg_type_name_entry {\n"
       << "    std::string_view value;\n"
       << "    std::string_view name;\n"
       << "};\n"
       << "inline constexpr msg_type_name_entry msg_type_names[] = {\n";
    for (auto const& m : messages)
        os << "    {\"" << c_escape(m.value) << "\", \"" << m.name << "\"},\n";
    os << "};\n"
       << "}  // namespace detail\n"
       << "\n"
       << "/// Long name for a MsgType value (e.g. \"D\" -> \"NewOrderSingle\"); empty if\n"
       << "/// unknown. Linear scan over the message table (cold path: tooling only).\n"
       << "[[nodiscard]] constexpr std::string_view msg_type_name(std::string_view value) noexcept "
          "{\n"
       << "    for (auto const& e : detail::msg_type_names)\n"
       << "        if (e.value == value)\n"
       << "            return e.name;\n"
       << "    return {};\n"
       << "}\n"
       << "\n"
       << "namespace detail {\n"
       << "struct value_name_entry {\n"
       << "    int tag;\n"
       << "    std::string_view value;\n"
       << "    std::string_view name;\n"
       << "};\n"
       << "// Sorted by tag (then spec order within a tag) for the binary search below.\n"
       << "inline constexpr value_name_entry value_names[] = {\n";
    std::size_t total_values = 0;
    for (auto const& [t, f] : fields)
        for (auto const& [en, desc] : f.values) {
            os << "    {" << t << ", \"" << c_escape(en) << "\", \"" << c_escape(desc) << "\"},\n";
            ++total_values;
        }
    if (total_values == 0)  // a non-empty array is simpler for the lookup
        os << "    {0, \"\", \"\"},\n";
    os << "};\n"
       << "}  // namespace detail\n"
       << "\n"
       << "/// Human name for an enum field value (e.g. (54, \"1\") -> \"BUY\"); empty if the\n"
       << "/// tag has no enum or the value is unlisted. Binary-search the tag block,\n"
       << "/// then scan it (cold path: tooling only).\n"
       << "[[nodiscard]] constexpr std::string_view value_name(int tag, std::string_view value) "
          "noexcept {\n"
       << "    constexpr std::size_t n = sizeof(detail::value_names) / "
          "sizeof(detail::value_names[0]);\n"
       << "    std::size_t lo = 0, hi = n;\n"
       << "    while (lo < hi) {\n"
       << "        std::size_t const mid = lo + (hi - lo) / 2;\n"
       << "        if (detail::value_names[mid].tag < tag)\n"
       << "            lo = mid + 1;\n"
       << "        else\n"
       << "            hi = mid;\n"
       << "    }\n"
       << "    for (std::size_t i = lo; i < n && detail::value_names[i].tag == tag; ++i)\n"
       << "        if (detail::value_names[i].value == value)\n"
       << "            return detail::value_names[i].name;\n"
       << "    return {};\n"
       << "}\n"
       << "\n";

    int const max_field_tag = fields.empty() ? 0 : fields.rbegin()->first;
    os << "// Populate a caller-supplied map (e.g. std::map<int, std::string>) from the\n"
       << "// constexpr lookups above, so the name data is not emitted a second time.\n"
       << "template <typename AssociativeContainer>\n"
       << "void dictionary_init_field(AssociativeContainer& dictionary) {\n"
       << "    using Value = typename AssociativeContainer::mapped_type;\n"
       << "    for (int t = 0; t <= " << max_field_tag << "; ++t)\n"
       << "        if (std::string_view const n = field_name(t); !n.empty())\n"
       << "            dictionary[t] = Value(n);\n"
       << "}\n"
       << "\n"
       << "template <typename AssociativeContainer>\n"
       << "void dictionary_init_message(AssociativeContainer& dictionary) {\n"
       << "    using Key = typename AssociativeContainer::key_type;\n"
       << "    using Value = typename AssociativeContainer::mapped_type;\n"
       << "    for (auto const& e : detail::msg_type_names)\n"
       << "        dictionary[Key(e.value)] = Value(e.name);\n"
       << "}\n"
       << "\n"
       << "}  // namespace nanofix\n";
    return total_values;
}

bool load_doc(char const* path, pugi::xml_document& doc) {
    auto r = doc.load_file(path);
    if (!r) {
        std::cerr << "failed to parse " << path << ": " << r.description() << "\n";
        return false;
    }
    return true;
}

}  // namespace

bool open_out(std::string const& dir, char const* name, std::ofstream& out) {
    std::filesystem::path const path = std::filesystem::path(dir) / name;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    out.open(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "cannot open " << path << " for writing\n";
        return false;
    }
    return true;
}

int main(int argc, char** argv) {
    std::vector<char const*> input_paths;
    char const* out_dir = nullptr;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-d" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "unknown argument: " << arg << "\n";
            return 2;
        } else {
            input_paths.push_back(argv[i]);
        }
    }

    if (input_paths.empty() || !out_dir) {
        std::cerr << "usage: " << argv[0] << " <spec.xml> [<spec2.xml> ...] -d <out-dir>\n"
                  << "  Emits detail/fields.hpp, names.hpp into <out-dir>.\n";
        return 2;
    }

    std::vector<pugi::xml_document> docs(input_paths.size());
    for (std::size_t i = 0; i < input_paths.size(); ++i) {
        if (!load_doc(input_paths[i], docs[i]))
            return 1;
    }

    std::map<int, Field> fields;
    std::vector<MessageType> messages;
    std::unordered_map<std::string, std::size_t> seen_msg;
    for (std::size_t i = 0; i < docs.size(); ++i) {
        load_fields(docs[i], input_paths[i], fields);
        load_messages(docs[i], input_paths[i], messages, seen_msg);
    }

    std::string const dir = out_dir;

    {
        std::ofstream out;
        if (!open_out(dir, "detail/fields.hpp", out))
            return 1;
        emit_fields(out, fields, messages);
    }

    {
        std::ofstream out;
        if (!open_out(dir, "names.hpp", out))
            return 1;
        std::size_t const nv = emit_names(out, fields, messages);
        std::cerr << "wrote name tables (" << fields.size() << " fields, " << messages.size()
                  << " msg types, " << nv << " enum values)\n";
    }

    return 0;
}
