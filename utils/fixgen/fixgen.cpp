// Write N synthetic FIX 5.0 SP2 messages over FIXT 1.1 to <path>.
//   fixgen -o <path> [-n count] [-s seed]
// Output is wire-format (no newlines); pipe through fixprint to inspect.

#include <nanofix.hpp>
#include <nanofix/detail/fields.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <string_view>

namespace {

constexpr std::size_t kBuf = 1 << 18;

char const* const kSymbols[] = {
    "AAPL",   "MSFT",   "GOOG",   "GOOGL",  "AMZN",   "TSLA",   "META",   "NVDA",   "BRK-B",
    "V",      "JPM",    "JNJ",    "WMT",    "MA",     "PG",     "XOM",    "UNH",    "HD",
    "BAC",    "ABBV",   "PFE",    "AVGO",   "CVX",    "KO",     "LLY",    "MRK",    "COST",
    "PEP",    "TMO",    "ADBE",   "CSCO",   "DIS",    "NKE",    "ABT",    "ACN",    "MCD",
    "TXN",    "DHR",    "CRM",    "NEE",    "VZ",     "ORCL",   "NFLX",   "AMD",    "INTC",
    "AMGN",   "IBM",    "T",      "BMY",    "QCOM",   "GE",     "GS",     "MS",     "AXP",
    "CAT",    "BLK",    "RTX",    "SPGI",   "BKNG",   "AMT",    "NOW",    "INTU",   "DE",
    "GILD",   "ISRG",   "MDT",    "ELV",    "ADP",    "LMT",    "MMC",    "SYK",    "CB",
    "MO",     "SCHW",   "CI",     "ZTS",    "BDX",    "USB",    "PNC",    "TJX",    "REGN",
    "BSX",    "EOG",    "EQIX",   "SHW",    "AMAT",   "SO",     "NSC",    "PLD",    "NOC",
    "CSX",    "FCX",    "ADI",    "ITW",    "FDX",    "KDP",    "ICE",    "GD",     "EW",
    "EMR",    "COP",    "MMM",    "BX",     "MDLZ",   "MU",     "PYPL",   "PANW",   "LRCX",
    "KLAC",   "SBUX",   "SPY",    "QQQ",    "IWM",    "DIA",    "VOO",    "VTI",    "EFA",
    "EEM",    "AGG",    "BND",    "GLD",    "SLV",    "USO",    "UNG",    "TLT",    "IEF",
    "SHY",    "LQD",    "HYG",    "VEA",    "VWO",    "IVV",    "IJR",    "IJH",    "IAU",
    "GDX",    "GDXJ",   "XLF",    "XLE",    "XLK",    "XLV",    "XLI",    "XLP",    "XLY",
    "XLU",    "XLB",    "XLRE",   "XLC",    "VNQ",    "DBC",    "ESH5",   "ESM5",   "ESU5",
    "ESZ5",   "NQH5",   "NQM5",   "NQU5",   "NQZ5",   "YMH5",   "YMM5",   "RTYH5",  "RTYM5",
    "CLG5",   "CLH5",   "NGG5",   "NGH5",   "RBG5",   "HOG5",   "BZG5",   "GCG5",   "GCJ5",
    "GCM5",   "SIH5",   "SIK5",   "HGH5",   "HGK5",   "PAH5",   "PLF5",   "ZBH5",   "ZNH5",
    "ZFH5",   "ZTH5",   "ZCH5",   "ZSH5",   "ZWH5",   "ZLH5",   "ZMH5",   "VXG5",   "VXH5",
    "VXM5",   "EURUSD", "GBPUSD", "USDJPY", "AUDUSD", "USDCAD", "USDCHF", "NZDUSD", "USDCNH",
    "EURGBP", "EURJPY", "GBPJPY", "AUDJPY", "EURCHF", "EURAUD", "EURNZD", "EURCAD", "EURSEK",
    "EURNOK", "USDMXN", "USDZAR", "6EH5",   "6JH5",   "6BH5",   "6AH5",   "6CH5",   "6SH5",
    "BTCH5",  "BTCM5",  "ETHH5",  "ETHM5",
};
constexpr std::size_t kNumSymbols = sizeof(kSymbols) / sizeof(kSymbols[0]);

char const* const kSenders[] = {"A", "B", "CME", "CLNT", "EXCH"};
constexpr std::size_t kNumSenders = sizeof(kSenders) / sizeof(kSenders[0]);

// Random nanosecond timestamp on 2024-01-15. Digits vary in every position so
// the SWAR timestamp parser sees full coverage.
template <typename Rng>
auto base_time(Rng& rng) noexcept {
    using namespace std::chrono;
    std::uniform_int_distribution<int> hour_dist(9, 15);
    std::uniform_int_distribution<int> minute_dist(0, 59);
    std::uniform_int_distribution<int> second_dist(0, 59);
    std::uniform_int_distribution<int> nano_dist(0, 999'999'999);
    return sys_days{year{2024} / January / 15} + hours{hour_dist(rng)} + minutes{minute_dist(rng)} +
           seconds{second_dist(rng)} + nanoseconds{nano_dist(rng)};
}

// 20-char ClOrdID — venue norm (UTC compaction + counter).
std::string_view make_clordid(int seq) noexcept {
    static thread_local char buf[20];
    static constexpr char hex[] = "0123456789ABCDEF";
    buf[0] = 'C';
    buf[1] = 'L';
    auto u = static_cast<std::uint32_t>(seq);
    for (int i = 0; i < 8; ++i)
        buf[2 + i] = hex[(u >> ((7 - i) * 4)) & 0xF];
    std::uint64_t mix = static_cast<std::uint64_t>(seq) * 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 10; ++i)
        buf[10 + i] = hex[(mix >> ((9 - i) * 4)) & 0xF];
    return std::string_view{buf, sizeof(buf)};
}

