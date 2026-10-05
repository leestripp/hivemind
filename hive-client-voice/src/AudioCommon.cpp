#include "AudioCommon.h"

#include <cmath>

namespace hv {

double chunkRms(const int16_t* samples, std::size_t n) {
    if (n == 0) return 0.0;
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = samples[i];
        sum += v * v;
    }
    return std::sqrt(sum / static_cast<double>(n));
}

}  // namespace hv
