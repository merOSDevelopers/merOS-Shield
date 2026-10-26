// merOS Shield - MSE iz arabellegi (track buffer)
// SourceBuffer'a eklenen ornekler burada saniye cinsinden, cozum (dts) sirasina gore tutulur.
// JS is parcacigi ekler/siler, oynatici is parcacigi okur: tum erisim kilitlidir.
#pragma once
#include "mp4.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <utility>
#include <vector>

namespace mer {
namespace media {

struct BufSample {
    double pts = 0, dts = 0, dur = 0;
    bool key = false;
    std::shared_ptr<std::vector<uint8_t>> data;
};

using Ranges = std::vector<std::pair<double, double>>;

class TrackBuffer {
public:
    explicit TrackBuffer(bool video) : video_(video) {}
    bool IsVideo() const { return video_; }

    void SetConfig(const TrackConfig& c) {
        std::lock_guard<std::mutex> g(mu_);
        cfg_ = c;
        cfgGen_++;
    }
    TrackConfig Config(int* gen = nullptr) const {
        std::lock_guard<std::mutex> g(mu_);
        if (gen) *gen = cfgGen_;
        return cfg_;
    }
    int ConfigGen() const { std::lock_guard<std::mutex> g(mu_); return cfgGen_; }

    // Kodlanmis kare isleme (basitlestirilmis MSE "coded frame processing"):
    // yeni grubun kapsadigi sunum araligindaki eski ornekler (ve onlara bagli kareler) silinir.
    void Insert(std::vector<BufSample>& group) {
        if (group.empty()) return;
        std::lock_guard<std::mutex> g(mu_);
        double a = 1e300, b = -1e300;
        for (auto& s : group) { a = std::min(a, s.pts); b = std::max(b, s.pts + s.dur); }
        if (!s_.empty() && MaxPts() > a + 1e-3) RemoveLocked(a, b, false);   // ortusme: eskiyi degistir
        for (auto& x : group) {
            bytes_ += x.data ? x.data->size() : 0;
            if (s_.empty() || s_.back().dts <= x.dts) s_.push_back(std::move(x));
            else {
                auto it = std::upper_bound(s_.begin(), s_.end(), x.dts, [](double d, const BufSample& e) { return d < e.dts; });
                s_.insert(it, std::move(x));
            }
        }
        dirty_ = true;
        gen_++;
    }

    void Remove(double a, double b) {
        std::lock_guard<std::mutex> g(mu_);
        RemoveLocked(a, b, true);
    }

    void Clear() {
        std::lock_guard<std::mutex> g(mu_);
        s_.clear();
        bytes_ = 0;
        dirty_ = true;
        gen_++;
    }

    Ranges Buffered() const {
        std::lock_guard<std::mutex> g(mu_);
        if (dirty_) {
            ranges_.clear();
            std::vector<std::pair<double, double>> iv;
            iv.reserve(s_.size());
            for (auto& s : s_) iv.push_back({s.pts, s.pts + s.dur});
            std::sort(iv.begin(), iv.end());
            const double tol = 0.12;
            for (auto& v : iv) {
                if (!ranges_.empty() && v.first <= ranges_.back().second + tol) ranges_.back().second = std::max(ranges_.back().second, v.second);
                else ranges_.push_back(v);
            }
            dirty_ = false;
        }
        return ranges_;
    }

    size_t Bytes() const { std::lock_guard<std::mutex> g(mu_); return bytes_; }
    uint64_t Gen() const { std::lock_guard<std::mutex> g(mu_); return gen_; }

    // dts'si 'afterDts'den buyuk (inclusive ise buyuk-esit) ilk ornek
    bool Next(double afterDts, bool inclusive, BufSample& out) const {
        std::lock_guard<std::mutex> g(mu_);
        auto it = inclusive
            ? std::lower_bound(s_.begin(), s_.end(), afterDts, [](const BufSample& e, double d) { return e.dts < d - 1e-7; })
            : std::upper_bound(s_.begin(), s_.end(), afterDts, [](double d, const BufSample& e) { return d + 1e-7 < e.dts; });
        if (it == s_.end()) return false;
        out = *it;
        return true;
    }

    // Arama icin: sunum zamani t'yi iceren (ya da t'den once gelen en yakin) anahtar karenin dts'si
    bool SeekPoint(double t, double& dts, double& pts) const {
        std::lock_guard<std::mutex> g(mu_);
        bool found = false;
        double bestPts = -1e300;
        for (auto& s : s_) {
            if (!s.key) continue;
            if (s.pts <= t + 1e-4 && s.pts > bestPts) { bestPts = s.pts; dts = s.dts; pts = s.pts; found = true; }
        }
        if (!found) {
            // t'den sonra baslayan ilk anahtar kare (t tamponlanmis araligin hemen oncesindeyse)
            double best = 1e300;
            for (auto& s : s_) if (s.key && s.pts >= t && s.pts < best && s.pts - t < 0.5) { best = s.pts; dts = s.dts; pts = s.pts; found = true; }
        }
        return found;
    }

    // t aninda veri var mi (tampon araligi icinde)
    bool Covers(double t, double slack = 0.12) const {
        for (auto& r : Buffered()) if (t >= r.first - slack && t < r.second) return true;
        return false;
    }
    // t'den itibaren kesintisiz tampon sonu
    double EndFrom(double t) const {
        for (auto& r : Buffered()) if (t >= r.first - 0.12 && t <= r.second) return r.second;
        return t;
    }
    double LastEnd() const {
        auto r = Buffered();
        return r.empty() ? 0 : r.back().second;
    }

private:
    double MaxPts() const {
        double m = -1e300;
        // son birkac ornek yeterli (B-kareler yuzunden sondan bir onceki daha buyuk olabilir)
        size_t n = s_.size();
        for (size_t i = n > 8 ? n - 8 : 0; i < n; i++) m = std::max(m, s_[i].pts + s_[i].dur);
        return m;
    }
    void RemoveLocked(double a, double b, bool) {
        if (s_.empty()) return;
        std::vector<BufSample> keep;
        keep.reserve(s_.size());
        bool dropDeps = false;
        for (auto& s : s_) {
            bool inRange = s.pts >= a - 1e-6 && s.pts < b - 1e-6;
            if (inRange) { bytes_ -= s.data ? s.data->size() : 0; dropDeps = video_; continue; }
            if (dropDeps) {
                if (s.key) dropDeps = false;
                else if (s.pts >= b - 1e-6) { bytes_ -= s.data ? s.data->size() : 0; continue; }
            }
            keep.push_back(std::move(s));
        }
        s_.swap(keep);
        dirty_ = true;
        gen_++;
    }

    bool video_;
    mutable std::mutex mu_;
    std::vector<BufSample> s_;
    TrackConfig cfg_;
    int cfgGen_ = 0;
    size_t bytes_ = 0;
    uint64_t gen_ = 0;
    mutable bool dirty_ = true;
    mutable Ranges ranges_;
};

inline Ranges IntersectRanges(const Ranges& a, const Ranges& b) {
    Ranges r;
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        double s = std::max(a[i].first, b[j].first), e = std::min(a[i].second, b[j].second);
        if (s < e) r.push_back({s, e});
        if (a[i].second < b[j].second) i++; else j++;
    }
    return r;
}

} // namespace media
} // namespace mer