// 8-char SecurityID — ISIN/SEDOL-shaped.
std::string_view make_security_id(int seq) noexcept {
    static thread_local char buf[8];
    static constexpr char alpha[] = "ABCDEFGHJKLMNPQRSTUVWXYZ";
    auto u = static_cast<std::uint64_t>(seq) * 0x100000001B3ULL ^ 0xCBF29CE484222325ULL;
    for (int i = 0; i < 8; ++i)
        buf[i] = alpha[(u >> (i * 3)) & 0x17];
    return std::string_view{buf, sizeof(buf)};
}

std::string_view make_account(int seq) noexcept {
    static thread_local char buf[12];
    static constexpr char alnum[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    buf[0] = 'A';
    buf[1] = 'C';
    buf[2] = 'C';
    auto u = static_cast<std::uint64_t>(seq) * 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 9; ++i)
        buf[3 + i] = alnum[(u >> (i * 3)) & 0x1F];
    return std::string_view{buf, sizeof(buf)};
}

template <typename Rng>
[[nodiscard]] bool write_session_header(nanofix::message_writer& w,
                                        char const* msg_type,
                                        int seq,
                                        char const* sender,
                                        char const* target,
                                        Rng& rng) {
    w.push_back_header("FIXT.1.1");
    w.push_back_string(nanofix::tag::MsgType, std::string_view{msg_type});
    w.push_back_string(nanofix::tag::SenderCompID, std::string_view{sender});
    w.push_back_string(nanofix::tag::TargetCompID, std::string_view{target});
    w.push_back_int(nanofix::tag::MsgSeqNum, seq);
    w.push_back_timestamp_nano(nanofix::tag::SendingTime, base_time(rng));
    return w.ok();
}

[[nodiscard]] bool push_back_trailer_msg(nanofix::message_writer& w, char const* what, int seq) {
    if (!w.push_back_trailer()) {
        std::fprintf(stderr, "fixgen: writer failure on %s seq=%d\n", what, seq);
        return false;
    }
    return true;
}

template <typename Rng>
std::size_t write_heartbeat(
    char* buf, std::size_t cap, int seq, char const* sender, char const* target, Rng& rng, bool& ok) {
    nanofix::message_writer w(buf, buf + cap);
    if (!write_session_header(w, "0", seq, sender, target, rng)) {
        ok = false;
        return 0;
    }
    if (!push_back_trailer_msg(w, "Heartbeat", seq)) {
        ok = false;
        return 0;
    }
    return static_cast<std::size_t>(w.message_end() - buf);
}

