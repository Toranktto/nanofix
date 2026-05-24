#include <gtest/gtest.h>

#include <nanofix.hpp>
#include <nanofix/detail/fields.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

using namespace nanofix;

namespace {

constexpr std::size_t kNumThreads = 8;
constexpr std::size_t kIterations = 4096;

std::size_t build_logon(char* buf, std::size_t cap) {
    message_writer w(buf, buf + cap);
    w.push_back_header("FIXT.1.1");
    w.push_back_string(tag::MsgType, "A");
    w.push_back_string(tag::SenderCompID, "SENDER");
    w.push_back_string(tag::TargetCompID, "TARGET");
    w.push_back_int(tag::MsgSeqNum, 1);
    w.push_back_int(tag::EncryptMethod, 0);
    w.push_back_int(tag::HeartBtInt, 30);
    w.push_back_int(tag::Price, 12345);
    w.push_back_string(tag::Symbol, "AAPL");
    bool ok = w.push_back_trailer();
    EXPECT_TRUE(ok);
    return static_cast<std::size_t>(w.message_end() - buf);
}

std::size_t build_n_messages(char* buf, std::size_t cap, std::size_t n) {
    char* p = buf;
    for (std::size_t i = 0; i < n; ++i) {
        message_writer w(p, buf + cap);
        w.push_back_header("FIXT.1.1");
        w.push_back_string(tag::MsgType, "D");
        w.push_back_string(tag::SenderCompID, "S");
        w.push_back_string(tag::TargetCompID, "T");
        w.push_back_int(tag::MsgSeqNum, static_cast<int>(i + 1));
        w.push_back_string(tag::Symbol, "AAPL");
        w.push_back_int(tag::Price, static_cast<int>(100 + i));
        bool ok = w.push_back_trailer();
        EXPECT_TRUE(ok);
        p = w.message_end();
    }
    return static_cast<std::size_t>(p - buf);
}

}  // namespace

TEST(Threading, ConcurrentIterationSameConstReader) {
    char buf[1024];
    std::size_t const sz = build_logon(buf, sizeof(buf));
    message_reader const r(buf, buf + sz);
    ASSERT_TRUE(r.is_complete() && r.is_valid());

    std::vector<std::size_t> field_counts(kNumThreads, 0);
    std::vector<std::uint64_t> tag_sums(kNumThreads, 0);
    std::barrier sync(static_cast<std::ptrdiff_t>(kNumThreads));
    std::vector<std::thread> ts;
    ts.reserve(kNumThreads);

    for (std::size_t t = 0; t < kNumThreads; ++t) {
        ts.emplace_back([&, t]() {
            sync.arrive_and_wait();
            for (std::size_t k = 0; k < kIterations; ++k) {
                std::size_t fc = 0;
                std::uint64_t ts_sum = 0;
                for (auto it = r.begin(); it != r.end(); ++it) {
                    ++fc;
                    ts_sum += static_cast<std::uint64_t>(it->tag());
                }
                if (k == kIterations - 1) {
                    field_counts[t] = fc;
                    tag_sums[t] = ts_sum;
                }
            }
        });
    }
    for (auto& th : ts)
        th.join();

    for (std::size_t t = 1; t < kNumThreads; ++t) {
        EXPECT_EQ(field_counts[t], field_counts[0]);
        EXPECT_EQ(tag_sums[t], tag_sums[0]);
    }
    EXPECT_GT(field_counts[0], 5u);
}

