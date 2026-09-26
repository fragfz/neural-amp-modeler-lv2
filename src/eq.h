#pragma once

#ifdef ENABLE_EQ

#include <cmath>

namespace NAM {

	// Simple one-channel 3-band EQ: bass low-shelf (~150 Hz), mid peaking
	// (~900 Hz, Q 1.0), treble high-shelf (~2500 Hz). Three cascaded RBJ
	// biquads; coefficients are recomputed on parameter changes (never on
	// the audio thread's hot path), state lives in the biquads themselves.
	class Eq
	{
	public:
		void Init(float sampleRate)
		{
			sampleRate_ = sampleRate;

			UpdateBass();
			UpdateMid();
			UpdateTreble();
		}

		void SetBass(float db)
		{
			bassDb_ = db;

			UpdateBass();
		}

		void SetMid(float db)
		{
			midDb_ = db;

			UpdateMid();
		}

		void SetTreble(float db)
		{
			trebleDb_ = db;

			UpdateTreble();
		}

		float Process(float in)
		{
			return (float)treble_(mid_(bass_((double)in)));
		}

	private:
		struct Biquad
		{
			double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
			double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

			double operator()(double x)
			{
				const double y = (b0 * x) + (b1 * x1) + (b2 * x2) - (a1 * y1) - (a2 * y2);

				x2 = x1;
				x1 = x;
				y2 = y1;
				y1 = y;

				return y;
			}
		};

		static void LowShelf(Biquad& filter, double sampleRate, double freq, double dbGain)
		{
			const double A = pow(10.0, dbGain / 40.0); // sqrt of linear gain

			const double w0 = 2.0 * M_PI * freq / sampleRate;
			const double alpha = sin(w0) / 2.0 * sqrt((A + 1.0 / A) * (1.0 / 0.9 - 1.0) + 2.0); // Q from shelf slope
			const double cw = cos(w0);
			const double s2a = 2.0 * sqrt(A) * alpha;

			filter.b0 = A * ((A + 1.0) - (A - 1.0) * cw + s2a);
			filter.b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
			filter.b2 = A * ((A + 1.0) - (A - 1.0) * cw - s2a);
			filter.a1 = 2.0 * ((A - 1.0) + (A + 1.0) * cw);
			filter.a2 = (A + 1.0) + (A - 1.0) * cw - s2a;
		}

		static void Peaking(Biquad& filter, double sampleRate, double freq, double dbGain, double q)
		{
			const double A = pow(10.0, dbGain / 40.0);

			const double w0 = 2.0 * M_PI * freq / sampleRate;
			const double alpha = sin(w0) / (2.0 * q);
			const double cw = cos(w0);

			filter.b0 = 1.0 + alpha * A;
			filter.b1 = -2.0 * cw;
			filter.b2 = 1.0 - alpha * A;
			filter.a1 = 2.0 * cw;
			filter.a2 = 1.0 - alpha / A;
		}

		static void HighShelf(Biquad& filter, double sampleRate, double freq, double dbGain)
		{
			const double A = pow(10.0, dbGain / 40.0);

			const double w0 = 2.0 * M_PI * freq / sampleRate;
			const double alpha = sin(w0) / 2.0 * sqrt((A + 1.0 / A) * (1.0 / 0.9 - 1.0) + 2.0);
			const double cw = cos(w0);
			const double s2a = 2.0 * sqrt(A) * alpha;

			filter.b0 = A * ((A + 1.0) + (A - 1.0) * cw + s2a);
			filter.b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw);
			filter.b2 = A * ((A + 1.0) + (A - 1.0) * cw - s2a);
			filter.a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw);
			filter.a2 = (A + 1.0) - (A - 1.0) * cw - s2a;
		}

		void UpdateBass() { LowShelf(bass_, sampleRate_, 150.0, (double)bassDb_); }
		void UpdateMid() { Peaking(mid_, sampleRate_, 900.0, (double)midDb_, 1.0); }
		void UpdateTreble() { HighShelf(treble_, sampleRate_, 2500.0, (double)trebleDb_); }

		float sampleRate_ = 48000.0f;
		float bassDb_ = 0.0f;
		float midDb_ = 0.0f;
		float trebleDb_ = 0.0f;

		Biquad bass_;
		Biquad mid_;
		Biquad treble_;
	};
}

#endif // ENABLE_EQ