template <typename Rng>
std::size_t write_logon(
    char* buf, std::size_t cap, int seq, char const* sender, char const* target, Rng& rng, bool& ok) {
    nanofix::message_writer w(buf, buf + cap);
    if (!write_session_header(w, "A", seq, sender, target, rng)) {
        ok = false;
        return 0;
    }
    w.push_back_int(nanofix::tag::EncryptMethod, 0);
    w.push_back_int(nanofix::tag::HeartBtInt, 30);
    w.push_back_int(nanofix::tag::DefaultApplVerID, 9);
    if (!push_back_trailer_msg(w, "Logon", seq)) {
        ok = false;
        return 0;
    }
    return static_cast<std::size_t>(w.message_end() - buf);
}

template <typename Rng>
std::size_t write_logout(
    char* buf, std::size_t cap, int seq, char const* sender, char const* target, Rng& rng, bool& ok) {
    nanofix::message_writer w(buf, buf + cap);
    if (!write_session_header(w, "5", seq, sender, target, rng)) {
        ok = false;
        return 0;
    }
    if (!push_back_trailer_msg(w, "Logout", seq)) {
        ok = false;
        return 0;
    }
    return static_cast<std::size_t>(w.message_end() - buf);
}

template <typename Rng>
std::size_t write_new_order(char* buf,
                            std::size_t cap,
                            int seq,
                            char const* sender,
                            char const* target,
                            char const* symbol,
                            std::int64_t price,
                            std::int64_t qty,
                            char side,
                            Rng& rng,
                            bool randomize,
                            bool& ok) {
    nanofix::message_writer w(buf, buf + cap);
    if (!write_session_header(w, "D", seq, sender, target, rng)) {
        ok = false;
        return 0;
    }
    auto const ts = base_time(rng);
    auto write_field = [&](int idx) {
        switch (idx) {
            case 0:
                w.push_back_string(nanofix::tag::ClOrdID, make_clordid(seq));
                break;
            case 1:
                w.push_back_string(nanofix::tag::Account, make_account(seq));
                break;
            case 2:
                w.push_back_string(nanofix::tag::SecurityID, make_security_id(seq));
                break;
            case 3:
                w.push_back_char(nanofix::tag::HandlInst, '1');
                break;
            case 4:
                w.push_back_string(nanofix::tag::Symbol, std::string_view{symbol});
                break;
            case 5:
                w.push_back_char(nanofix::tag::Side, side);
                break;
            case 6:
                w.push_back_timestamp_nano(nanofix::tag::TransactTime, ts);
                break;
            case 7:
                w.push_back_decimal(nanofix::tag::OrderQty, qty, std::int64_t{-8});
                break;
            case 8:
                w.push_back_char(nanofix::tag::OrdType, '2');
                break;
            case 9:
                w.push_back_decimal(nanofix::tag::Price, price, std::int64_t{-8});
                break;
            case 10:
                w.push_back_char(nanofix::tag::TimeInForce, '0');
                break;
            default:
                break;
        }
    };
    std::array<int, 11> order;
    for (int i = 0; i < 11; ++i)
        order[i] = i;
    if (randomize)
        std::shuffle(order.begin(), order.end(), rng);
    for (int idx : order)
        write_field(idx);
    if (!push_back_trailer_msg(w, "NewOrderSingle", seq)) {
        ok = false;
        return 0;
    }
    return static_cast<std::size_t>(w.message_end() - buf);
}

