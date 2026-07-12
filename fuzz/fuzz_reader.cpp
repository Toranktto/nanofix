#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

#include <nanofix.hpp>

namespace {

struct GroupSpec {
    int count_tag;
    int delim_tag;
};

constexpr GroupSpec kGroups[] = {
    {nanofix::tag::NoMDEntries, nanofix::tag::MDEntryType},
    {nanofix::tag::NoPartyIDs, nanofix::tag::PartyID},
    {nanofix::tag::NoOrders, nanofix::tag::ClOrdID},
    {nanofix::tag::NoLegs, nanofix::tag::LegSymbol},
    {nanofix::tag::NoSides, nanofix::tag::Side},
    {nanofix::tag::NoTrades, nanofix::tag::TradeReportID},
    {nanofix::tag::NoAllocs, nanofix::tag::AllocAccount},
    {nanofix::tag::NoContraBrokers, nanofix::tag::ContraBroker},
    {nanofix::tag::NoMiscFees, nanofix::tag::MiscFeeAmt},
    {nanofix::tag::NoRelatedSym, nanofix::tag::Symbol},
    {nanofix::tag::NoQuoteEntries, nanofix::tag::QuoteEntryID},
    {nanofix::tag::NoExecs, nanofix::tag::ExecID},
    {nanofix::tag::NoFills, nanofix::tag::FillExecID},
    {nanofix::tag::NoEvents, nanofix::tag::EventType},
    {nanofix::tag::NoInstrAttrib, nanofix::tag::InstrAttribType},
    {nanofix::tag::NoSecurityAltID, nanofix::tag::SecurityAltID},
    {nanofix::tag::NoUnderlyings, nanofix::tag::UnderlyingSymbol},
    {nanofix::tag::NoTradingSessions, nanofix::tag::TradingSessionID},
};

void exercise_field_value(nanofix::field_value const& v) noexcept {
    int64_t i_signed = 0;
    uint64_t i_unsigned = 0;
    int64_t mantissa = 0, exponent = 0;
    (void)v.try_as_int(i_signed);
    (void)v.try_as_int(i_unsigned);
    (void)v.try_as_decimal(mantissa, exponent);

    char c = 0;
    bool b = false;
    (void)v.try_as_char(c);
    (void)v.try_as_bool(b);

    int yy = 0, mm = 0, dd = 0;
    (void)v.as_date(yy, mm, dd);
    (void)v.as_monthyear(yy, mm);

    int h = 0, mi = 0, s = 0, ms = 0, ns = 0;
    (void)v.as_timeonly(h, mi, s, ms);
    (void)v.as_timeonly_nano(h, mi, s, ns);

    (void)v.as_epoch_millis();
    (void)v.as_epoch_nanos();

    // The fully validating chrono tier (digits/separators/ranges) — the
    // default read surface for production callers, so it must be fuzzed too.
    std::chrono::sys_time<std::chrono::milliseconds> tp_ms;
    std::chrono::sys_time<std::chrono::nanoseconds> tp_ns;
    (void)v.try_as_timestamp(tp_ms);
    (void)v.try_as_timestamp(tp_ns);
    std::chrono::milliseconds dur_ms{};
    std::chrono::nanoseconds dur_ns{};
    (void)v.try_as_timeonly(dur_ms);
    (void)v.try_as_timeonly(dur_ns);
    std::chrono::year_month_day ymd{};
    (void)v.try_as_date(ymd);
    std::chrono::year_month ym{};
    (void)v.try_as_monthyear(ym);

    (void)v.size();
    (void)v.as_string_view();
}

// Typed find(tag::X) facade: category-gated accessors on a typed_value.
void exercise_typed_find(nanofix::message_reader const& r) noexcept {
    if (auto sending = r.find(nanofix::tag::SendingTime)) {
        std::chrono::sys_time<std::chrono::nanoseconds> tp;
        (void)sending.try_as_timestamp(tp);
        (void)sending.as_epoch_millis();
    }
    if (auto px = r.find(nanofix::tag::Price)) {
        int64_t m = 0, e = 0;
        (void)px.try_as_decimal(m, e);
    }
    if (auto seq = r.find(nanofix::tag::MsgSeqNum)) {
        uint64_t n = 0;
        (void)seq.try_as_int(n);
    }
    if (auto side = r.find(nanofix::tag::Side)) {
        char c = 0;
        (void)side.try_as_char(c);
    }
    if (auto possdup = r.find(nanofix::tag::PossDupFlag)) {
        bool b = false;
        (void)possdup.try_as_bool(b);
    }
    if (auto d = r.find(nanofix::tag::MaturityDate)) {
        std::chrono::year_month_day ymd{};
        (void)d.try_as_date(ymd);
    }
    if (auto my = r.find(nanofix::tag::MaturityMonthYear)) {
        std::chrono::year_month ym{};
        (void)my.try_as_monthyear(ym);
    }
}

void exercise_groups(nanofix::message_reader const& r) noexcept {
    for (auto const& spec : kGroups) {
        r.group(spec.count_tag, spec.delim_tag).for_each([&](nanofix::group_entry const& entry) {
            for (auto it = entry.begin(); it != entry.end(); ++it) {
                (void)it->tag();
                exercise_field_value(it->value());
            }
            // Exercises the group-entry build_field_index overload.
            nanofix::field_index_buffer<32> ibuf;
            auto eidx = nanofix::build_field_index(entry, ibuf);
            std::size_t hint = 0;
            (void)eidx.find_with_hint(spec.delim_tag, hint);
        });
    }
}

void fuzz_one(std::span<char const> wire) noexcept {
    nanofix::message_reader r(wire);
    if (r.is_complete() && r.is_valid()) {
        for (auto it = r.begin(); it != r.end(); ++it) {
            (void)it->tag();
            exercise_field_value(it->value());
        }

        (void)r.calculate_check_sum();
        (void)r.message_type();
        (void)r.check_sum();

        exercise_typed_find(r);
        exercise_groups(r);

        nanofix::field_index_buffer<256> ibuf;
        auto idx = nanofix::build_field_index(r, ibuf);
        if (idx.field_count() > 0) {
            std::size_t hint = 0;
            (void)idx.find_with_hint(nanofix::tag::Price, hint);
            (void)idx.has(nanofix::tag::MsgSeqNum);
            (void)idx.has(nanofix::tag::NoMDEntries);
            (void)idx.has(nanofix::tag::NoPartyIDs);
        }
    }

    char const* tail = nanofix::for_each_message(wire, [](nanofix::message_reader const& m) noexcept {
        for (auto it = m.begin(); it != m.end(); ++it) {
            (void)it->tag();
        }
        exercise_groups(m);
    });
    (void)tail;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size) {
    std::span<char const> wire(reinterpret_cast<char const*>(data), size);
    fuzz_one(wire);
    return 0;
}