TEST(Threading, ConcurrentReaderCopiesOnSameBuffer) {
    char buf[1024];
    std::size_t const sz = build_logon(buf, sizeof(buf));
    std::span<char const> const wire(buf, sz);

    std::atomic<std::size_t> mismatches{0};
    std::barrier sync(static_cast<std::ptrdiff_t>(kNumThreads));
    std::vector<std::thread> ts;
    ts.reserve(kNumThreads);

    for (std::size_t t = 0; t < kNumThreads; ++t) {
        ts.emplace_back([&]() {
            sync.arrive_and_wait();
            for (std::size_t k = 0; k < kIterations; ++k) {
                message_reader local(wire.data(), wire.data() + wire.size());
                if (!local.is_complete() || !local.is_valid()) {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                std::size_t fc = 0;
                for (auto it = local.begin(); it != local.end(); ++it)
                    ++fc;
                if (fc < 6)
                    mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : ts)
        th.join();
    EXPECT_EQ(mismatches.load(), 0u);
}

TEST(Threading, ConcurrentIndexedFindWithPerThreadHint) {
    char buf[1024];
    std::size_t const sz = build_logon(buf, sizeof(buf));
    message_reader const r(buf, buf + sz);
    ASSERT_TRUE(r.is_complete() && r.is_valid());

    field_index_buffer<64> idx_buf;
    auto const idx = build_field_index(r, idx_buf);
    ASSERT_FALSE(idx.truncated());
    ASSERT_GT(idx.field_count(), 0u);

    std::atomic<std::size_t> mismatches{0};
    std::barrier sync(static_cast<std::ptrdiff_t>(kNumThreads));
    std::vector<std::thread> ts;
    ts.reserve(kNumThreads);

    int const expected_price = 12345;
    for (std::size_t t = 0; t < kNumThreads; ++t) {
        ts.emplace_back([&]() {
            sync.arrive_and_wait();
            for (std::size_t k = 0; k < kIterations; ++k) {
                std::size_t hint = 0;
                auto v1 = idx.find_with_hint(tag::Price, hint);  // Price is decimal
                long mant = 0, exp = 0;
                if (!v1.try_as_decimal(mant, exp) || mant != expected_price || exp != 0)
                    mismatches.fetch_add(1, std::memory_order_relaxed);

                std::size_t hint2 = 0;
                auto v2 = idx.find_with_hint(tag::Symbol, hint2);
                if (v2.as_string_view() != "AAPL")
                    mismatches.fetch_add(1, std::memory_order_relaxed);

                if (!idx.has(tag::MsgSeqNum))
                    mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : ts)
        th.join();
    EXPECT_EQ(mismatches.load(), 0u);
}

TEST(Threading, ConcurrentBuildFieldIndexPerThreadBuffer) {
    // Each thread builds its own index from the shared const reader: the
    // bulk-SIMD build_field_index sweep, run concurrently. Reader and bytes are
    // immutable; per-thread writes are the local buffer and the stack SOH window.
    char buf[1024];
    std::size_t const sz = build_logon(buf, sizeof(buf));
    message_reader const r(buf, buf + sz);
    ASSERT_TRUE(r.is_complete() && r.is_valid());

    field_index_buffer<64> ref_buf;
    auto const ref = build_field_index(r, ref_buf);
    ASSERT_FALSE(ref.truncated());
    std::size_t const expected_count = ref.field_count();
    ASSERT_GT(expected_count, 0u);

    std::atomic<std::size_t> mismatches{0};
    std::barrier sync(static_cast<std::ptrdiff_t>(kNumThreads));
    std::vector<std::thread> ts;
    ts.reserve(kNumThreads);

    for (std::size_t t = 0; t < kNumThreads; ++t) {
        ts.emplace_back([&]() {
            field_index_buffer<64> local;
            sync.arrive_and_wait();
            for (std::size_t k = 0; k < kIterations; ++k) {
                auto const idx = build_field_index(r, local);
                if (idx.truncated() || idx.field_count() != expected_count) {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                std::size_t hint = 0;
                long mant = 0, exp = 0;  // Price is decimal
                if (!idx.find_with_hint(tag::Price, hint).try_as_decimal(mant, exp) ||
                    mant != 12345 || exp != 0)
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                std::size_t hint2 = 0;
                if (idx.find_with_hint(tag::Symbol, hint2).as_string_view() != "AAPL")
                    mismatches.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : ts)
        th.join();
    EXPECT_EQ(mismatches.load(), 0u);
}

TEST(Threading, ConcurrentMessagesRangeIteration) {
    std::vector<char> buf(std::size_t{64} * 1024);
    constexpr std::size_t kMsgCount = 64;
    std::size_t const sz = build_n_messages(buf.data(), buf.size(), kMsgCount);

    std::vector<std::size_t> seen(kNumThreads, 0);
    std::barrier sync(static_cast<std::ptrdiff_t>(kNumThreads));
    std::vector<std::thread> ts;
    ts.reserve(kNumThreads);

    for (std::size_t t = 0; t < kNumThreads; ++t) {
        ts.emplace_back([&, t]() {
            sync.arrive_and_wait();
            std::size_t count = 0;
            for (std::size_t k = 0; k < 128; ++k) {
                std::size_t local = 0;
                for (auto const& m : messages(std::span<char const>(buf.data(), sz))) {
                    (void)m;
                    ++local;
                }
                count = local;
            }
            seen[t] = count;
        });
    }
    for (auto& th : ts)
        th.join();

    for (std::size_t t = 0; t < kNumThreads; ++t) {
        EXPECT_EQ(seen[t], kMsgCount);
    }
}

TEST(Threading, ConcurrentForEachMessage) {
    std::vector<char> buf(std::size_t{64} * 1024);
    constexpr std::size_t kMsgCount = 32;
    std::size_t const sz = build_n_messages(buf.data(), buf.size(), kMsgCount);

    std::vector<std::uint64_t> sums(kNumThreads, 0);
    std::barrier sync(static_cast<std::ptrdiff_t>(kNumThreads));
    std::vector<std::thread> ts;
    ts.reserve(kNumThreads);

    for (std::size_t t = 0; t < kNumThreads; ++t) {
        ts.emplace_back([&, t]() {
            sync.arrive_and_wait();
            std::uint64_t acc = 0;
            for (std::size_t k = 0; k < 256; ++k) {
                std::uint64_t local = 0;
                for_each_message(std::span<char const>(buf.data(), sz),
                                 [&](message_reader const& m) noexcept {
                                     for (auto it = m.begin(); it != m.end(); ++it)
                                         local += static_cast<std::uint64_t>(it->tag());
                                 });
                acc = local;
            }
            sums[t] = acc;
        });
    }
    for (auto& th : ts)
        th.join();

    for (std::size_t t = 1; t < kNumThreads; ++t) {
        EXPECT_EQ(sums[t], sums[0]);
    }
    EXPECT_GT(sums[0], 0u);
}