template <typename Rng>
std::size_t write_exec_report(char* buf,
                              std::size_t cap,
                              int seq,
                              char const* sender,
                              char const* target,
                              char const* symbol,
                              std::int64_t price,
                              std::int64_t qty,
                              char side,
                              Rng& rng,
                              bool randomize,
                              bool& ok) {
    nanofix::message_writer w(buf, buf + cap);
    if (!write_session_header(w, "8", seq, sender, target, rng)) {
        ok = false;
        return 0;
    }
    auto write_field = [&](int idx) {
        switch (idx) {
            case 0:
                w.push_back_string(nanofix::tag::ClOrdID, make_clordid(seq));
                break;
            case 1:
                w.push_back_string(nanofix::tag::Account, make_account(seq));
                break;
            case 2:
                w.push_back_string(nanofix::tag::SecurityID, make_security_id(seq));
                break;
            case 3:
                w.push_back_int(nanofix::tag::OrderID, seq);
                break;
            case 4:
                w.push_back_int(nanofix::tag::ExecID, seq * 7);
                break;
            case 5:
                w.push_back_char(nanofix::tag::ExecType, '2');
                break;
            case 6:
                w.push_back_char(nanofix::tag::OrdStatus, '2');
                break;
            case 7:
                w.push_back_string(nanofix::tag::Symbol, std::string_view{symbol});
                break;
            case 8:
                w.push_back_char(nanofix::tag::Side, side);
                break;
            case 9:
                w.push_back_decimal(nanofix::tag::LeavesQty, std::int64_t{0}, std::int64_t{-8});
                break;
            case 10:
                w.push_back_decimal(nanofix::tag::CumQty, qty, std::int64_t{-8});
                break;
            case 11:
                w.push_back_decimal(nanofix::tag::AvgPx, price, std::int64_t{-8});
                break;
            case 12:
                w.push_back_decimal(nanofix::tag::LastPx, price, std::int64_t{-8});
                break;
            case 13:
                w.push_back_decimal(nanofix::tag::LastQty, qty, std::int64_t{-8});
                break;
            default:
                break;
        }
    };
    std::array<int, 14> order;
    for (int i = 0; i < 14; ++i)
        order[i] = i;
    if (randomize)
        std::shuffle(order.begin(), order.end(), rng);
    for (int idx : order)
        write_field(idx);
    // ~10% of ExecReports carry venue-specific RawData payload — exercises
    // the data-length pair path (is_tag_a_data_length / parse-with-length).
    std::uniform_int_distribution<int> raw_dist(0, 9);
    if (raw_dist(rng) == 0) {
        static thread_local char payload[48];
        std::uniform_int_distribution<int> len_dist(8, 48);
        int const len = len_dist(rng);
        for (int i = 0; i < len; ++i) {
            // Include SOH (0x01) bytes deliberately — the data-length parser
            // must not stop on SOH inside the value.
            payload[i] = static_cast<char>((seq * 31 + i * 7 + (i & 3)) & 0xFF);
        }
        w.push_back_data(nanofix::tag::RawDataLength, nanofix::tag::RawData, payload, payload + len);
    }
    if (!push_back_trailer_msg(w, "ExecutionReport", seq)) {
        ok = false;
        return 0;
    }
    return static_cast<std::size_t>(w.message_end() - buf);
}

