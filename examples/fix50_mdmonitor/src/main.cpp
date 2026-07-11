// FIX 5.0 market-data monitor: writes MarketDataSnapshotFullRefresh ('W') and
// MarketDataIncrementalRefresh ('X'), reads them back through the iterator.
//
// Both carry a NoMDEntries group, but its delimiter differs per MsgType —
// snapshot entries start at MDEntryType (269), incremental entries at
// MDUpdateAction (279) — so each goes through the runtime group(count, delim)
// overload. Snapshot entries are read with the entry's forward-scan find();
// incremental entries carry more fields, so they go through a per-entry field
// index with a forward-scan fallback. MDEntryType / MDUpdateAction are decoded
// to spec names via nanofix/names.hpp.

#include <nanofix.hpp>
#include <nanofix/names.hpp>

#include <array>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>

namespace {

[[nodiscard]] int sv_arg(std::string_view s) {
    return static_cast<int>(s.size());
}

// Spec name for an enum value (e.g. MDEntryType "0" -> "BID"); the raw value if
// the tag has no enum or the value is unlisted.
[[nodiscard]] std::string_view decode(int tag, std::string_view value) {
    std::string_view const name = nanofix::value_name(tag, value);
    return name.empty() ? value : name;
}

// One tag as a text view (empty if absent); as_string_view() works for any tag
// category. Fields is anything exposing find(tag::X): group_entry (forward
// scan) or indexed_fields (index lookup).
template <class Fields, int Tag, nanofix::fix_type Ty>
[[nodiscard]] std::string_view field_get(Fields& f, nanofix::field_tag<Tag, Ty> t) {
    auto const v = f.find(t);
    return v.empty() ? std::string_view{} : v.as_string_view();
}

// Tiered per-entry access, mirroring nanofix::with_fields at entry scope:
// index the entry once, then hinted lookups through indexed_fields; an entry
// that overflows the buffer falls back to the entry's forward-scan find.
template <std::size_t N, class Fn>
auto with_entry_fields(nanofix::group_entry const& e, nanofix::field_index_buffer<N>& buf,
                       Fn&& fn) {
    auto const idx = nanofix::build_field_index(e, buf);
    if (!idx.truncated()) {
        nanofix::indexed_fields<N> f(idx);
        return fn(f);
    }
    return fn(e);
}

[[nodiscard]] std::size_t write_snapshot(std::span<char> buf, int seq, std::string_view symbol,
                                         int bid_px, int ask_px) {
    nanofix::message_writer w(buf.data(), buf.data() + buf.size());
    w.push_back_header("FIXT.1.1");
    w.push_back_string(nanofix::tag::MsgType, "W");
    w.push_back_string(nanofix::tag::SenderCompID, "FEED");
    w.push_back_string(nanofix::tag::TargetCompID, "OMS");
    w.push_back_int(nanofix::tag::MsgSeqNum, seq);
    w.push_back_string(nanofix::tag::Symbol, symbol);
    w.push_back_int(nanofix::tag::NoMDEntries, 2);
    w.push_back_char(nanofix::tag::MDEntryType, '0');
    w.push_back_decimal(nanofix::tag::MDEntryPx, bid_px, -2);
    w.push_back_int(nanofix::tag::MDEntrySize, 500);
    w.push_back_char(nanofix::tag::MDEntryType, '1');
    w.push_back_decimal(nanofix::tag::MDEntryPx, ask_px, -2);
    w.push_back_int(nanofix::tag::MDEntrySize, 300);
    if (!w.push_back_trailer())
        return 0;
    return static_cast<std::size_t>(w.message_end() - buf.data());
}

[[nodiscard]] std::size_t write_incremental(std::span<char> buf, int seq) {
    nanofix::message_writer w(buf.data(), buf.data() + buf.size());
    w.push_back_header("FIXT.1.1");
    w.push_back_string(nanofix::tag::MsgType, "X");
    w.push_back_string(nanofix::tag::SenderCompID, "FEED");
    w.push_back_string(nanofix::tag::TargetCompID, "OMS");
    w.push_back_int(nanofix::tag::MsgSeqNum, seq);
    w.push_back_int(nanofix::tag::NoMDEntries, 2);
    w.push_back_char(nanofix::tag::MDUpdateAction, '0');
    w.push_back_char(nanofix::tag::MDEntryType, '1');
    w.push_back_string(nanofix::tag::Symbol, "AAPL");
    w.push_back_decimal(nanofix::tag::MDEntryPx, 19012, -2);
    w.push_back_int(nanofix::tag::MDEntrySize, 120);
    w.push_back_char(nanofix::tag::MDUpdateAction, '2');
    w.push_back_char(nanofix::tag::MDEntryType, '0');
    w.push_back_string(nanofix::tag::Symbol, "MSFT");
    w.push_back_decimal(nanofix::tag::MDEntryPx, 41050, -2);
    w.push_back_int(nanofix::tag::MDEntrySize, 80);
    if (!w.push_back_trailer())
        return 0;
    return static_cast<std::size_t>(w.message_end() - buf.data());
}

void on_snapshot(nanofix::message_reader const& r) {
    auto sit = r.begin();
    auto const symbol =
        r.find_with_hint(nanofix::tag::Symbol, sit) ? sit->value().as_string_view() : std::string_view{};
    std::printf("SNAPSHOT %.*s\n", sv_arg(symbol), symbol.data());

    // Snapshot NoMDEntries entries are delimited by MDEntryType (269). Three
    // fields per entry — forward-scan find, no index.
    r.group(nanofix::tag::NoMDEntries, nanofix::tag::MDEntryType)
        .for_each([](nanofix::group_entry const& e) {
            auto const type = decode(nanofix::tag::MDEntryType, field_get(e, nanofix::tag::MDEntryType));
            auto const px = field_get(e, nanofix::tag::MDEntryPx);
            auto const size = field_get(e, nanofix::tag::MDEntrySize);
            std::printf("    %-6.*s px=%-10.*s size=%.*s\n",
                        sv_arg(type),
                        type.data(),
                        sv_arg(px),
                        px.data(),
                        sv_arg(size),
                        size.data());
        });
}

void on_incremental(nanofix::message_reader const& r) {
    std::printf("INCREMENTAL\n");

    // Incremental NoMDEntries entries are delimited by MDUpdateAction (279).
    // Five fields per entry — worth the per-entry index (see with_entry_fields).
    nanofix::field_index_buffer<16> ebuf;
    r.group(nanofix::tag::NoMDEntries, nanofix::tag::MDUpdateAction)
        .for_each([&ebuf](nanofix::group_entry const& e) {
            with_entry_fields(e, ebuf, [](auto& f) {
                auto const act =
                    decode(nanofix::tag::MDUpdateAction, field_get(f, nanofix::tag::MDUpdateAction));
                auto const type =
                    decode(nanofix::tag::MDEntryType, field_get(f, nanofix::tag::MDEntryType));
                auto const sym = field_get(f, nanofix::tag::Symbol);
                auto const px = field_get(f, nanofix::tag::MDEntryPx);
                auto const size = field_get(f, nanofix::tag::MDEntrySize);
                std::printf("    %-6.*s %-6.*s %-6.*s px=%-10.*s size=%.*s\n",
                            sv_arg(act),
                            act.data(),
                            sv_arg(type),
                            type.data(),
                            sv_arg(sym),
                            sym.data(),
                            sv_arg(px),
                            px.data(),
                            sv_arg(size),
                            size.data());
            });
        });
}

}  // namespace

int main() {
    std::printf("nanofix %s\n", NANOFIX_VERSION);

    std::array<char, 2048> feed{};
    std::span<char> const all(feed);
    std::size_t total = 0;
    total += write_snapshot(all.subspan(total), 1, "KO", 6012, 6015);
    total += write_incremental(all.subspan(total), 2);
    total += write_snapshot(all.subspan(total), 3, "MSFT", 41048, 41052);
    total += write_incremental(all.subspan(total), 4);

    std::size_t snapshots = 0;
    std::size_t incrementals = 0;
    nanofix::for_each_message(std::span<char const>(feed.data(), total),
                              [&](nanofix::message_reader const& r) noexcept {
                                  auto const mt = r.message_type()->value().as_string_view();
                                  if (mt == "W") {
                                      on_snapshot(r);
                                      ++snapshots;
                                  } else if (mt == "X") {
                                      on_incremental(r);
                                      ++incrementals;
                                  }
                              });

    std::printf("---\n%zu snapshots, %zu incrementals\n", snapshots, incrementals);
    return 0;
}
