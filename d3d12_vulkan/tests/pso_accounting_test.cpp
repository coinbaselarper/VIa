#include "../src/vk_run_counters.h"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace rc = leonida::run_counters;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

std::uint64_t reason_count(const std::string& reason) {
    auto& r = rc::refusals();
    std::lock_guard lock(r.mutex);
    const auto it = r.by_reason.find(reason);
    return it == r.by_reason.end() ? 0 : it->second;
}

std::pair<std::uint64_t, std::uint64_t> entry(const std::string& name) {
    auto& r = rc::refusals();
    std::lock_guard lock(r.mutex);
    const auto it = r.by_entry.find(name);
    return it == r.by_entry.end() ? std::pair<std::uint64_t, std::uint64_t>{} : it->second;
}

std::uint64_t value(const std::atomic<std::uint64_t>& a) { return a.load(); }

constexpr long kInvalidArg = static_cast<int>(0x80070057u);
constexpr long kNotImpl = static_cast<int>(0x80004001u);

long inner_graphics(long hr, const char* reason) {
    rc::PsoCall call("inner graphics", rc::PsoKind::graphics);
    rc::set_kind(rc::PsoKind::graphics);
    if (reason) rc::refused(reason);
    return call.finish(hr);
}

long inner_compute(long hr, const char* reason) {
    rc::PsoCall call("inner compute", rc::PsoKind::compute);
    rc::set_kind(rc::PsoKind::compute);
    if (reason) rc::refused(reason);
    return call.finish(hr);
}

long slot57_like(const char* factory_reason, const char* fallback_reason, bool fallback_ok) {
    rc::PsoCall call("slot 57", rc::PsoKind::graphics);
    long hr = inner_graphics(kNotImpl, factory_reason);
    if (hr == kNotImpl) {
        const std::string first = rc::take_reason();
        hr = fallback_ok ? inner_graphics(0, nullptr) : inner_graphics(kInvalidArg, fallback_reason);
        if (hr < 0) {
            const std::string second = rc::take_reason();
            rc::refused(first + "; then: " + (second.empty() ? rc::unexplained("fallback", hr) : second));
        } else {
            rc::count_path("slot 57 fallback");
        }
    }
    return call.finish(hr);
}

}  // namespace

int main() {
    auto& c = rc::counters();

    inner_graphics(0, nullptr);
    check(value(c.pso_graphics_ok) == 1 && value(c.pso_graphics_failed) == 0, "a lone successful call counts once");

    slot57_like("mesh refused", "no VS", false);
    check(value(c.pso_graphics_failed) == 1, "a nested failure counts once, at the outer call");
    check(entry("slot 57").second == 1 && entry("inner graphics").second == 0, "only the outer entry is counted");
    check(reason_count("mesh refused; then: no VS") == 1, "a retried failure keeps both reasons");
    check(reason_count("mesh refused") == 0 && reason_count("no VS") == 0, "the parts are not counted on their own");

    slot57_like("plan missing", nullptr, true);
    check(value(c.pso_graphics_ok) == 2 && entry("slot 57").first == 1, "a retry that builds is one success");
    check(reason_count("plan missing") == 0, "the reason of a call that succeeds is dropped");
    check(entry("slot 57 fallback").first == 1, "the path that built it is listed");

    slot57_like(nullptr, nullptr, false);
    check(reason_count("slot 57: E_NOTIMPL; then: fallback returned 0x80070057 with no reason recorded") == 0 &&
              reason_count("; then: fallback returned 0x80070057 with no reason recorded") == 1,
          "a failure with no reason is named by its HRESULT");

    inner_graphics(kInvalidArg, nullptr);
    check(reason_count("inner graphics returned 0x80070057 with no reason recorded") == 1,
          "a lone failure with no reason is named by its entry and HRESULT");

    {
        rc::PsoCall stream("stream", rc::PsoKind::unknown);
        inner_compute(kInvalidArg, "workgroup memory");
        stream.finish(kInvalidArg);
    }
    check(value(c.pso_compute_failed) == 1 && value(c.pso_graphics_failed) == 3,
          "an entry of unknown kind takes the kind of the PSO built below it");
    check(reason_count("workgroup memory") == 1, "the inner reason is the outer call's");

    {
        rc::PsoCall stream("stream", rc::PsoKind::unknown);
        rc::refused("first reason");
        rc::refused("second reason");
        stream.finish(kInvalidArg);
    }
    check(reason_count("first reason") == 1 && reason_count("second reason") == 0, "the first reason given wins");
    check(value(c.pso_graphics_failed) == 4, "a stream that never reached a PSO counts as graphics");

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([t] {
            for (int i = 0; i < 1000; ++i) {
                if (t & 1) slot57_like("thread reason", "thread fallback", false);
                else inner_compute(0, nullptr);
            }
        });
    for (auto& t : threads) t.join();
    check(reason_count("thread reason; then: thread fallback") == 2000, "reasons stay with their own thread");
    check(value(c.pso_compute_ok) == 2000, "concurrent successes are counted");

    check(value(c.pso_graphics_failed) + value(c.pso_compute_failed) == rc::failed_with_reason(),
          "every failed call has exactly one reason");

    rc::refused("outside any call");
    check(rc::failed_with_reason() == value(c.pso_graphics_failed) + value(c.pso_compute_failed) + 1,
          "a reason outside a call is still counted (and reported as a mismatch)");

    rc::report(0.0);
    std::printf("pso_accounting_test: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