template <typename Rng>
std::size_t write_md_snapshot(
    char* buf, std::size_t cap, int seq, char const* sender, char const* target, Rng& rng, bool& ok) {
    std::uniform_int_distribution<int> levels_dist(3, 10);
    std::uniform_int_distribution<std::int64_t> qty_dist(100000LL, 500000000000LL);
    std::uniform_int_distribution<std::int64_t> mid_dist(500'000'000LL, 6'000'000'000'000LL);
    std::uniform_int_distribution<std::int64_t> spread_dist(50'000LL, 5'000'000LL);
    std::uniform_int_distribution<int> sym_dist(0, kNumSymbols - 1);

    char const* symbol = kSymbols[sym_dist(rng)];
    int n_bid = levels_dist(rng);
    int n_ask = levels_dist(rng);
    std::int64_t mid = mid_dist(rng);
    std::int64_t spread = spread_dist(rng);

    nanofix::message_writer w(buf, buf + cap);
    if (!write_session_header(w, "W", seq, sender, target, rng)) {
        ok = false;
        return 0;
    }
    w.push_back_string(nanofix::tag::Symbol, std::string_view{symbol});
    w.push_back_int(nanofix::tag::NoMDEntries, n_bid + n_ask);
    for (int i = 0; i < n_bid; ++i) {
        w.push_back_char(nanofix::tag::MDEntryType, '0');
        w.push_back_decimal(nanofix::tag::MDEntryPx,
                            mid - spread - static_cast<std::int64_t>(i) * 1000,
                            std::int64_t{-8});
        w.push_back_decimal(nanofix::tag::MDEntrySize, qty_dist(rng), std::int64_t{-8});
    }
    for (int i = 0; i < n_ask; ++i) {
        w.push_back_char(nanofix::tag::MDEntryType, '1');
        w.push_back_decimal(nanofix::tag::MDEntryPx,
                            mid + spread + static_cast<std::int64_t>(i) * 1000,
                            std::int64_t{-8});
        w.push_back_decimal(nanofix::tag::MDEntrySize, qty_dist(rng), std::int64_t{-8});
    }
    if (!push_back_trailer_msg(w, "MarketDataSnapshot", seq)) {
        ok = false;
        return 0;
    }
    return static_cast<std::size_t>(w.message_end() - buf);
}

template <typename Rng>
std::size_t write_md_incremental(
    char* buf, std::size_t cap, int seq, char const* sender, char const* target, Rng& rng, bool& ok) {
    std::uniform_int_distribution<int> entries_dist(1, 200);
    std::uniform_int_distribution<std::int64_t> qty_dist(100000LL, 500000000000LL);
    std::uniform_int_distribution<std::int64_t> px_dist(500'000'000LL, 6'000'000'000'000LL);
    char const update_actions[] = {'0', '1', '2'};

    int n = entries_dist(rng);

    int indices[kNumSymbols];
    for (std::size_t i = 0; i < kNumSymbols; ++i)
        indices[i] = static_cast<int>(i);
    for (int i = 0; i < n; ++i) {
        std::uniform_int_distribution<int> swap_dist(i, static_cast<int>(kNumSymbols) - 1);
        int j = swap_dist(rng);
        std::swap(indices[i], indices[j]);
    }

    nanofix::message_writer w(buf, buf + cap);
    if (!write_session_header(w, "X", seq, sender, target, rng)) {
        ok = false;
        return 0;
    }
    w.push_back_int(nanofix::tag::NoMDEntries, n);
    for (int i = 0; i < n; ++i) {
        w.push_back_char(nanofix::tag::MDUpdateAction, update_actions[i % 3]);
        w.push_back_char(nanofix::tag::MDEntryType, (i & 1) ? '0' : '1');
        w.push_back_string(nanofix::tag::Symbol, std::string_view{kSymbols[indices[i]]});
        w.push_back_decimal(nanofix::tag::MDEntryPx, px_dist(rng), std::int64_t{-8});
        w.push_back_decimal(nanofix::tag::MDEntrySize, qty_dist(rng), std::int64_t{-8});
    }
    if (!push_back_trailer_msg(w, "MarketDataIncremental", seq)) {
        ok = false;
        return 0;
    }
    return static_cast<std::size_t>(w.message_end() - buf);
}

struct Args {
    std::string out;
    long count = 200000;
    std::uint32_t seed = 42;
    bool randomize_fields = false;
};

bool parse_args(int argc, char** argv, Args& args) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) {
            args.out = argv[++i];
        } else if (a == "-n" && i + 1 < argc) {
            args.count = std::strtol(argv[++i], nullptr, 10);
        } else if (a == "-s" && i + 1 < argc) {
            args.seed = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "-r" || a == "--random-fields") {
            args.randomize_fields = true;
        } else if (a == "-h" || a == "--help") {
            return false;
        } else {
            std::cerr << "unknown argument: " << a << "\n";
            return false;
        }
    }
    if (args.out.empty()) {
        std::cerr << "missing -o <path>\n";
        return false;
    }
    if (args.count <= 0) {
        std::cerr << "count must be positive\n";
        return false;
    }
    return true;
}

