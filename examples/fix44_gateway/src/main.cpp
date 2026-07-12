// FIX 4.4 example. Writes NewOrderSingle x 2 + ExecutionReport,
// parses them back, prints decoded fields. nanofix/detail/fields.hpp here is
// regenerated from fixspec/FIX44.xml by nanofix_generate() (fields + names).

#include <nanofix.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr char kBeginString[] = "FIX.4.4";
constexpr char kSender[]      = "OMS";
constexpr char kTarget[]      = "EXCH";

std::size_t write_new_order(char* buf, std::size_t cap, int seq,
                            char const* clordid, char const* symbol,
                            char side, std::int64_t price_x100, int qty) {
    nanofix::message_writer w(buf, buf + cap);
    w.push_back_header(kBeginString);
    w.push_back_string(nanofix::tag::MsgType, "D");
    w.push_back_string(nanofix::tag::SenderCompID, kSender);
    w.push_back_string(nanofix::tag::TargetCompID, kTarget);
    w.push_back_int(nanofix::tag::MsgSeqNum, seq);
    w.push_back_timestamp(nanofix::tag::SendingTime,
                          std::chrono::system_clock::now());
    w.push_back_string(nanofix::tag::ClOrdID, clordid);
    w.push_back_char(nanofix::tag::HandlInst, '1');
    w.push_back_string(nanofix::tag::Symbol, symbol);
    w.push_back_char(nanofix::tag::Side, side);
    w.push_back_int(nanofix::tag::OrderQty, qty);
    w.push_back_char(nanofix::tag::OrdType, '2');
    w.push_back_decimal(nanofix::tag::Price, price_x100, std::int64_t{-2});
    w.push_back_char(nanofix::tag::TimeInForce, '0');
    if (!w.push_back_trailer())
        return 0;
    return static_cast<std::size_t>(w.message_end() - buf);
}

std::size_t write_execution_report(char* buf, std::size_t cap, int seq,
                                   char const* orderid, char const* clordid,
                                   char const* execid, char const* symbol,
                                   char side, std::int64_t last_px_x100,
                                   int last_qty, int leaves_qty, int cum_qty) {
    nanofix::message_writer w(buf, buf + cap);
    w.push_back_header(kBeginString);
    w.push_back_string(nanofix::tag::MsgType, "8");
    w.push_back_string(nanofix::tag::SenderCompID, kTarget);
    w.push_back_string(nanofix::tag::TargetCompID, kSender);
    w.push_back_int(nanofix::tag::MsgSeqNum, seq);
    w.push_back_timestamp(nanofix::tag::SendingTime,
                          std::chrono::system_clock::now());
    w.push_back_string(nanofix::tag::OrderID, orderid);
    w.push_back_string(nanofix::tag::ClOrdID, clordid);
    w.push_back_string(nanofix::tag::ExecID, execid);
    w.push_back_char(nanofix::tag::ExecType, 'F');
    w.push_back_char(nanofix::tag::OrdStatus, leaves_qty == 0 ? '2' : '1');
    w.push_back_string(nanofix::tag::Symbol, symbol);
    w.push_back_char(nanofix::tag::Side, side);
    w.push_back_int(nanofix::tag::LeavesQty, leaves_qty);
    w.push_back_int(nanofix::tag::CumQty, cum_qty);
    w.push_back_decimal(nanofix::tag::AvgPx, last_px_x100, std::int64_t{-2});
    w.push_back_int(nanofix::tag::LastQty, last_qty);
    w.push_back_decimal(nanofix::tag::LastPx, last_px_x100, std::int64_t{-2});
    if (!w.push_back_trailer())
        return 0;
    return static_cast<std::size_t>(w.message_end() - buf);
}

void print_message(nanofix::message_reader const& r) {
    auto mt = r.message_type()->value().as_string_view();
    std::printf("MsgType=%.*s\n",
                static_cast<int>(mt.size()), mt.data());

    // with_fields builds the index, checks truncated() once, then hands back an
    // accessor that is index-backed when the message fit idx_buf and
    // iterator-backed when it overran N=64 — its find(tag::X) carries that
    // choice with no per-lookup branch, so this reads the same either way.
    nanofix::field_index_buffer<64> idx_buf;
    nanofix::with_fields(r, idx_buf, [&](auto& f) {
        // as_string_view() is universal, so a text dump works for any tag.
        auto show = [](char const* label, auto tv) {
            if (tv)
                std::printf("  %-12s = %.*s\n", label,
                            static_cast<int>(tv.bytes().size()), tv.bytes().data());
        };
        show("SenderCompID", f.find(nanofix::tag::SenderCompID));
        show("TargetCompID", f.find(nanofix::tag::TargetCompID));
        show("ClOrdID", f.find(nanofix::tag::ClOrdID));
        show("OrderID", f.find(nanofix::tag::OrderID));
        show("ExecID", f.find(nanofix::tag::ExecID));
        show("Symbol", f.find(nanofix::tag::Symbol));

        char side_c = 0;
        if (f.find(nanofix::tag::Side).try_as_char(side_c))
            std::printf("  %-12s = %c\n", "Side", side_c);

        // Price is decimal: try_as_int would not compile here.
        long mant = 0, exp = 0;
        if (f.find(nanofix::tag::Price).try_as_decimal(mant, exp))
            std::printf("  %-12s = %ld x 10^%ld\n", "Price", mant, exp);
        if (f.find(nanofix::tag::AvgPx).try_as_decimal(mant, exp))
            std::printf("  %-12s = %ld x 10^%ld\n", "AvgPx", mant, exp);
    });
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("nanofix %s\n", NANOFIX_VERSION);

    std::vector<char> wire(8192);
    std::size_t off = 0;

    off += write_new_order(wire.data() + off, wire.size() - off, 1,
                           "CL-1001", "AAPL", '1', 17525, 100);
    off += write_new_order(wire.data() + off, wire.size() - off, 2,
                           "CL-1002", "MSFT", '2', 41275, 200);
    off += write_execution_report(wire.data() + off, wire.size() - off, 3,
                                  "ORD-1", "CL-1001", "EX-1", "AAPL",
                                  '1', 17525, 100, 0, 100);

    if (off == 0) {
        std::fputs("fix44_gateway: writer overflow\n", stderr);
        return EXIT_FAILURE;
    }

    if (argc > 1) {
        std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "fix44_gateway: cannot open %s\n", argv[1]);
            return EXIT_FAILURE;
        }
        out.write(wire.data(), static_cast<std::streamsize>(off));
        std::printf("wrote %zu bytes to %s\n\n", off, argv[1]);
    }

    int frames = 0;
    int checksum_rejects = 0;
    char const* tail = nanofix::for_each_message(
        wire.data(), wire.data() + off,
        [&](nanofix::message_reader const& r) {
            // Ingress checksum gate. Framing validation (is_valid(), already
            // done by for_each_message) never sums the bytes — and does not
            // check that the CheckSum field is digits, hence try_as_int.
            // Whether to pay this whole-message scan is a per-session policy;
            // over TCP many deployments skip it.
            unsigned char wire_cs = 0;
            if (!r.check_sum()->value().try_as_int(wire_cs) ||
                wire_cs != r.calculate_check_sum()) {
                ++checksum_rejects;
                return;
            }
            if (frames > 0)
                std::putchar('\n');
            print_message(r);
            ++frames;
        });

    auto leftover = static_cast<std::size_t>(wire.data() + off - tail);
    std::printf("\nparsed %d frame(s), %d checksum reject(s), "
                "%zu byte(s) of unread tail\n",
                frames, checksum_rejects, leftover);
    return EXIT_SUCCESS;
}
