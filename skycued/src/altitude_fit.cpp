#include "altitude_fit.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

namespace skycued {

namespace {

constexpr std::chrono::seconds kCacheSpan{5};
constexpr double kMinRejectFt = 150.0;
constexpr double kMadScale = 1.4826;
constexpr double kRejectMads = 3.0;

struct Point {
    double baro, diff;
    std::size_t n;  // samples behind this point
    AltitudeFit::Clock::time_point newest;
};

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const std::size_t m = v.size() / 2;
    return v.size() % 2 ? v[m] : (v[m - 1] + v[m]) / 2.0;
}

struct Line {
    double a = 0, b = 0;
    bool linear = false;
};

double medianDiff(const std::vector<Point>& pts) {
    std::vector<double> d;
    d.reserve(pts.size());
    for (const auto& p : pts) d.push_back(p.diff);
    return median(std::move(d));
}

// Linear fit when the data allow it (count, span, slope range), else the median.
Line fitLine(const std::vector<Point>& pts, const AltitudeFitConfig& cfg) {
    Line med;
    med.a = medianDiff(pts);
    std::size_t samples = 0;
    double lo = pts.front().baro, hi = lo;
    for (const auto& p : pts) {
        samples += p.n;
        lo = std::min(lo, p.baro);
        hi = std::max(hi, p.baro);
    }
    if (pts.size() < cfg.min_aircraft || samples < cfg.min_samples ||
        hi - lo < static_cast<double>(cfg.min_span_ft)) {
        return med;
    }
    double xm = 0, ym = 0;
    for (const auto& p : pts) {
        xm += p.baro;
        ym += p.diff;
    }
    xm /= static_cast<double>(pts.size());
    ym /= static_cast<double>(pts.size());
    double sxx = 0, sxy = 0;
    for (const auto& p : pts) {
        sxx += (p.baro - xm) * (p.baro - xm);
        sxy += (p.baro - xm) * (p.diff - ym);
    }
    if (sxx <= 0) return med;
    const double b = sxy / sxx;
    if (b < cfg.slope_min || b > cfg.slope_max) return med;
    Line l;
    l.b = b;
    l.a = ym - b * xm;
    l.linear = true;
    return l;
}

double residual(const Line& l, const Point& p) { return p.diff - (l.a + l.b * p.baro); }

}  // namespace

AltitudeFit::AltitudeFit(AltitudeFitConfig cfg) : cfg_(cfg) {}

void AltitudeFit::add(uint64_t aircraft_key, Clock::time_point t, int32_t baro_ft,
                      int32_t diff_ft) {
    auto& q = samples_[aircraft_key];
    // Keep the deque in time order (it is small), so a wall clock that stepped
    // back still stores samples. One sample per bucket: a time closer than
    // `bucket` to a neighbour is dropped.
    auto pos = std::upper_bound(q.begin(), q.end(), t,
                                [](Clock::time_point v, const Sample& s) { return v < s.t; });
    if (pos != q.begin() && t - std::prev(pos)->t < cfg_.bucket) return;
    if (pos != q.end() && pos->t - t < cfg_.bucket) return;
    q.insert(pos, {t, baro_ft, diff_ft});
    if (q.size() > kMaxPerAircraft) q.pop_front();
}

std::size_t AltitudeFit::sampleCount() const {
    std::size_t n = 0;
    for (const auto& kv : samples_) n += kv.second.size();
    return n;
}

std::optional<FitResult> AltitudeFit::fit(Clock::time_point now) {
    if (cache_valid_ && now >= cache_at_ && now - cache_at_ < kCacheSpan) return cache_;

    const auto cutoff = now - cfg_.window;
    std::vector<Point> pts;
    for (auto it = samples_.begin(); it != samples_.end();) {
        auto& q = it->second;
        // Stale: older than the window, or from beyond `now` by more than the
        // window (a forward clock jump that was stepped back afterwards).
        q.erase(std::remove_if(q.begin(), q.end(),
                               [&](const Sample& s) { return s.t < cutoff || s.t > now + cfg_.window; }),
                q.end());
        if (q.empty()) {
            it = samples_.erase(it);
            continue;
        }
        std::vector<double> b, d;
        for (const auto& s : q) {
            b.push_back(s.baro_ft);
            d.push_back(s.diff_ft);
        }
        pts.push_back({median(std::move(b)), median(std::move(d)), q.size(), q.back().t});
        ++it;
    }

    std::optional<FitResult> out;
    if (!pts.empty()) {
        Line line = fitLine(pts, cfg_);
        // One MAD rejection pass, then refit on what is kept.
        std::vector<double> absres;
        absres.reserve(pts.size());
        for (const auto& p : pts) absres.push_back(std::fabs(residual(line, p)));
        const double limit = std::max(kMinRejectFt, kRejectMads * kMadScale * median(absres));
        std::vector<Point> kept;
        for (const auto& p : pts) {
            if (std::fabs(residual(line, p)) <= limit) kept.push_back(p);
        }
        if (kept.size() != pts.size() && !kept.empty()) {
            pts = std::move(kept);
            line = fitLine(pts, cfg_);
        }
        FitResult r;
        r.a = line.a;
        r.b = line.b;
        r.linear = line.linear;
        r.aircraft = pts.size();
        std::vector<double> res;
        for (const auto& p : pts) {
            r.samples += p.n;
            r.newest = std::max(r.newest, p.newest);
            res.push_back(std::fabs(residual(line, p)));
        }
        std::sort(res.begin(), res.end());
        const auto rank = static_cast<std::size_t>(std::ceil(0.9 * static_cast<double>(res.size())));
        r.p90_ft = res[std::max<std::size_t>(rank, 1) - 1];
        if (r.linear || r.aircraft >= cfg_.min_median_aircraft) out = r;
    }
    cache_valid_ = true;
    cache_at_ = now;
    cache_ = out;
    return out;
}

}  // namespace skycued