void usage(char const* argv0) {
    std::cerr << "usage: " << argv0 << " -o <path> [-n count] [-s seed] [-r|--random-fields]\n"
              << "  -r  shuffle body field order per message (default: fixed order)\n";
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) {
        usage(argv[0]);
        return 2;
    }

    std::ostream* out = nullptr;
    std::ofstream file;
    if (args.out == "-") {
        out = &std::cout;
    } else {
        file.open(args.out, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::cerr << "cannot open " << args.out << " for writing\n";
            return 1;
        }
        out = &file;
    }

    std::mt19937 rng(args.seed);
    // Mix tuned to venue feed shape: MD-incremental dominates, orders ~15%,
    // session msgs sporadic.
    std::discrete_distribution<int> type_dist({
        3,   // Heartbeat
        8,   // NewOrderSingle
        7,   // ExecutionReport
        5,   // MarketDataSnapshot
        75,  // MarketDataIncremental
        1,   // Logon
        1,   // Logout
    });
    std::uniform_int_distribution<int> sym_dist(0, kNumSymbols - 1);
    std::uniform_int_distribution<int> sender_dist(0, kNumSenders - 1);
    std::uniform_int_distribution<std::int64_t> price_dist(500'000'000LL, 6'000'000'000'000LL);
    std::uniform_int_distribution<std::int64_t> qty_dist(100000LL, 50000000000LL);
    std::uniform_int_distribution<int> side_dist(0, 1);

    char buf[kBuf];
    std::size_t total_bytes = 0;
    std::size_t min_seen = SIZE_MAX;
    std::size_t max_seen = 0;
    bool dataset_ok = true;

    // Inject `corruption_interval`-message blocks of noise — exercises the
    // next_message_reader resync path that `is_valid()` falls through to.
    constexpr long corruption_interval = 10000;
    std::uniform_int_distribution<int> corrupt_len_dist(4, 24);
    std::uniform_int_distribution<int> corrupt_byte_dist(0, 255);

    for (long i = 0; i < args.count; ++i) {
        int t = type_dist(rng);
        char const* sym = kSymbols[sym_dist(rng)];
        char const* sender = kSenders[sender_dist(rng)];
        char const* target = kSenders[sender_dist(rng)];
        std::int64_t price = price_dist(rng);
        std::int64_t qty = qty_dist(rng);
        char side = side_dist(rng) == 0 ? '1' : '2';
        int seq = static_cast<int>(i + 1);

        std::size_t n = 0;
        switch (t) {
            case 0:
                n = write_heartbeat(buf, kBuf, seq, sender, target, rng, dataset_ok);
                break;
            case 1:
                n = write_new_order(buf,
                                    kBuf,
                                    seq,
                                    sender,
                                    target,
                                    sym,
                                    price,
                                    qty,
                                    side,
                                    rng,
                                    args.randomize_fields,
                                    dataset_ok);
                break;
            case 2:
                n = write_exec_report(buf,
                                      kBuf,
                                      seq,
                                      sender,
                                      target,
                                      sym,
                                      price,
                                      qty,
                                      side,
                                      rng,
                                      args.randomize_fields,
                                      dataset_ok);
                break;
            case 3:
                n = write_md_snapshot(buf, kBuf, seq, sender, target, rng, dataset_ok);
                break;
            case 4:
                n = write_md_incremental(buf, kBuf, seq, sender, target, rng, dataset_ok);
                break;
            case 5:
                n = write_logon(buf, kBuf, seq, sender, target, rng, dataset_ok);
                break;
            case 6:
                n = write_logout(buf, kBuf, seq, sender, target, rng, dataset_ok);
                break;
            default:
                break;
        }
        if (!dataset_ok) {
            std::fprintf(stderr, "fixgen: aborting at seq=%d after writer failure\n", seq);
            return 1;
        }
        out->write(buf, static_cast<std::streamsize>(n));
        total_bytes += n;
        if (n < min_seen)
            min_seen = n;
        if (n > max_seen)
            max_seen = n;

        if ((i + 1) % corruption_interval == 0 && (i + 1) < args.count) {
            char noise[24];
            int const nlen = corrupt_len_dist(rng);
            for (int j = 0; j < nlen; ++j) {
                char c;
                do {
                    c = static_cast<char>(corrupt_byte_dist(rng));
                } while (c == '8');  // avoid spurious "8=FIX" pattern start
                noise[j] = c;
            }
            out->write(noise, nlen);
            total_bytes += static_cast<std::size_t>(nlen);
        }
    }

    std::cerr << "wrote " << args.count << " messages, " << total_bytes << " bytes to " << args.out
              << " (min " << min_seen << "B, max " << max_seen << "B, avg "
              << (total_bytes / static_cast<std::size_t>(args.count)) << "B)\n";
    return 0;
}
