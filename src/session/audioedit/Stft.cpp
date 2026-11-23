#include "session/audioedit/Stft.h"

#include <algorithm>

namespace dojo::audioedit
{

RealFft::RealFft(int order)
    : fft_(order)
    , size_(1 << order)
    , work_(static_cast<std::size_t>(2 * (1 << order)), 0.0f)
{
}

void RealFft::forward(const float* in, std::complex<float>* out)
{
    std::copy(in, in + size_, work_.begin());
    std::fill(work_.begin() + size_, work_.end(), 0.0f);
    fft_.performRealOnlyForwardTransform(work_.data(), true);
    for (int k = 0; k < bins(); ++k)
        out[k] = {work_[static_cast<std::size_t>(2 * k)], work_[static_cast<std::size_t>(2 * k + 1)]};
}

void RealFft::inverse(const std::complex<float>* in, float* out)
{
    std::fill(work_.begin(), work_.end(), 0.0f);
    for (int k = 0; k < bins(); ++k)
    {
        work_[static_cast<std::size_t>(2 * k)] = in[k].real();
        work_[static_cast<std::size_t>(2 * k + 1)] = in[k].imag();
    }
    fft_.performRealOnlyInverseTransform(work_.data());
    std::copy(work_.begin(), work_.begin() + size_, out);
}

std::vector<float> hannWindow(int size)
{
    std::vector<float> w(static_cast<std::size_t>(size));
    for (int i = 0; i < size; ++i)
        w[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / size));
    return w;
}

int orderFor(double samples) noexcept
{
    int order = 4;
    while ((1 << order) < samples && order < 16)
        ++order;
    return order;
}

} // namespace dojo::audioedit
