// Response curves, blends, and opposed contests: the small toolkit that turns
// ratings into outcomes (docs/cfbsim_design.md#response-curves-and-contests).
// Every curve's shape comes from tuning data, never from code.
#pragma once

#include <utility>
#include <vector>

#include "cfbsim/data/attributes.h"

namespace cfbsim {

// One named curve type, evaluated at an input r (usually a 1-100 rating).
//
//   Linear:   straight lines between `points` (sorted by input), flat beyond
//             the first and last point. Two points give y = a + b*r.
//   Logistic: lo + (hi - lo) / (1 + e^(-steepness * (r - mid)))   (an S-curve)
//   Power:    lo + (hi - lo) * (r / 100)^exponent
class Curve {
  public:
    enum class Kind { Linear, Logistic, Power };

    static Curve linear(std::vector<std::pair<double, double>> points);
    static Curve logistic(double mid, double steepness, double lo = 0.0, double hi = 1.0);
    static Curve power(double exponent, double lo = 0.0, double hi = 1.0);

    // A default-constructed Curve is the constant 0 (linear through one point).
    Curve() = default;

    double operator()(double r) const;

    Kind kind() const { return kind_; }

  private:
    Kind kind_ = Kind::Linear;
    std::vector<std::pair<double, double>> points_{{0.0, 0.0}};
    double mid_ = 50.0;
    double steepness_ = 0.1;
    double exponent_ = 1.0;
    double lo_ = 0.0;
    double hi_ = 1.0;
};

// A skill value made from several ratings: a weighted average, for example
// route running = 0.6 x Route Running + 0.4 x Playbook Knowledge.
struct Blend {
    std::vector<std::pair<Attr, double>> weights;

    double operator()(const Attributes& ratings) const;
};

// 1 / (1 + e^-x)
double logistic(double x);

// Probability that side A wins an opposed contest: equal skill gives 0.5, and
// `scale` sets how much a 10-point edge matters (smaller = more dominant).
double contest_probability(double skill_a, double skill_b, double scale);

}  // namespace cfbsim
