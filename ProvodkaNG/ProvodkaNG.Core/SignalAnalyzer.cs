using System.Numerics;

namespace ProvodkaNG.Core;

public sealed record SpectrumSnapshot(
    double[] FrequenciesHz,
    double[] MagnitudesDb,
    double PeakFrequencyHz,
    double PeakMagnitudeDb,
    double NoiseFloorDb,
    double SampleRateHz);

/// <summary>
/// Pure C# Radix-2 Cooley-Tukey FFT analyzer with DC removal and Hann windowing
/// for real-time MWD signal spectrum visualization (0 .. Fs/2).
/// </summary>
public static class SignalAnalyzer
{
    public static SpectrumSnapshot ComputeSpectrum(ReadOnlySpan<double> samples, double sampleRateHz, int fftSize = 512)
    {
        if (fftSize < 32 || (fftSize & (fftSize - 1)) != 0)
            throw new ArgumentException("FFT size must be a power of two >= 32.", nameof(fftSize));

        if (sampleRateHz <= 0.0)
            sampleRateHz = 250.0;

        int n = Math.Min(samples.Length, fftSize);
        int start = Math.Max(0, samples.Length - n);

        // Compute mean to strip DC offset before windowing
        double mean = 0.0;
        int valid = 0;
        for (int i = 0; i < n; i++)
        {
            double v = samples[start + i];
            if (double.IsFinite(v))
            {
                mean += v;
                valid++;
            }
        }
        if (valid > 0) mean /= valid;

        var buf = new Complex[fftSize];
        double windowNorm = 0.0;
        for (int i = 0; i < n; i++)
        {
            double v = samples[start + i];
            if (!double.IsFinite(v)) v = mean;
            double w = 0.5 * (1.0 - Math.Cos(2.0 * Math.PI * i / Math.Max(1, n - 1)));
            buf[i] = new Complex((v - mean) * w, 0.0);
            windowNorm += w;
        }
        if (windowNorm < 1e-12) windowNorm = 1.0;

        FftInPlace(buf);

        int half = fftSize / 2;
        double[] freqs = new double[half];
        double[] magsDb = new double[half];

        double peakMag = -120.0;
        double peakFreq = 0.0;
        double sumDb = 0.0;

        // First compute linear magnitudes and normalize relative to full-scale peak or 1.0
        double maxLin = 1e-12;
        double[] lin = new double[half];
        for (int k = 0; k < half; k++)
        {
            double m = (2.0 * buf[k].Magnitude) / windowNorm;
            lin[k] = m;
            if (k >= 1 && m > maxLin) maxLin = m;
        }

        for (int k = 0; k < half; k++)
        {
            freqs[k] = k * sampleRateHz / fftSize;
            double ratio = Math.Max(lin[k] / maxLin, 1e-6);
            double db = 20.0 * Math.Log10(ratio); // 0 dB = peak, down to -120 dB
            magsDb[k] = db;
            sumDb += db;

            if (k >= 1 && db > peakMag)
            {
                peakMag = db;
                peakFreq = freqs[k];
            }
        }

        double noiseFloorDb = half > 0 ? sumDb / half : -120.0;
        return new SpectrumSnapshot(freqs, magsDb, peakFreq, peakMag, noiseFloorDb, sampleRateHz);
    }

    private static void FftInPlace(Complex[] buffer)
    {
        int n = buffer.Length;
        int bits = (int)Math.Log2(n);

        for (int i = 0; i < n; i++)
        {
            int j = BitReverse(i, bits);
            if (j > i)
            {
                (buffer[i], buffer[j]) = (buffer[j], buffer[i]);
            }
        }

        for (int len = 2; len <= n; len <<= 1)
        {
            double angle = -2.0 * Math.PI / len;
            var wlen = new Complex(Math.Cos(angle), Math.Sin(angle));
            int half = len >> 1;

            for (int i = 0; i < n; i += len)
            {
                Complex w = Complex.One;
                for (int j = 0; j < half; j++)
                {
                    Complex u = buffer[i + j];
                    Complex v = buffer[i + j + half] * w;
                    buffer[i + j] = u + v;
                    buffer[i + j + half] = u - v;
                    w *= wlen;
                }
            }
        }
    }

    private static int BitReverse(int x, int bits)
    {
        int r = 0;
        for (int i = 0; i < bits; i++)
        {
            r = (r << 1) | (x & 1);
            x >>= 1;
        }
        return r;
    }
}
