/*
  Streaming sample rate converter used to run the reverb chip at its own rate.

  The RRV-10 gate array takes 256 clock cycles per sample from an 8 MHz crystal, so the hardware runs at
  31250 Hz. Running the chip once per host sample (the plugin's original behaviour) makes every delay line
  and decay faster by hostRate / 31250: 1.41x at 44.1 kHz, 3.07x at 96 kHz.

  RateConverter converts a continuous stream from one rate to another with a Kaiser windowed sinc. When the
  output rate is lower than the input rate the filter is scaled to the output Nyquist, so it also removes
  what the hardware's input filter would. It keeps its own input history and fractional read position across
  calls, so no samples are dropped or repeated at block boundaries whatever the block size.
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

class RateConverter {
  static constexpr double kPi = 3.14159265358979323846;

public:
  // zeroCrossings: one-sided filter length in zero crossings of the (scaled) sinc. 16 gives a stopband well
  // below the 16-bit floor of the chip.
  void prepare(double inRate, double outRate, int zeroCrossings = 16, double passband = 0.9) {
    step = inRate / outRate; // input samples advanced per output sample
    cutoff = std::min(1.0, outRate / inRate) * passband;
    halfTaps = (int)std::ceil(zeroCrossings / cutoff);
    buildTable(zeroCrossings);
    history.assign((size_t)(2 * halfTaps + 2), 0.0f);
    writeIx = 0;
    filled = 0;
    // position of the next output sample, in input samples counted from the first input sample
    nextOutPos = 0.0;
    inCount = 0;
  }

  void reset() {
    std::fill(history.begin(), history.end(), 0.0f);
    writeIx = 0;
    filled = 0;
    nextOutPos = 0.0;
    inCount = 0;
  }

  // Delay added by the filter, in input samples.
  double latencyIn() const { return (double)halfTaps; }

  // Feed one input sample; appends every output sample that becomes computable to out.
  template <typename Out>
  void push(float x, Out &out) {
    history[writeIx] = x;
    writeIx = (writeIx + 1) % history.size();
    ++inCount;
    if (filled < history.size())
      ++filled;

    // An output at position p (delayed by halfTaps) needs inputs up to floor(p) + halfTaps + ... we produce
    // output for time t = nextOutPos using inputs [t - halfTaps, t + halfTaps], so it is ready when the
    // newest input index (inCount - 1) >= floor(t) + halfTaps.
    while ((double)(inCount - 1) >= std::floor(nextOutPos) + halfTaps) {
      out.push_back(compute(nextOutPos));
      nextOutPos += step;
    }
  }

private:
  static constexpr int tableRes = 512; // table points per zero crossing

  double step = 1.0;
  double cutoff = 1.0;
  int halfTaps = 16;
  std::vector<float> table; // windowed sinc over [0, zeroCrossings], tableRes points per crossing
  int tableZeroCrossings = 16;
  std::vector<float> history;
  size_t writeIx = 0;
  size_t filled = 0;
  double nextOutPos = 0.0;
  long long inCount = 0;

  static double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k) {
      term *= (x / (2.0 * k)) * (x / (2.0 * k));
      sum += term;
      if (term < sum * 1e-12)
        break;
    }
    return sum;
  }

  void buildTable(int zeroCrossings) {
    tableZeroCrossings = zeroCrossings;
    const double beta = 9.0;
    const double i0b = besselI0(beta);
    table.resize((size_t)(zeroCrossings * tableRes + 2));
    for (size_t i = 0; i < table.size(); ++i) {
      const double x = (double)i / tableRes; // in zero crossings
      double v;
      if (x >= zeroCrossings)
        v = 0.0;
      else {
        const double sinc = x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        const double r = x / zeroCrossings;
        v = sinc * besselI0(beta * std::sqrt(1.0 - r * r)) / i0b;
      }
      table[i] = (float)v;
    }
  }

  float kernel(double distanceInSamples) const {
    double x = std::fabs(distanceInSamples) * cutoff * tableRes;
    const size_t i = (size_t)x;
    if (i + 1 >= table.size())
      return 0.0f;
    const double f = x - (double)i;
    return (float)(table[i] + f * (table[i + 1] - table[i]));
  }

  // Input sample with absolute index n (0 = first ever input). Indices before the start read as silence.
  float inputAt(long long n) const {
    if (n < 0 || n >= inCount || inCount - n > (long long)filled)
      return 0.0f;
    const long long back = inCount - 1 - n; // 0 = newest
    const size_t size = history.size();
    const size_t ix = (writeIx + size - 1 - (size_t)back) % size;
    return history[ix];
  }

  float compute(double t) const {
    // output for time t, delayed by halfTaps so that it only needs past inputs
    const double center = t;
    const long long base = (long long)std::floor(center);
    double acc = 0.0;
    for (long long n = base - halfTaps + 1; n <= base + halfTaps; ++n)
      acc += (double)inputAt(n) * kernel(center - (double)n);
    return (float)(acc * cutoff);
  }
};

// Runs a per-sample stereo process at a fixed internal rate inside a host running at any rate:
// host -> internal rate -> process -> host. Output is delayed by a small constant (getLatency) so that every
// host block gets exactly as many samples as it put in.
class NativeRateRunner {
public:
  void prepare(double hostRate, double internalRate) {
    for (int c = 0; c < 2; ++c) {
      down[c].prepare(hostRate, internalRate);
      up[c].prepare(internalRate, hostRate);
    }
    const double ratio = hostRate / internalRate;
    latency = (int)std::ceil(down[0].latencyIn() + (up[0].latencyIn() + 2.0) * ratio) + 4;
    reset();
  }

  void reset() {
    for (int c = 0; c < 2; ++c) {
      down[c].reset();
      up[c].reset();
      fifo[c].assign((size_t)latency, 0.0f); // pre-fill: the constant delay that keeps the fifo from running dry
      fifoRead[c] = 0;
      nativeIn[c].clear();
    }
  }

  int getLatency() const { return latency; }

  // process(inL, inR, outL, outR) is called once per internal-rate sample.
  template <typename Process>
  void run(const float *inL, const float *inR, float *outL, float *outR, int n, Process &&process) {
    for (int i = 0; i < n; ++i) {
      nativeIn[0].clear();
      nativeIn[1].clear();
      down[0].push(inL[i], nativeIn[0]);
      down[1].push(inR[i], nativeIn[1]);
      // both channels share the same timing, so they produce the same number of samples
      for (size_t k = 0; k < nativeIn[0].size(); ++k) {
        float l, r;
        process(nativeIn[0][k], nativeIn[1][k], l, r);
        up[0].push(l, fifo[0]);
        up[1].push(r, fifo[1]);
      }
    }
    for (int c = 0; c < 2; ++c) {
      float *dst = c == 0 ? outL : outR;
      auto &q = fifo[c];
      size_t &rd = fifoRead[c];
      for (int i = 0; i < n; ++i)
        dst[i] = rd < q.size() ? q[rd++] : 0.0f;
      // drop consumed samples now and then so the buffer does not grow
      if (rd > 4096) {
        q.erase(q.begin(), q.begin() + (std::ptrdiff_t)rd);
        rd = 0;
      }
    }
  }

private:
  RateConverter down[2], up[2];
  std::vector<float> fifo[2];
  size_t fifoRead[2] = {0, 0};
  std::vector<float> nativeIn[2];
  int latency = 0;
};
