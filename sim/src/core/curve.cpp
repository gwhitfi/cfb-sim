#include "cfbsim/core/curve.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace cfbsim {

Curve Curve::linear(std::vector<std::pair<double, double>> points) {
    if (points.empty()) throw std::invalid_argument("linear curve needs at least one point");
    if (!std::is_sorted(points.begin(), points.end(),
                        [](const auto& a, const auto& b) { return a.first < b.first; })) {
        throw std::invalid_argument("linear curve points must be sorted by input");
    }
    Curve c;
    c.kind_ = Kind::Linear;
    c.points_ = std::move(points);
    return c;
}

Curve Curve::logistic(double mid, double steepness, double lo, double hi) {
    Curve c;
    c.kind_ = Kind::Logistic;
    c.mid_ = mid;
    c.steepness_ = steepness;
    c.lo_ = lo;
    c.hi_ = hi;
    return c;
}

Curve Curve::power(double exponent, double lo, double hi) {
    Curve c;
    c.kind_ = Kind::Power;
    c.exponent_ = exponent;
    c.lo_ = lo;
    c.hi_ = hi;
    return c;
}

double Curve::operator()(double r) const {
    switch (kind_) {
        case Kind::Linear: {
            if (r <= points_.front().first) return points_.front().second;
            if (r >= points_.back().first) return points_.back().second;
            // Find the segment [a, b] that contains r and interpolate along it.
            for (std::size_t i = 1; i < points_.size(); ++i) {
                const auto& [x1, y1] = points_[i];
                if (r <= x1) {
                    const auto& [x0, y0] = points_[i - 1];
                    return y0 + (y1 - y0) * (r - x0) / (x1 - x0);
                }
            }
            return points_.back().second;  // not reached
        }
        case Kind::Logistic:
            return lo_ + (hi_ - lo_) * cfbsim::logistic(steepness_ * (r - mid_));
        case Kind::Power:
            return lo_ + (hi_ - lo_) * std::pow(std::clamp(r, 0.0, 100.0) / 100.0, exponent_);
    }
    return 0.0;  // not reached: every Kind is handled above
}

double Blend::operator()(const Attributes& ratings) const {
    double sum = 0.0;
    double total = 0.0;
    for (const auto& [attr, weight] : weights) {
        sum += ratings[attr] * weight;
        total += weight;
    }
    return total > 0.0 ? sum / total : 0.0;
}

double logistic(double x) { return 1.0 / (1.0 + std::exp(-x)); }

double contest_probability(double skill_a, double skill_b, double scale) {
    return logistic((skill_a - skill_b) / scale);
}

}  // namespace cfbsim
