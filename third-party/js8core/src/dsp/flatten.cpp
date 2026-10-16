#include "js8core/dsp/flatten.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <memory>
#include "js8core/compat/numbers.hpp"
#include <utility>
#include <vector>

#include <vendor/Eigen/Dense>

namespace js8core::dsp {

/******************************************************************************/
// Flatten Constants
/******************************************************************************/

namespace {
  constexpr auto FLATTEN_DEGREE = 5;
  constexpr auto FLATTEN_SAMPLE = 10;

  static_assert(FLATTEN_DEGREE & 1, "Degree must be odd");
  static_assert(FLATTEN_SAMPLE >= 0 && FLATTEN_SAMPLE <= 100, "Sample must be a percentage");

  inline std::array<double, (FLATTEN_DEGREE + 1) / 2> make_nodes() {
    constexpr auto COUNT = (FLATTEN_DEGREE + 1) / 2;
    std::array<double, COUNT> nodes{};
    auto pi = std::numbers::pi;
    auto i = 0;
    std::generate(nodes.begin(), nodes.end(), [&]() { return std::cos(pi * ((2 * i++ + 1.0) / (2 * COUNT))); });
    return nodes;
  }

  const auto FLATTEN_NODES = make_nodes();
}

/******************************************************************************/
// Implementations
/******************************************************************************/

class Flatten::Impl
{
  std::vector<float>          m_work1;
  std::vector<float>          m_work2;
  std::vector<float>          m_work3;
  Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic> m_matrix;

public:
  explicit Impl(std::size_t n = 0)
  {
    reinit(n);
  }

  void reinit(std::size_t n)
  {
    m_work1.resize(n);
    m_work2.resize(n);
    m_work3.resize(n);
    m_matrix.resize(n, FLATTEN_DEGREE + 1);
  }

  void operator()(float * data,
                  std::size_t size)
  {
    if (size != static_cast<std::size_t>(m_matrix.rows()))
    {
      reinit(size);
    }

    // We're asked to flatten the data; by this point, it should
    // already be in dB, so it's acceptable to assume log values
    // from here on forward.

    std::copy(data, data + size, m_work1.begin());
    std::vector<std::size_t> iv(size);
    std::iota(iv.begin(), iv.end(), std::size_t{0});

    // Fit a polynomial to the percentile sampling of the data, and then
    // smooth the baseline via convolution with a flat window filter. As
    // with other parts of this, the degree of the polynomial is tunable,
    // as is the percentile used to sample the input data for the fit.

    for (auto i = std::size_t{0}; i != iv.size(); ++i)
    {
      // sample at the 10th percentile of the span, and use that for the fit
      iv[i] = iv.size() * FLATTEN_SAMPLE / 100 + i;
      if (iv[i] >= iv.size()) iv[i] -= iv.size();
      m_work2[i] = m_work1[iv[i]];
    }

    // The Chebyshev nodes are scaled to the span and used to generate the
    // matrix on which we'll perform a least squares solution to find the
    // coefficients of our polynomial fit.

    for (int i = 0; i < m_matrix.rows(); ++i)
    {
      auto node = 0.5 * (FLATTEN_NODES[i * FLATTEN_NODES.size() / size] + 1.0) * (size - 1);
      double x = 1.0;
      for (int j = 0; j <= FLATTEN_DEGREE; ++j)
      {
        m_matrix(i, j) = static_cast<float>(x);
        x *= node;
      }
    }

    Eigen::Matrix<float, Eigen::Dynamic, 1> b(size);
    for (int i = 0; i < b.rows(); ++i) b(i) = m_work2[i];

    auto coeff = m_matrix.colPivHouseholderQr().solve(b);

    // Evaluate polynomial across the span and subtract it from the original.

    for (int i = 0; i < m_matrix.rows(); ++i)
    {
      double x = 1.0;
      double y = 0.0;
      for (int j = 0; j <= FLATTEN_DEGREE; ++j)
      {
        y += coeff(j) * x;
        x *= i;
      }
      m_work3[i] = static_cast<float>(y);
    }

    // baseline-smooth using a flat window of width 3
    for (std::size_t i = 1; i + 1 < m_work3.size(); ++i)
    {
      m_work3[i] = (m_work3[i - 1] + m_work3[i] + m_work3[i + 1]) / 3.0f;
    }

    // flatten
    for (std::size_t i = 0; i < size; ++i)
    {
      data[i] -= m_work3[i];
    }
  }
};

Flatten::Flatten(bool value)
  : impl_(value ? std::make_unique<Impl>() : nullptr)
{
}

Flatten::~Flatten() = default;

void
Flatten::operator()(bool value)
{
  if (value && !impl_)
    {
      impl_ = std::make_unique<Impl> ();
    }
  else if (!value)
    {
      impl_.reset ();
    }
}

void
Flatten::operator()(float     * data,
                    std::size_t size)
{
  if (impl_)
    {
      (*impl_)(data, size);
    }
}

}  // namespace js8core::dsp
