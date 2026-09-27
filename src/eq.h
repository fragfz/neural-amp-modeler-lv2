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
			UpdateActivity();
		}

		void SetBass(float db)
		{
			bassDb_ = db;

			UpdateBass();
			UpdateActivity();
		}

		void SetMid(float db)
		{
			midDb_ = db;

			UpdateMid();
			UpdateActivity();
		}

		void SetTreble(float db)
		{
			trebleDb_ = db;

			UpdateTreble();
			UpdateActivity();
		}

		float Process(float in)
		{
			// flat-skip: when all knobs are ~0 dB the EQ is a bit-exact wire
			// (same optimization as the tone3000 plugin's BlockEq)
			if (!anyBandActive_)
				return in;

			return (float)treble_(mid_(bass_((double)in)));
		}

	private:
		struct Biquad
		{
			double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
			double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

			void ResetState()
			{
				x1 = x2 = y1 = y2 = 0.0;
			}

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

			const double a0 = (A + 1.0) + (A - 1.0) * cw + s2a;

			filter.b0 = A * ((A + 1.0) - (A - 1.0) * cw + s2a) / a0;
			filter.b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cw) / a0;
			filter.b2 = A * ((A + 1.0) - (A - 1.0) * cw - s2a) / a0;
			filter.a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cw) / a0;
			filter.a2 = ((A + 1.0) + (A - 1.0) * cw - s2a) / a0;
		}

		static void Peaking(Biquad& filter, double sampleRate, double freq, double dbGain, double q)
		{
			const double A = pow(10.0, dbGain / 40.0);

			const double w0 = 2.0 * M_PI * freq / sampleRate;
			const double alpha = sin(w0) / (2.0 * q);
			const double cw = cos(w0);

			const double a0 = 1.0 + alpha / A;

			filter.b0 = (1.0 + alpha * A) / a0;
			filter.b1 = -2.0 * cw / a0;
			filter.b2 = (1.0 - alpha * A) / a0;
			filter.a1 = -2.0 * cw / a0;
			filter.a2 = (1.0 - alpha / A) / a0;
		}

		static void HighShelf(Biquad& filter, double sampleRate, double freq, double dbGain)
		{
			const double A = pow(10.0, dbGain / 40.0);

			const double w0 = 2.0 * M_PI * freq / sampleRate;
			const double alpha = sin(w0) / 2.0 * sqrt((A + 1.0 / A) * (1.0 / 0.9 - 1.0) + 2.0);
			const double cw = cos(w0);
			const double s2a = 2.0 * sqrt(A) * alpha;

			const double a0 = (A + 1.0) - (A - 1.0) * cw + s2a;

			filter.b0 = A * ((A + 1.0) + (A - 1.0) * cw + s2a) / a0;
			filter.b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cw) / a0;
			filter.b2 = A * ((A + 1.0) + (A - 1.0) * cw - s2a) / a0;
			filter.a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cw) / a0;
			filter.a2 = ((A + 1.0) - (A - 1.0) * cw - s2a) / a0;
		}

		void UpdateBass() { LowShelf(bass_, sampleRate_, 150.0, (double)bassDb_); }
		void UpdateMid() { Peaking(mid_, sampleRate_, 900.0, (double)midDb_, 1.0); }
		void UpdateTreble() { HighShelf(treble_, sampleRate_, 2500.0, (double)trebleDb_); }

		// a band counts as active only when its knob is meaningfully off 0 dB;
		// while no band is active the EQ is bypassed entirely (flat-skip).
		// Coming back from the skip path the biquad state is stale, so clear it
		// to keep the first processed block from ringing with old history.
		void UpdateActivity()
		{
			const bool active = (fabs(bassDb_) >= kFlatThresholdDb)
				|| (fabs(midDb_) >= kFlatThresholdDb)
				|| (fabs(trebleDb_) >= kFlatThresholdDb);

			if (active && !anyBandActive_)
			{
				bass_.ResetState();
				mid_.ResetState();
				treble_.ResetState();
			}

			anyBandActive_ = active;
		}

		static constexpr float kFlatThresholdDb = 0.05f;

		float sampleRate_ = 48000.0f;
		float bassDb_ = 0.0f;
		float midDb_ = 0.0f;
		float trebleDb_ = 0.0f;
		bool anyBandActive_ = false;

		Biquad bass_;
		Biquad mid_;
		Biquad treble_;
	};
}

#endif // ENABLE_EQ
