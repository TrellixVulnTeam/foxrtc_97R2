﻿/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

/*
 * The core AEC algorithm, which is presented with time-aligned signals.
 */

#include "webrtc/modules/audio_processing/aec/aec_core.h"

#include <algorithm>
#include <math.h>
#include <stddef.h>  // size_t
#include <stdlib.h>
#include <string.h>

#include "webrtc/base/checks.h"
extern "C" {
#include "webrtc/common_audio/ring_buffer.h"
#include "webrtc/common_audio/signal_processing/include/signal_processing_library.h"
}
#include "webrtc/base/checks.h"
//#include "webrtc/common_audio/signal_processing/include/signal_processing_library.h"
#include "webrtc/modules/audio_processing/aec/aec_common.h"
#include "webrtc/modules/audio_processing/aec/aec_core_optimized_methods.h"
#include "webrtc/modules/audio_processing/aec/aec_rdft.h"
#include "webrtc/modules/audio_processing/logging/apm_data_dumper.h"
#include "webrtc/modules/audio_processing/utility/delay_estimator_wrapper.h"
#include "webrtc/system_wrappers/include/cpu_features_wrapper.h"
#include "webrtc/system_wrappers/include/metrics.h"
#include "webrtc/typedefs.h"
#include "webrtc/modules/audio_processing/aec/aec_resampler.h"

namespace webrtc {
namespace {
enum class DelaySource {
  kSystemDelay,    // The delay values come from the OS.
  kDelayAgnostic,  // The delay values come from the DA-AEC.
};

constexpr int kMinDelayLogValue = -200;
constexpr int kMaxDelayLogValue = 200;
constexpr int kNumDelayLogBuckets = 100;

void MaybeLogDelayAdjustment(int moved_ms, DelaySource source) {
  if (moved_ms == 0)
    return;
  switch (source) {
    case DelaySource::kSystemDelay:
      RTC_HISTOGRAM_COUNTS("WebRTC.Audio.AecDelayAdjustmentMsSystemValue",
                           moved_ms, kMinDelayLogValue, kMaxDelayLogValue,
                           kNumDelayLogBuckets);
      return;
    case DelaySource::kDelayAgnostic:
      RTC_HISTOGRAM_COUNTS("WebRTC.Audio.AecDelayAdjustmentMsAgnosticValue",
                           moved_ms, kMinDelayLogValue, kMaxDelayLogValue,
                           kNumDelayLogBuckets);
      return;
  }
}

#ifdef AEC_OPTIMIZE
void MaybeLogDelayAdjustmentNew(int moved_ms, DelaySource source) {
	if (moved_ms == 0)
		return;
	switch (source) {
	case DelaySource::kSystemDelay:
		RTC_HISTOGRAM_COUNTS("WebRTC.Audio.AecDelayAdjustmentMsSystemValue",
			moved_ms, kMinDelayLogValue, kMaxDelayLogValue,
			kNumDelayLogBuckets);
		return;
	case DelaySource::kDelayAgnostic:
		RTC_HISTOGRAM_COUNTS("WebRTC.Audio.AecDelayAdjustmentMsAgnosticValue",
			moved_ms, kMinDelayLogValue, kMaxDelayLogValue,
			kNumDelayLogBuckets);
		return;
	}
}
#endif
}  // namespace

#ifdef AEC_OPTIMIZE
int AecNew::instance_count = 0;

// Estimates delay to set the position of the far-end buffer read pointer
// (controlled by knownDelay)

static void EstBufDelayNormal_New(AecNew* aecInst);
//static void EstBufDelayExtended_New(AecNew* aecInst);
static int ProcessNormal_New(AecNew* self,
	const float* const* near,
	size_t num_bands,
	float* const* out,
	size_t num_samples,
	int16_t reported_delay_ms,
	int32_t skew);

static void ProcessExtended_New(AecNew* self,
	const float* const* near,
	size_t num_bands,
	float* const* out,
	size_t num_samples,
	int16_t reported_delay_ms,
	int32_t skew);

void* WebRtcAec_Create_New() {
	AecNew* aecpc = new AecNew();

	if (!aecpc) {
		return NULL;
	}
	aecpc->data_dumper.reset(new ApmDataDumper(aecpc->instance_count));

	aecpc->aec = WebRtcAec_CreateAec_New(aecpc->instance_count);
	if (!aecpc->aec) {
		WebRtcAec_Free_New(aecpc);
		return NULL;
	}
	/*aecpc->resampler = WebRtcAec_CreateResampler();
	if (!aecpc->resampler) {
		WebRtcAec_Free_New(aecpc);
		return NULL;
	}*/
	// Create far-end pre-buffer. The buffer size has to be large enough for
	// largest possible drift compensation (kResamplerBufferSize) + "almost" an
	// FFT buffer (PART_LEN2 - 1).
	aecpc->far_pre_buf = WebRtc_CreateBuffer(PART_LEN2 + kResamplerBufferSize, sizeof(float));
	if (!aecpc->far_pre_buf) 
	{
		WebRtcAec_Free_New(aecpc);
		return NULL;
	}

	aecpc->initFlag = 0;

	aecpc->instance_count++;
	return aecpc;
}

void WebRtcAec_Free_New(void* aecInst) {
	AecNew* aecpc = reinterpret_cast<AecNew*>(aecInst);

	if (aecpc == NULL) {
		return;
	}

	WebRtc_FreeBuffer(aecpc->far_pre_buf);

	WebRtcAec_FreeAec_New(aecpc->aec);
	//WebRtcAec_FreeResampler(aecpc->resampler);
	delete aecpc;
}

#ifdef AEC_OPTIMIZE
AecNew::AecNew() = default;
AecNew::~AecNew() = default;
#endif
static const int sampMsNb = 8;
static const int kMaxBufSizeStart = 62;  // In partitions
static const int initCheck = 42;
int32_t WebRtcAec_Init_New(void* aecInst, int32_t sampFreq, int32_t scSampFreq)
{
	AecNew* aecpc = reinterpret_cast<AecNew*>(aecInst);
	aecpc->data_dumper->InitiateNewSetOfRecordings();
	AecConfigNew aecConfig;

	if (sampFreq != 8000 && sampFreq != 16000 && sampFreq != 32000 &&
		sampFreq != 48000) {
		return 12004;
	}
	aecpc->sampFreq = sampFreq;

	if (scSampFreq < 1 || scSampFreq > 96000) {
		return 12004;
	}
	aecpc->scSampFreq = scSampFreq;

	// Initialize echo canceller core
	if (WebRtcAec_InitAec_New(aecpc->aec, aecpc->sampFreq) == -1) {
		return 12000;
	}

	//if (WebRtcAec_InitResampler(aecpc->resampler, aecpc->scSampFreq) == -1) {
	//	return AEC_UNSPECIFIED_ERROR;
	//}

	WebRtc_InitBuffer(aecpc->far_pre_buf);
	WebRtc_MoveReadPtr(aecpc->far_pre_buf, -PART_LEN);  // Start overlap.

	aecpc->initFlag = initCheck;  // indicates that initialization has been done

	if (aecpc->sampFreq == 32000 || aecpc->sampFreq == 48000) {
		aecpc->splitSampFreq = 16000;
	}
	else {
		aecpc->splitSampFreq = sampFreq;
	}

	aecpc->delayCtr = 0;
	aecpc->sampFactor = (aecpc->scSampFreq * 1.0f) / aecpc->splitSampFreq;
	// Sampling frequency multiplier (SWB is processed as 160 frame size).
	aecpc->rate_factor = aecpc->splitSampFreq / 8000;

	aecpc->sum = 0;
	aecpc->counter = 0;
	aecpc->checkBuffSize = 1;
	aecpc->firstVal = 0;

	// We skip the startup_phase completely (setting to 0) if DA-AEC is enabled,
	// but not extended_filter mode.
	aecpc->startup_phase = WebRtcAec_extended_filter_enabled_New(aecpc->aec) ||
		!WebRtcAec_delay_agnostic_enabled_New(aecpc->aec);
	aecpc->bufSizeStart = 0;
	aecpc->checkBufSizeCtr = 0;
	aecpc->msInSndCardBuf = 0;
	aecpc->filtDelay = -1;  // -1 indicates an initialized state.
	aecpc->timeForDelayChange = 0;
	aecpc->knownDelay = 0;
	aecpc->lastDelayDiff = 0;

	aecpc->skewFrCtr = 0;
	aecpc->resample = kAecFalsenew;
	aecpc->highSkewCtr = 0;
	aecpc->skew = 0;

	aecpc->farend_started = 0;

	// Default settings.
	aecConfig.nlpMode = kAecNlpModeratenew;
	aecConfig.skewMode = kAecFalsenew;
	aecConfig.metricsMode = kAecFalsenew;
	aecConfig.delay_logging = kAecFalsenew;

	if (WebRtcAec_set_config_New(aecpc, aecConfig) == -1) {
		return AEC_UNSPECIFIED_ERROR;
	}

	return 0;
}

// Returns any error that is caused when buffering the
// far-end signal.
int32_t WebRtcAec_GetBufferFarendError_New(void* aecInst,
	const float* farend,
	size_t nrOfSamples) {
	AecNew* aecpc = reinterpret_cast<AecNew*>(aecInst);

	if (!farend)
		return AEC_NULL_POINTER_ERROR;

	if (aecpc->initFlag != initCheck)
		return AEC_UNINITIALIZED_ERROR;

	// number of samples == 160 for SWB input
	//if (nrOfSamples != 80 && nrOfSamples != 160)
	//  return AEC_BAD_PARAMETER_ERROR;

	return 0;
}

// only buffer L band for farend
int32_t WebRtcAec_BufferFarend_New(void* aecInst,
	const float* farend,
	size_t nrOfSamples) {
	AecNew* aecpc = reinterpret_cast<AecNew*>(aecInst);
	size_t newNrOfSamples = nrOfSamples;
	//float new_farend[MAX_RESAMP_LEN_NEW];
	const float* farend_ptr = farend;

	// Get any error caused by buffering the farend signal.
	int32_t error_code =
		WebRtcAec_GetBufferFarendError_New(aecInst, farend, nrOfSamples);

	if (error_code != 0)
		return error_code;

	//if (aecpc->skewMode == kAecTrue && aecpc->resample == kAecTrue) {
	//	// Resample and get a new number of samples
	//	WebRtcAec_ResampleLinear(aecpc->resampler, farend, nrOfSamples, aecpc->skew,
	//		new_farend, &newNrOfSamples);
	//	farend_ptr = new_farend;
	//}

	aecpc->farend_started = 1;
	WebRtcAec_SetSystemDelay_New(aecpc->aec, WebRtcAec_system_delay_New(aecpc->aec) +
		static_cast<int>(newNrOfSamples));

	// Write the time-domain data to |far_pre_buf|.
	WebRtc_WriteBuffer(aecpc->far_pre_buf, farend_ptr, newNrOfSamples);

	// TODO(minyue): reduce to |PART_LEN| samples for each buffering.
	while (WebRtc_available_read(aecpc->far_pre_buf) >= PART_LEN2) {
		// We have enough data to pass to the FFT, hence read PART_LEN2 samples.
		{
			float* ptmp = NULL;
			float tmp[PART_LEN2];
			WebRtc_ReadBuffer(aecpc->far_pre_buf,
				reinterpret_cast<void**>(&ptmp), tmp, PART_LEN2);
			WebRtcAec_BufferFarendBlock_New(aecpc->aec, &ptmp[PART_LEN]);
		}

		// Rewind |far_pre_buf| PART_LEN samples for overlap before continuing.
		WebRtc_MoveReadPtr(aecpc->far_pre_buf, -PART_LEN);
	}

	return 0;
}
static const int kMaxTrustedDelayMs = 500;
int32_t WebRtcAec_Process_New(void* aecInst,
	const float* const* nearend,
	size_t num_bands,
	float* const* out,
	size_t nrOfSamples,
	int16_t msInSndCardBuf,
	int32_t skew) {
	AecNew* aecpc = reinterpret_cast<AecNew*>(aecInst);
	int32_t retVal = 0;

	if (out == NULL) {
		return AEC_NULL_POINTER_ERROR;
	}

	if (aecpc->initFlag != initCheck) {
		return AEC_UNINITIALIZED_ERROR;
	}

	// number of samples == 160 for SWB input
	//if (nrOfSamples != 80 && nrOfSamples != 160) {
	//  return AEC_BAD_PARAMETER_ERROR;
	//}

	if (msInSndCardBuf < 0) {
		msInSndCardBuf = 0;
		retVal = AEC_BAD_PARAMETER_WARNING;
	}
	else if (msInSndCardBuf > kMaxTrustedDelayMs) {
		// The clamping is now done in ProcessExtended_New/Normal().
		retVal = AEC_BAD_PARAMETER_WARNING;
	}

	// This returns the value of aec->extended_filter_enabled.
	if (WebRtcAec_extended_filter_enabled_New(aecpc->aec)) {
		ProcessExtended_New(aecpc, nearend, num_bands, out, nrOfSamples, msInSndCardBuf,
			skew);
	}
	else {
		retVal = ProcessNormal_New(aecpc, nearend, num_bands, out, nrOfSamples,
			msInSndCardBuf, skew);
	}

	int far_buf_size_samples = WebRtcAec_system_delay_New(aecpc->aec);
	aecpc->data_dumper->DumpRaw("aec_system_delay", 1, &far_buf_size_samples);
	aecpc->data_dumper->DumpRaw("aec_known_delay", 1, &aecpc->knownDelay);

	return retVal;
}

int WebRtcAec_set_config_New(void* handle, AecConfigNew config) {
	AecNew* self = reinterpret_cast<AecNew*>(handle);
	if (self->initFlag != initCheck) {
		return AEC_UNINITIALIZED_ERROR;
	}

	if (config.skewMode != kAecFalsenew && config.skewMode != kAecTruenew) {
		return AEC_BAD_PARAMETER_ERROR;
	}
	self->skewMode = config.skewMode;

	if (config.nlpMode != kAecNlpConservativenew &&
		config.nlpMode != kAecNlpModeratenew &&
		config.nlpMode != kAecNlpAggressivenew) {
		return AEC_BAD_PARAMETER_ERROR;
	}

	if (config.metricsMode != kAecFalsenew && config.metricsMode != kAecTruenew) {
		return AEC_BAD_PARAMETER_ERROR;
	}

	if (config.delay_logging != kAecFalsenew && config.delay_logging != kAecTruenew) {
		return AEC_BAD_PARAMETER_ERROR;
	}

	WebRtcAec_SetConfigCore_New(self->aec, config.nlpMode, config.metricsMode,
		config.delay_logging);
	return 0;
}

int WebRtcAec_get_echo_status_New(void* handle, int* status) {
	AecNew* self = reinterpret_cast<AecNew*>(handle);
	if (status == NULL) {
		return AEC_NULL_POINTER_ERROR;
	}
	if (self->initFlag != initCheck) {
		return AEC_UNINITIALIZED_ERROR;
	}

	*status = WebRtcAec_echo_state_New(self->aec);

	return 0;
}

//int WebRtcAec_GetMetrics_New(void* handle, AecMetrics* metrics) {
//	const float kUpWeight = 0.7f;
//	float dtmp;
//	int stmp;
//	AecNew* self = reinterpret_cast<AecNew*>(handle);
//	Stats erl;
//	Stats erle;
//	Stats a_nlp;
//
//	if (handle == NULL) {
//		return -1;
//	}
//	if (metrics == NULL) {
//		return AEC_NULL_POINTER_ERROR;
//	}
//	if (self->initFlag != initCheck) {
//		return AEC_UNINITIALIZED_ERROR;
//	}
//
//	WebRtcAec_GetEchoStats_New(self->aec, &erl, &erle, &a_nlp,
//		&metrics->divergent_filter_fraction);
//
//	// ERL
//	metrics->erl.instant = static_cast<int>(erl.instant);
//
//	if ((erl.himean > kOffsetLevel) && (erl.average > kOffsetLevel)) {
//		// Use a mix between regular average and upper part average.
//		dtmp = kUpWeight * erl.himean + (1 - kUpWeight) * erl.average;
//		metrics->erl.average = static_cast<int>(dtmp);
//	}
//	else {
//		metrics->erl.average = kOffsetLevel;
//	}
//
//	metrics->erl.max = static_cast<int>(erl.max);
//
//	if (erl.min < (kOffsetLevel * (-1))) {
//		metrics->erl.min = static_cast<int>(erl.min);
//	}
//	else {
//		metrics->erl.min = kOffsetLevel;
//	}
//
//	// ERLE
//	metrics->erle.instant = static_cast<int>(erle.instant);
//
//	if ((erle.himean > kOffsetLevel) && (erle.average > kOffsetLevel)) {
//		// Use a mix between regular average and upper part average.
//		dtmp = kUpWeight * erle.himean + (1 - kUpWeight) * erle.average;
//		metrics->erle.average = static_cast<int>(dtmp);
//	}
//	else {
//		metrics->erle.average = kOffsetLevel;
//	}
//
//	metrics->erle.max = static_cast<int>(erle.max);
//
//	if (erle.min < (kOffsetLevel * (-1))) {
//		metrics->erle.min = static_cast<int>(erle.min);
//	}
//	else {
//		metrics->erle.min = kOffsetLevel;
//	}
//
//	// RERL
//	if ((metrics->erl.average > kOffsetLevel) &&
//		(metrics->erle.average > kOffsetLevel)) {
//		stmp = metrics->erl.average + metrics->erle.average;
//	}
//	else {
//		stmp = kOffsetLevel;
//	}
//	metrics->rerl.average = stmp;
//
//	// No other statistics needed, but returned for completeness.
//	metrics->rerl.instant = stmp;
//	metrics->rerl.max = stmp;
//	metrics->rerl.min = stmp;
//
//	// A_NLP
//	metrics->aNlp.instant = static_cast<int>(a_nlp.instant);
//
//	if ((a_nlp.himean > kOffsetLevel) && (a_nlp.average > kOffsetLevel)) {
//		// Use a mix between regular average and upper part average.
//		dtmp = kUpWeight * a_nlp.himean + (1 - kUpWeight) * a_nlp.average;
//		metrics->aNlp.average = static_cast<int>(dtmp);
//	}
//	else {
//		metrics->aNlp.average = kOffsetLevel;
//	}
//
//	metrics->aNlp.max = static_cast<int>(a_nlp.max);
//
//	if (a_nlp.min < (kOffsetLevel * (-1))) {
//		metrics->aNlp.min = static_cast<int>(a_nlp.min);
//	}
//	else {
//		metrics->aNlp.min = kOffsetLevel;
//	}
//
//	return 0;
//}

int WebRtcAec_GetDelayMetrics_New(void* handle,
	int* median,
	int* std,
	float* fraction_poor_delays) {
	AecNew* self = reinterpret_cast<AecNew*>(handle);
	if (median == NULL) {
		return AEC_NULL_POINTER_ERROR;
	}
	if (std == NULL) {
		return AEC_NULL_POINTER_ERROR;
	}
	if (self->initFlag != initCheck) {
		return AEC_UNINITIALIZED_ERROR;
	}
	if (WebRtcAec_GetDelayMetricsCore_New(self->aec, median, std,
		fraction_poor_delays) == -1) {
		// Logging disabled.
		return AEC_UNSUPPORTED_FUNCTION_ERROR;
	}

	return 0;
}

AecCoreNew* WebRtcAec_aec_core_New(void* handle) {
	if (!handle) {
		return NULL;
	}
	return reinterpret_cast<AecNew*>(handle)->aec;
}

static int ProcessNormal_New(AecNew* aecpc,
	const float* const* nearend,
	size_t num_bands,
	float* const* out,
	size_t nrOfSamples,
	int16_t msInSndCardBuf,
	int32_t skew) {
	int retVal = 0;
	size_t i;
	size_t nBlocks10ms;
	// Limit resampling to doubling/halving of signal
	const float minSkewEst = -0.5f;
	const float maxSkewEst = 1.0f;

	msInSndCardBuf =
		msInSndCardBuf > kMaxTrustedDelayMs ? kMaxTrustedDelayMs : msInSndCardBuf;
	// TODO(andrew): we need to investigate if this +10 is really wanted.
	msInSndCardBuf += 10;
	aecpc->msInSndCardBuf = msInSndCardBuf;

	if (aecpc->skewMode == kAecTruenew) {
		if (aecpc->skewFrCtr < 25) {
			aecpc->skewFrCtr++;
		}
		else {
		/*	retVal = WebRtcAec_GetSkew(aecpc->resampler, skew, &aecpc->skew);
			if (retVal == -1) {
				aecpc->skew = 0;
				retVal = AEC_BAD_PARAMETER_WARNING;
			}*/

			aecpc->skew /= aecpc->sampFactor * nrOfSamples;

			if (aecpc->skew < 1.0e-3 && aecpc->skew > -1.0e-3) {
				aecpc->resample = kAecFalsenew;
			}
			else {
				aecpc->resample = kAecTruenew;
			}

			if (aecpc->skew < minSkewEst) {
				aecpc->skew = minSkewEst;
			}
			else if (aecpc->skew > maxSkewEst) {
				aecpc->skew = maxSkewEst;
			}

			aecpc->data_dumper->DumpRaw("aec_skew", 1, &aecpc->skew);
		}
	}

	nBlocks10ms = nrOfSamples / (FRAME_LEN_NEW * aecpc->rate_factor);

	if (aecpc->startup_phase) {
		for (i = 0; i < num_bands; ++i) {
			// Only needed if they don't already point to the same place.
			if (nearend[i] != out[i]) {
				memcpy(out[i], nearend[i], sizeof(nearend[i][0]) * nrOfSamples);
			}
		}

		// The AEC is in the start up mode
		// AEC is disabled until the system delay is OK

		// Mechanism to ensure that the system delay is reasonably stable.
		if (aecpc->checkBuffSize) {
			aecpc->checkBufSizeCtr++;
			// Before we fill up the far-end buffer we require the system delay
			// to be stable (+/-8 ms) compared to the first value. This
			// comparison is made during the following 6 consecutive 10 ms
			// blocks. If it seems to be stable then we start to fill up the
			// far-end buffer.
			if (aecpc->counter == 0) {
				aecpc->firstVal = aecpc->msInSndCardBuf;
				aecpc->sum = 0;
			}

			if (abs(aecpc->firstVal - aecpc->msInSndCardBuf) <
				WEBRTC_SPL_MAX(0.2 * aecpc->msInSndCardBuf, sampMsNb)) {
				aecpc->sum += aecpc->msInSndCardBuf;
				aecpc->counter++;
			}
			else {
				aecpc->counter = 0;
			}

			if (aecpc->counter * nBlocks10ms >= 6) {
				// The far-end buffer size is determined in partitions of
				// PART_LEN samples. Use 75% of the average value of the system
				// delay as buffer size to start with.
				aecpc->bufSizeStart =
					WEBRTC_SPL_MIN((3 * aecpc->sum * aecpc->rate_factor * 8) /
					(4 * aecpc->counter * PART_LEN),
						kMaxBufSizeStart);
				// Buffer size has now been determined.
				aecpc->checkBuffSize = 0;
			}

			if (aecpc->checkBufSizeCtr * nBlocks10ms > 50) {
				// For really bad systems, don't disable the echo canceller for
				// more than 0.5 sec.
				aecpc->bufSizeStart = WEBRTC_SPL_MIN(
					(aecpc->msInSndCardBuf * aecpc->rate_factor * 3) / 40,
					kMaxBufSizeStart);
				aecpc->checkBuffSize = 0;
			}
		}

		// If |checkBuffSize| changed in the if-statement above.
		if (!aecpc->checkBuffSize) {
			// The system delay is now reasonably stable (or has been unstable
			// for too long). When the far-end buffer is filled with
			// approximately the same amount of data as reported by the system
			// we end the startup phase.
			int overhead_elements =
				WebRtcAec_system_delay_New(aecpc->aec) / PART_LEN - aecpc->bufSizeStart;
			if (overhead_elements == 0) {
				// Enable the AEC
				aecpc->startup_phase = 0;
			}
			else if (overhead_elements > 0) {
				// TODO(bjornv): Do we need a check on how much we actually
				// moved the read pointer? It should always be possible to move
				// the pointer |overhead_elements| since we have only added data
				// to the buffer and no delay compensation nor AEC processing
				// has been done.
				WebRtcAec_AdjustFarendBufferSizeAndSystemDelay_New(aecpc->aec,
					overhead_elements);

				// Enable the AEC
				aecpc->startup_phase = 0;
			}
		}
	}
	else {
		// AEC is enabled.
		EstBufDelayNormal_New(aecpc);

		// Call the AEC.
		// TODO(bjornv): Re-structure such that we don't have to pass
		// |aecpc->knownDelay| as input. Change name to something like
		// |system_buffer_diff|.
		WebRtcAec_ProcessFrames_New(aecpc->aec, nearend, num_bands, nrOfSamples,
			aecpc->knownDelay, out);
	}

	return retVal;
}

static void ProcessExtended_New(AecNew* self,
	const float* const* near,
	size_t num_bands,
	float* const* out,
	size_t num_samples,
	int16_t reported_delay_ms,
	int32_t skew) {
//	size_t i;
//	const int delay_diff_offset = kDelayDiffOffsetSamples;
//	//RTC_DCHECK(num_samples == 80 || num_samples == 160);
//#if defined(WEBRTC_UNTRUSTED_DELAY)
//	reported_delay_ms = kFixedDelayMs;
//#else
//	// This is the usual mode where we trust the reported system delay values.
//	// Due to the longer filter, we no longer add 10 ms to the reported delay
//	// to reduce chance of non-causality. Instead we apply a minimum here to avoid
//	// issues with the read pointer jumping around needlessly.
//	reported_delay_ms = reported_delay_ms < kMinTrustedDelayMs
//		? kMinTrustedDelayMs
//		: reported_delay_ms;
//	// If the reported delay appears to be bogus, we attempt to recover by using
//	// the measured fixed delay values. We use >= here because higher layers
//	// may already clamp to this maximum value, and we would otherwise not
//	// detect it here.
//	reported_delay_ms = reported_delay_ms >= kMaxTrustedDelayMs
//		? kFixedDelayMs
//		: reported_delay_ms;
//#endif
//	self->msInSndCardBuf = reported_delay_ms;
//
//	if (!self->farend_started) {
//		for (i = 0; i < num_bands; ++i) {
//			// Only needed if they don't already point to the same place.
//			if (near[i] != out[i]) {
//				memcpy(out[i], near[i], sizeof(near[i][0]) * num_samples);
//			}
//		}
//		return;
//	}
//	if (self->startup_phase) {
//		// In the extended mode, there isn't a startup "phase", just a special
//		// action on the first frame. In the trusted delay case, we'll take the
//		// current reported delay, unless it's less then our conservative
//		// measurement.
//		int startup_size_ms =
//			reported_delay_ms < kFixedDelayMs ? kFixedDelayMs : reported_delay_ms;
//#if defined(WEBRTC_ANDROID)
//		int target_delay = startup_size_ms * self->rate_factor * 8;
//#else
//		// To avoid putting the AEC in a non-causal state we're being slightly
//		// conservative and scale by 2. On Android we use a fixed delay and
//		// therefore there is no need to scale the target_delay.
//		int target_delay = startup_size_ms * self->rate_factor * 8 / 2;
//#endif
//		int overhead_elements =
//			(WebRtcAec_system_delay_New(self->aec) - target_delay) / PART_LEN;
//		WebRtcAec_AdjustFarendBufferSizeAndSystemDelay_New(self->aec,
//			overhead_elements);
//		self->startup_phase = 0;
//	}
//
//	EstBufDelayExtended_New(self);
//
//	{
//		// |delay_diff_offset| gives us the option to manually rewind the delay on
//		// very low delay platforms which can't be expressed purely through
//		// |reported_delay_ms|.
//		const int adjusted_known_delay =
//			WEBRTC_SPL_MAX(0, self->knownDelay + delay_diff_offset);
//
//		WebRtcAec_ProcessFrames_New(self->aec, near, num_bands, num_samples,
//			adjusted_known_delay, out);
//	}
}

static void EstBufDelayNormal_New(AecNew* aecpc) {
	int nSampSndCard = aecpc->msInSndCardBuf * sampMsNb * aecpc->rate_factor;
	int current_delay = nSampSndCard - WebRtcAec_system_delay_New(aecpc->aec);
	int delay_difference = 0;

	// Before we proceed with the delay estimate filtering we:
	// 1) Compensate for the frame that will be read.
	// 2) Compensate for drift resampling.
	// 3) Compensate for non-causality if needed, since the estimated delay can't
	//    be negative.

	// 1) Compensating for the frame(s) that will be read/processed.
	current_delay += FRAME_LEN_NEW * aecpc->rate_factor;

	// 2) Account for resampling frame delay.
	if (aecpc->skewMode == kAecTruenew && aecpc->resample == kAecTruenew) {
		current_delay -= kResamplingDelay;
	}

	// 3) Compensate for non-causality, if needed, by flushing one block.
	if (current_delay < PART_LEN) {
		current_delay +=
			WebRtcAec_AdjustFarendBufferSizeAndSystemDelay_New(aecpc->aec, 1) *
			PART_LEN;
	}

	// We use -1 to signal an initialized state in the "extended" implementation;
	// compensate for that.
	aecpc->filtDelay = aecpc->filtDelay < 0 ? 0 : aecpc->filtDelay;
	aecpc->filtDelay =
		WEBRTC_SPL_MAX(0, static_cast<int16_t>(0.8 *
			aecpc->filtDelay +
			0.2 * current_delay));

	delay_difference = aecpc->filtDelay - aecpc->knownDelay;
	if (delay_difference > 224) {
		if (aecpc->lastDelayDiff < 96) {
			aecpc->timeForDelayChange = 0;
		}
		else {
			aecpc->timeForDelayChange++;
		}
	}
	else if (delay_difference < 96 && aecpc->knownDelay > 0) {
		if (aecpc->lastDelayDiff > 224) {
			aecpc->timeForDelayChange = 0;
		}
		else {
			aecpc->timeForDelayChange++;
		}
	}
	else {
		aecpc->timeForDelayChange = 0;
	}
	aecpc->lastDelayDiff = delay_difference;

	if (aecpc->timeForDelayChange > 25) {
		aecpc->knownDelay = WEBRTC_SPL_MAX((int)aecpc->filtDelay - 160, 0);
	}
}

//static void EstBufDelayExtended_New(AecNew* self) {
//	int reported_delay = self->msInSndCardBuf * sampMsNb * self->rate_factor;
//	int current_delay = reported_delay - WebRtcAec_system_delay_New(self->aec);
//	int delay_difference = 0;
//
//	// Before we proceed with the delay estimate filtering we:
//	// 1) Compensate for the frame that will be read.
//	// 2) Compensate for drift resampling.
//	// 3) Compensate for non-causality if needed, since the estimated delay can't
//	//    be negative.
//
//	// 1) Compensating for the frame(s) that will be read/processed.
//	current_delay += FRAME_LEN_NEW * self->rate_factor;
//
//	// 2) Account for resampling frame delay.
//	if (self->skewMode == kAecTruenew && self->resample == kAecTruenew) {
//		current_delay -= kResamplingDelay;
//	}
//
//	// 3) Compensate for non-causality, if needed, by flushing two blocks.
//	if (current_delay < PART_LEN) {
//		current_delay +=
//			WebRtcAec_AdjustFarendBufferSizeAndSystemDelay_New(self->aec, 2) * PART_LEN;
//	}
//
//	if (self->filtDelay == -1) {
//		self->filtDelay = WEBRTC_SPL_MAX(0, 0.5 * current_delay);
//	}
//	else {
//		self->filtDelay = WEBRTC_SPL_MAX(
//			0, static_cast<int16_t>(0.95 * self->filtDelay + 0.05 * current_delay));
//	}
//
//	delay_difference = self->filtDelay - self->knownDelay;
//	if (delay_difference > 384) {
//		if (self->lastDelayDiff < 128) {
//			self->timeForDelayChange = 0;
//		}
//		else {
//			self->timeForDelayChange++;
//		}
//	}
//	else if (delay_difference < 128 && self->knownDelay > 0) {
//		if (self->lastDelayDiff > 384) {
//			self->timeForDelayChange = 0;
//		}
//		else {
//			self->timeForDelayChange++;
//		}
//	}
//	else {
//		self->timeForDelayChange = 0;
//	}
//	self->lastDelayDiff = delay_difference;
//
//	if (self->timeForDelayChange > 25) {
//		self->knownDelay = WEBRTC_SPL_MAX((int)self->filtDelay - 256, 0);
//	}
//}
#endif


// Buffer size (samples)
static const size_t kBufferSizeBlocks = 250;  // 1 second of audio in 16 kHz.

// Metrics
static const size_t kSubCountLen = 4;
static const size_t kCountLen = 50;
static const int kDelayMetricsAggregationWindow = 1250;  // 5 seconds at 16 kHz.

// Divergence metric is based on audio level, which gets updated every
// |kSubCountLen + 1| * PART_LEN samples. Divergence metric takes the statistics
// of |kDivergentFilterFractionAggregationWindowSize| audio levels. The
// following value corresponds to 1 second at 16 kHz.
static const int kDivergentFilterFractionAggregationWindowSize = 50;

// Quantities to control H band scaling for SWB input
static const float cnScaleHband = 0.4f;  // scale for comfort noise in H band.
// Initial bin for averaging nlp gain in low band
static const int freqAvgIc = PART_LEN / 2;

// Matlab code to produce table:
// win = sqrt(hanning(63)); win = [0 ; win(1:32)];
// fprintf(1, '\t%.14f, %.14f, %.14f,\n', win);
ALIGN16_BEG const float ALIGN16_END WebRtcAec_sqrtHanning[65] = {
    0.00000000000000f, 0.02454122852291f, 0.04906767432742f, 0.07356456359967f,
    0.09801714032956f, 0.12241067519922f, 0.14673047445536f, 0.17096188876030f,
    0.19509032201613f, 0.21910124015687f, 0.24298017990326f, 0.26671275747490f,
    0.29028467725446f, 0.31368174039889f, 0.33688985339222f, 0.35989503653499f,
    0.38268343236509f, 0.40524131400499f, 0.42755509343028f, 0.44961132965461f,
    0.47139673682600f, 0.49289819222978f, 0.51410274419322f, 0.53499761988710f,
    0.55557023301960f, 0.57580819141785f, 0.59569930449243f, 0.61523159058063f,
    0.63439328416365f, 0.65317284295378f, 0.67155895484702f, 0.68954054473707f,
    0.70710678118655f, 0.72424708295147f, 0.74095112535496f, 0.75720884650648f,
    0.77301045336274f, 0.78834642762661f, 0.80320753148064f, 0.81758481315158f,
    0.83146961230255f, 0.84485356524971f, 0.85772861000027f, 0.87008699110871f,
    0.88192126434835f, 0.89322430119552f, 0.90398929312344f, 0.91420975570353f,
    0.92387953251129f, 0.93299279883474f, 0.94154406518302f, 0.94952818059304f,
    0.95694033573221f, 0.96377606579544f, 0.97003125319454f, 0.97570213003853f,
    0.98078528040323f, 0.98527764238894f, 0.98917650996478f, 0.99247953459871f,
    0.99518472667220f, 0.99729045667869f, 0.99879545620517f, 0.99969881869620f,
    1.00000000000000f};

// Matlab code to produce table:
// weightCurve = [0 ; 0.3 * sqrt(linspace(0,1,64))' + 0.1];
// fprintf(1, '\t%.4f, %.4f, %.4f, %.4f, %.4f, %.4f,\n', weightCurve);
ALIGN16_BEG const float ALIGN16_END WebRtcAec_weightCurve[65] = {
    0.0000f, 0.1000f, 0.1378f, 0.1535f, 0.1655f, 0.1756f, 0.1845f, 0.1926f,
    0.2000f, 0.2069f, 0.2134f, 0.2195f, 0.2254f, 0.2309f, 0.2363f, 0.2414f,
    0.2464f, 0.2512f, 0.2558f, 0.2604f, 0.2648f, 0.2690f, 0.2732f, 0.2773f,
    0.2813f, 0.2852f, 0.2890f, 0.2927f, 0.2964f, 0.3000f, 0.3035f, 0.3070f,
    0.3104f, 0.3138f, 0.3171f, 0.3204f, 0.3236f, 0.3268f, 0.3299f, 0.3330f,
    0.3360f, 0.3390f, 0.3420f, 0.3449f, 0.3478f, 0.3507f, 0.3535f, 0.3563f,
    0.3591f, 0.3619f, 0.3646f, 0.3673f, 0.3699f, 0.3726f, 0.3752f, 0.3777f,
    0.3803f, 0.3828f, 0.3854f, 0.3878f, 0.3903f, 0.3928f, 0.3952f, 0.3976f,
    0.4000f};

// Matlab code to produce table:
// overDriveCurve = [sqrt(linspace(0,1,65))' + 1];
// fprintf(1, '\t%.4f, %.4f, %.4f, %.4f, %.4f, %.4f,\n', overDriveCurve);
ALIGN16_BEG const float ALIGN16_END WebRtcAec_overDriveCurve[65] = {
    1.0000f, 1.1250f, 1.1768f, 1.2165f, 1.2500f, 1.2795f, 1.3062f, 1.3307f,
    1.3536f, 1.3750f, 1.3953f, 1.4146f, 1.4330f, 1.4507f, 1.4677f, 1.4841f,
    1.5000f, 1.5154f, 1.5303f, 1.5449f, 1.5590f, 1.5728f, 1.5863f, 1.5995f,
    1.6124f, 1.6250f, 1.6374f, 1.6495f, 1.6614f, 1.6731f, 1.6847f, 1.6960f,
    1.7071f, 1.7181f, 1.7289f, 1.7395f, 1.7500f, 1.7603f, 1.7706f, 1.7806f,
    1.7906f, 1.8004f, 1.8101f, 1.8197f, 1.8292f, 1.8385f, 1.8478f, 1.8570f,
    1.8660f, 1.8750f, 1.8839f, 1.8927f, 1.9014f, 1.9100f, 1.9186f, 1.9270f,
    1.9354f, 1.9437f, 1.9520f, 1.9601f, 1.9682f, 1.9763f, 1.9843f, 1.9922f,
    2.0000f};

// Delay Agnostic AEC parameters, still under development and may change.
static const float kDelayQualityThresholdMax = 0.07f;
static const float kDelayQualityThresholdMin = 0.01f;
static const int kInitialShiftOffset = 5;
#if !defined(WEBRTC_ANDROID)
static const int kDelayCorrectionStart = 1500;  // 10 ms chunks
#endif

#ifdef AEC_OPTIMIZE
#if !defined(WEBRTC_ANDROID)//加快收敛速度
static const int kDelayCorrectionStartNew = 100;  // 10 ms chunks modify by wuzhaoyang 1500
#endif
#endif

// Target suppression levels for nlp modes.
// log{0.001, 0.00001, 0.00000001}
static const float kTargetSupp[3] = {-6.9f, -11.5f, -18.4f};

// Two sets of parameters, one for the extended filter mode.
static const float kExtendedMinOverDrive[3] = {3.0f, 6.0f, 15.0f};
static const float kNormalMinOverDrive[3] = {1.0f, 2.0f, 5.0f};
const float WebRtcAec_kExtendedSmoothingCoefficients[2][2] = {{0.9f, 0.1f},
                                                              {0.92f, 0.08f}};
const float WebRtcAec_kNormalSmoothingCoefficients[2][2] = {{0.9f, 0.1f},
                                                            {0.93f, 0.07f}};

// Number of partitions forming the NLP's "preferred" bands.
enum { kPrefBandSize = 24 };

WebRtcAecFilterFar WebRtcAec_FilterFar;
WebRtcAecScaleErrorSignal WebRtcAec_ScaleErrorSignal;
WebRtcAecFilterAdaptation WebRtcAec_FilterAdaptation;
WebRtcAecOverdrive WebRtcAec_Overdrive;
WebRtcAecSuppress WebRtcAec_Suppress;
WebRtcAecComputeCoherence WebRtcAec_ComputeCoherence;
WebRtcAecUpdateCoherenceSpectra WebRtcAec_UpdateCoherenceSpectra;
WebRtcAecStoreAsComplex WebRtcAec_StoreAsComplex;
WebRtcAecPartitionDelay WebRtcAec_PartitionDelay;
WebRtcAecWindowData WebRtcAec_WindowData;

__inline static float MulRe(float aRe, float aIm, float bRe, float bIm) {
  return aRe * bRe - aIm * bIm;
}

__inline static float MulIm(float aRe, float aIm, float bRe, float bIm) {
  return aRe * bIm + aIm * bRe;
}

// TODO(minyue): Due to a legacy bug, |framelevel| and |averagelevel| use a
// window, of which the length is 1 unit longer than indicated. Remove "+1" when
// the code is refactored.
PowerLevel::PowerLevel()
    : framelevel(kSubCountLen + 1),
      averagelevel(kCountLen + 1) {
}

BlockBuffer::BlockBuffer() {
  buffer_ = WebRtc_CreateBuffer(kBufferSizeBlocks, sizeof(float) * PART_LEN);
  RTC_CHECK(buffer_);
  ReInit();
}

BlockBuffer::~BlockBuffer() {
  WebRtc_FreeBuffer(buffer_);
}

void BlockBuffer::ReInit() {
  WebRtc_InitBuffer(buffer_);
}

void BlockBuffer::Insert(const float block[PART_LEN]) {
  WebRtc_WriteBuffer(buffer_, block, 1);
}

void BlockBuffer::ExtractExtendedBlock(float extended_block[PART_LEN2]) {
  float* block_ptr = NULL;
  RTC_DCHECK_LT(0u, AvaliableSpace());

  // Extract the previous block.
  WebRtc_MoveReadPtr(buffer_, -1);
  WebRtc_ReadBuffer(buffer_, reinterpret_cast<void**>(&block_ptr),
                    &extended_block[0], 1);
  if (block_ptr != &extended_block[0]) {
    memcpy(&extended_block[0], block_ptr, PART_LEN * sizeof(float));
  }

  // Extract the current block.
  WebRtc_ReadBuffer(buffer_, reinterpret_cast<void**>(&block_ptr),
                    &extended_block[PART_LEN], 1);
  if (block_ptr != &extended_block[PART_LEN]) {
    memcpy(&extended_block[PART_LEN], block_ptr, PART_LEN * sizeof(float));
  }
}

int BlockBuffer::AdjustSize(int buffer_size_decrease) {
  return WebRtc_MoveReadPtr(buffer_, buffer_size_decrease);
}

size_t BlockBuffer::Size() {
  return static_cast<int>(WebRtc_available_read(buffer_));
}

size_t BlockBuffer::AvaliableSpace() {
  return WebRtc_available_write(buffer_);
}

DivergentFilterFraction::DivergentFilterFraction()
    : count_(0),
      occurrence_(0),
      fraction_(-1.0) {
}

void DivergentFilterFraction::Reset() {
  Clear();
  fraction_ = -1.0;
}

void DivergentFilterFraction::AddObservation(const PowerLevel& nearlevel,
                                             const PowerLevel& linoutlevel,
                                             const PowerLevel& nlpoutlevel) {
  const float near_level = nearlevel.framelevel.GetLatestMean();
  const float level_increase =
      linoutlevel.framelevel.GetLatestMean() - near_level;
  const bool output_signal_active = nlpoutlevel.framelevel.GetLatestMean() >
          40.0 * nlpoutlevel.minlevel;
  // Level increase should be, in principle, negative, when the filter
  // does not diverge. Here we allow some margin (0.01 * near end level) and
  // numerical error (1.0). We count divergence only when the AEC output
  // signal is active.
  if (output_signal_active &&
      level_increase > std::max(0.01 * near_level, 1.0))
    occurrence_++;
  ++count_;
  if (count_ == kDivergentFilterFractionAggregationWindowSize) {
    fraction_ = static_cast<float>(occurrence_) /
        kDivergentFilterFractionAggregationWindowSize;
    Clear();
  }
}

float DivergentFilterFraction::GetLatestFraction() const {
  return fraction_;
}

void DivergentFilterFraction::Clear() {
  count_ = 0;
  occurrence_ = 0;
}

// TODO(minyue): Moving some initialization from WebRtcAec_CreateAec() to ctor.
AecCore::AecCore(int instance_index)
    : data_dumper(new ApmDataDumper(instance_index)) {}

AecCore::~AecCore() {}

#ifdef AEC_OPTIMIZE
// TODO(minyue): Moving some initialization from WebRtcAec_CreateAec_New() to ctor.
AecCoreNew::AecCoreNew(int instance_index)
	: data_dumper(new ApmDataDumper(instance_index)) {}

AecCoreNew::~AecCoreNew() {}
#endif

static int CmpFloat(const void* a, const void* b) {
  const float* da = (const float*)a;
  const float* db = (const float*)b;

  return (*da > *db) - (*da < *db);
}

static void FilterFar(int num_partitions,
                      int x_fft_buf_block_pos,
                      float x_fft_buf[2][kExtendedNumPartitions * PART_LEN1],
                      float h_fft_buf[2][kExtendedNumPartitions * PART_LEN1],
                      float y_fft[2][PART_LEN1]) {
  int i;
  for (i = 0; i < num_partitions; i++) {
    int j;
    int xPos = (i + x_fft_buf_block_pos) * PART_LEN1;
    int pos = i * PART_LEN1;
    // Check for wrap
    if (i + x_fft_buf_block_pos >= num_partitions) {
      xPos -= num_partitions * (PART_LEN1);
    }

    for (j = 0; j < PART_LEN1; j++) {
      y_fft[0][j] += MulRe(x_fft_buf[0][xPos + j], x_fft_buf[1][xPos + j],
                           h_fft_buf[0][pos + j], h_fft_buf[1][pos + j]);
      y_fft[1][j] += MulIm(x_fft_buf[0][xPos + j], x_fft_buf[1][xPos + j],
                           h_fft_buf[0][pos + j], h_fft_buf[1][pos + j]);
    }
  }
}

static void ScaleErrorSignal(float mu,
                             float error_threshold,
                             float x_pow[PART_LEN1],
                             float ef[2][PART_LEN1]) {
  int i;
  float abs_ef;
  for (i = 0; i < (PART_LEN1); i++) {
    ef[0][i] /= (x_pow[i] + 1e-10f);
    ef[1][i] /= (x_pow[i] + 1e-10f);
    abs_ef = sqrtf(ef[0][i] * ef[0][i] + ef[1][i] * ef[1][i]);

    if (abs_ef > error_threshold) {
      abs_ef = error_threshold / (abs_ef + 1e-10f);
      ef[0][i] *= abs_ef;
      ef[1][i] *= abs_ef;
    }

    // Stepsize factor
    ef[0][i] *= mu;
    ef[1][i] *= mu;
  }
}

static void FilterAdaptation(
    int num_partitions,
    int x_fft_buf_block_pos,
    float x_fft_buf[2][kExtendedNumPartitions * PART_LEN1],
    float e_fft[2][PART_LEN1],
    float h_fft_buf[2][kExtendedNumPartitions * PART_LEN1]) {
  int i, j;
  float fft[PART_LEN2];
  for (i = 0; i < num_partitions; i++) {
    int xPos = (i + x_fft_buf_block_pos) * (PART_LEN1);
    int pos;
    // Check for wrap
    if (i + x_fft_buf_block_pos >= num_partitions) {
      xPos -= num_partitions * PART_LEN1;
    }

    pos = i * PART_LEN1;

    for (j = 0; j < PART_LEN; j++) {
      fft[2 * j] = MulRe(x_fft_buf[0][xPos + j], -x_fft_buf[1][xPos + j],
                         e_fft[0][j], e_fft[1][j]);
      fft[2 * j + 1] = MulIm(x_fft_buf[0][xPos + j], -x_fft_buf[1][xPos + j],
                             e_fft[0][j], e_fft[1][j]);
    }
    fft[1] =
        MulRe(x_fft_buf[0][xPos + PART_LEN], -x_fft_buf[1][xPos + PART_LEN],
              e_fft[0][PART_LEN], e_fft[1][PART_LEN]);

    aec_rdft_inverse_128(fft);
    memset(fft + PART_LEN, 0, sizeof(float) * PART_LEN);

    // fft scaling
    {
      float scale = 2.0f / PART_LEN2;
      for (j = 0; j < PART_LEN; j++) {
        fft[j] *= scale;
      }
    }
    aec_rdft_forward_128(fft);

    h_fft_buf[0][pos] += fft[0];
    h_fft_buf[0][pos + PART_LEN] += fft[1];

    for (j = 1; j < PART_LEN; j++) {
      h_fft_buf[0][pos + j] += fft[2 * j];
      h_fft_buf[1][pos + j] += fft[2 * j + 1];
    }
  }
}

static void Overdrive(float overdrive_scaling,
                      const float hNlFb,
                      float hNl[PART_LEN1]) {
  for (int i = 0; i < PART_LEN1; ++i) {
    // Weight subbands
    if (hNl[i] > hNlFb) {
      hNl[i] = WebRtcAec_weightCurve[i] * hNlFb +
               (1 - WebRtcAec_weightCurve[i]) * hNl[i];
    }
    hNl[i] = powf(hNl[i], overdrive_scaling * WebRtcAec_overDriveCurve[i]);
  }
}

static void Suppress(const float hNl[PART_LEN1], float efw[2][PART_LEN1]) {
  for (int i = 0; i < PART_LEN1; ++i) {
    // Suppress error signal
    efw[0][i] *= hNl[i];
    efw[1][i] *= hNl[i];

    // Ooura fft returns incorrect sign on imaginary component. It matters here
    // because we are making an additive change with comfort noise.
    efw[1][i] *= -1;
  }
}

static int PartitionDelay(int num_partitions,
                          float h_fft_buf[2]
                                         [kExtendedNumPartitions * PART_LEN1]) {
  // Measures the energy in each filter partition and returns the partition with
  // highest energy.
  // TODO(bjornv): Spread computational cost by computing one partition per
  // block?
  float wfEnMax = 0;
  int i;
  int delay = 0;

  for (i = 0; i < num_partitions; i++) {
    int j;
    int pos = i * PART_LEN1;
    float wfEn = 0;
    for (j = 0; j < PART_LEN1; j++) {
      wfEn += h_fft_buf[0][pos + j] * h_fft_buf[0][pos + j] +
              h_fft_buf[1][pos + j] * h_fft_buf[1][pos + j];
    }

    if (wfEn > wfEnMax) {
      wfEnMax = wfEn;
      delay = i;
    }
  }
  return delay;
}

// Update metric with 10 * log10(numerator / denominator).
static void UpdateLogRatioMetric(Stats* metric, float numerator,
                                 float denominator) {
  RTC_DCHECK(metric);
  RTC_CHECK(numerator >= 0);
  RTC_CHECK(denominator >= 0);

  const float log_numerator = log10(numerator + 1e-10f);
  const float log_denominator = log10(denominator + 1e-10f);
  metric->instant = 10.0f * (log_numerator - log_denominator);

  // Max.
  if (metric->instant > metric->max)
    metric->max = metric->instant;

  // Min.
  if (metric->instant < metric->min)
    metric->min = metric->instant;

  // Average.
  metric->counter++;
  // This is to protect overflow, which should almost never happen.
  RTC_CHECK_NE(0u, metric->counter);
  metric->sum += metric->instant;
  metric->average = metric->sum / metric->counter;

  // Upper mean.
  if (metric->instant > metric->average) {
    metric->hicounter++;
    // This is to protect overflow, which should almost never happen.
    RTC_CHECK_NE(0u, metric->hicounter);
    metric->hisum += metric->instant;
    metric->himean = metric->hisum / metric->hicounter;
  }
}

// Threshold to protect against the ill-effects of a zero far-end.
const float WebRtcAec_kMinFarendPSD = 15;

// Updates the following smoothed Power Spectral Densities (PSD):
//  - sd  : near-end
//  - se  : residual echo
//  - sx  : far-end
//  - sde : cross-PSD of near-end and residual echo
//  - sxd : cross-PSD of near-end and far-end
//
// In addition to updating the PSDs, also the filter diverge state is
// determined.
static void UpdateCoherenceSpectra(int mult,
                                   bool extended_filter_enabled,
                                   float efw[2][PART_LEN1],
                                   float dfw[2][PART_LEN1],
                                   float xfw[2][PART_LEN1],
                                   CoherenceState* coherence_state,
                                   short* filter_divergence_state,
                                   int* extreme_filter_divergence) {
  // Power estimate smoothing coefficients.
  const float* ptrGCoh =
      extended_filter_enabled
          ? WebRtcAec_kExtendedSmoothingCoefficients[mult - 1]
          : WebRtcAec_kNormalSmoothingCoefficients[mult - 1];
  int i;
  float sdSum = 0, seSum = 0;

  for (i = 0; i < PART_LEN1; i++) {
    coherence_state->sd[i] =
        ptrGCoh[0] * coherence_state->sd[i] +
        ptrGCoh[1] * (dfw[0][i] * dfw[0][i] + dfw[1][i] * dfw[1][i]);
    coherence_state->se[i] =
        ptrGCoh[0] * coherence_state->se[i] +
        ptrGCoh[1] * (efw[0][i] * efw[0][i] + efw[1][i] * efw[1][i]);
    // We threshold here to protect against the ill-effects of a zero farend.
    // The threshold is not arbitrarily chosen, but balances protection and
    // adverse interaction with the algorithm's tuning.
    // TODO(bjornv): investigate further why this is so sensitive.
    coherence_state->sx[i] =
        ptrGCoh[0] * coherence_state->sx[i] +
        ptrGCoh[1] *
            WEBRTC_SPL_MAX(xfw[0][i] * xfw[0][i] + xfw[1][i] * xfw[1][i],
                           WebRtcAec_kMinFarendPSD);

    coherence_state->sde[i][0] =
        ptrGCoh[0] * coherence_state->sde[i][0] +
        ptrGCoh[1] * (dfw[0][i] * efw[0][i] + dfw[1][i] * efw[1][i]);
    coherence_state->sde[i][1] =
        ptrGCoh[0] * coherence_state->sde[i][1] +
        ptrGCoh[1] * (dfw[0][i] * efw[1][i] - dfw[1][i] * efw[0][i]);

    coherence_state->sxd[i][0] =
        ptrGCoh[0] * coherence_state->sxd[i][0] +
        ptrGCoh[1] * (dfw[0][i] * xfw[0][i] + dfw[1][i] * xfw[1][i]);
    coherence_state->sxd[i][1] =
        ptrGCoh[0] * coherence_state->sxd[i][1] +
        ptrGCoh[1] * (dfw[0][i] * xfw[1][i] - dfw[1][i] * xfw[0][i]);

    sdSum += coherence_state->sd[i];
    seSum += coherence_state->se[i];
  }

  // Divergent filter safeguard update.
  *filter_divergence_state =
      (*filter_divergence_state ? 1.05f : 1.0f) * seSum > sdSum;

  // Signal extreme filter divergence if the error is significantly larger
  // than the nearend (13 dB).
  *extreme_filter_divergence = (seSum > (19.95f * sdSum));
}

// Window time domain data to be used by the fft.
__inline static void WindowData(float* x_windowed, const float* x) {
  int i;
  for (i = 0; i < PART_LEN; i++) {
    x_windowed[i] = x[i] * WebRtcAec_sqrtHanning[i];
    x_windowed[PART_LEN + i] =
        x[PART_LEN + i] * WebRtcAec_sqrtHanning[PART_LEN - i];
  }
}

// Puts fft output data into a complex valued array.
__inline static void StoreAsComplex(const float* data,
                                    float data_complex[2][PART_LEN1]) {
  int i;
  data_complex[0][0] = data[0];
  data_complex[1][0] = 0;
  for (i = 1; i < PART_LEN; i++) {
    data_complex[0][i] = data[2 * i];
    data_complex[1][i] = data[2 * i + 1];
  }
  data_complex[0][PART_LEN] = data[1];
  data_complex[1][PART_LEN] = 0;
}

static void ComputeCoherence(const CoherenceState* coherence_state,
                             float* cohde,
                             float* cohxd) {
  // Subband coherence
  for (int i = 0; i < PART_LEN1; i++) {
    cohde[i] = (coherence_state->sde[i][0] * coherence_state->sde[i][0] +
                coherence_state->sde[i][1] * coherence_state->sde[i][1]) /
               (coherence_state->sd[i] * coherence_state->se[i] + 1e-10f);
    cohxd[i] = (coherence_state->sxd[i][0] * coherence_state->sxd[i][0] +
                coherence_state->sxd[i][1] * coherence_state->sxd[i][1]) /
               (coherence_state->sx[i] * coherence_state->sd[i] + 1e-10f);
  }
}

static void GetHighbandGain(const float* lambda, float* nlpGainHband) {
  int i;

  *nlpGainHband = 0.0f;
  for (i = freqAvgIc; i < PART_LEN1 - 1; i++) {
    *nlpGainHband += lambda[i];
  }
  *nlpGainHband /= static_cast<float>(PART_LEN1 - 1 - freqAvgIc);
}

static void GenerateComplexNoise(uint32_t* seed, float noise[2][PART_LEN1]) {
  const float kPi2 = 6.28318530717959f;
  int16_t randW16[PART_LEN];
  WebRtcSpl_RandUArray(randW16, PART_LEN, seed);

  noise[0][0] = 0;
  noise[1][0] = 0;
  for (size_t i = 1; i < PART_LEN1; i++) {
    float tmp = kPi2 * randW16[i - 1] / 32768.f;
    noise[0][i] = cosf(tmp);
    noise[1][i] = -sinf(tmp);
  }
  noise[1][PART_LEN] = 0;
}

static void ComfortNoise(bool generate_high_frequency_noise,
                         uint32_t* seed,
                         float e_fft[2][PART_LEN1],
                         float high_frequency_comfort_noise[2][PART_LEN1],
                         const float* noise_spectrum,
                         const float* suppressor_gain) {
  float complex_noise[2][PART_LEN1];

  GenerateComplexNoise(seed, complex_noise);

  // Shape, scale and add comfort noise.
  for (int i = 1; i < PART_LEN1; ++i) {
    float noise_scaling =
        sqrtf(WEBRTC_SPL_MAX(1 - suppressor_gain[i] * suppressor_gain[i], 0)) *
        sqrtf(noise_spectrum[i]);
    e_fft[0][i] += noise_scaling * complex_noise[0][i];
    e_fft[1][i] += noise_scaling * complex_noise[1][i];
  }

  // Form comfort noise for higher frequencies.
  if (generate_high_frequency_noise) {
    // Compute average noise power and nlp gain over the second half of freq
    // spectrum (i.e., 4->8khz).
    int start_avg_band = PART_LEN1 / 2;
    float upper_bands_noise_power = 0.f;
    float upper_bands_suppressor_gain = 0.f;
    for (int i = start_avg_band; i < PART_LEN1; ++i) {
      upper_bands_noise_power += sqrtf(noise_spectrum[i]);
      upper_bands_suppressor_gain +=
          sqrtf(WEBRTC_SPL_MAX(1 - suppressor_gain[i] * suppressor_gain[i], 0));
    }
    upper_bands_noise_power /= (PART_LEN1 - start_avg_band);
    upper_bands_suppressor_gain /= (PART_LEN1 - start_avg_band);

    // Shape, scale and add comfort noise.
    float noise_scaling = upper_bands_suppressor_gain * upper_bands_noise_power;
    high_frequency_comfort_noise[0][0] = 0;
    high_frequency_comfort_noise[1][0] = 0;
    for (int i = 1; i < PART_LEN1; ++i) {
      high_frequency_comfort_noise[0][i] = noise_scaling * complex_noise[0][i];
      high_frequency_comfort_noise[1][i] = noise_scaling * complex_noise[1][i];
    }
    high_frequency_comfort_noise[1][PART_LEN] = 0;
  } else {
    memset(high_frequency_comfort_noise, 0,
           2 * PART_LEN1 * sizeof(high_frequency_comfort_noise[0][0]));
  }
}

static void InitLevel(PowerLevel* level) {
  const float kBigFloat = 1E17f;
  level->averagelevel.Reset();
  level->framelevel.Reset();
  level->minlevel = kBigFloat;
}

static void InitStats(Stats* stats) {
  stats->instant = kOffsetLevel;
  stats->average = kOffsetLevel;
  stats->max = kOffsetLevel;
  stats->min = kOffsetLevel * (-1);
  stats->sum = 0;
  stats->hisum = 0;
  stats->himean = kOffsetLevel;
  stats->counter = 0;
  stats->hicounter = 0;
}

static void InitMetrics(AecCore* self) {
  self->stateCounter = 0;
  InitLevel(&self->farlevel);
  InitLevel(&self->nearlevel);
  InitLevel(&self->linoutlevel);
  InitLevel(&self->nlpoutlevel);

  InitStats(&self->erl);
  InitStats(&self->erle);
  InitStats(&self->aNlp);
  InitStats(&self->rerl);

  self->divergent_filter_fraction.Reset();
}


#ifdef AEC_OPTIMIZE
static void InitMetrics_New(AecCoreNew* self) {
	self->stateCounter = 0;
	InitLevel(&self->farlevel);
	InitLevel(&self->nearlevel);
	InitLevel(&self->linoutlevel);
	InitLevel(&self->nlpoutlevel);

	InitStats(&self->erl);
	InitStats(&self->erle);
	InitStats(&self->aNlp);
	InitStats(&self->rerl);

	self->divergent_filter_fraction.Reset();
}
#endif


static float CalculatePower(const float* in, size_t num_samples) {
  size_t k;
  float energy = 0.0f;

  for (k = 0; k < num_samples; ++k) {
    energy += in[k] * in[k];
  }
  return energy / num_samples;
}

static void UpdateLevel(PowerLevel* level, float power) {
  level->framelevel.AddValue(power);
  if (level->framelevel.EndOfBlock()) {
    const float new_frame_level = level->framelevel.GetLatestMean();
    if (new_frame_level > 0) {
      if (new_frame_level < level->minlevel) {
        level->minlevel = new_frame_level;  // New minimum.
      } else {
        level->minlevel *= (1 + 0.001f);  // Small increase.
      }
    }
    level->averagelevel.AddValue(new_frame_level);
  }
}

//static void UpdateMetrics(AecCore* aec) {
//  const float actThresholdNoisy = 8.0f;
//  const float actThresholdClean = 40.0f;
//
//  const float noisyPower = 300000.0f;
//
//  float actThreshold;
//
//  if (aec->echoState) {  // Check if echo is likely present
//    aec->stateCounter++;
//  }
//
//  if (aec->linoutlevel.framelevel.EndOfBlock()) {
//    aec->divergent_filter_fraction.AddObservation(aec->nearlevel,
//                                                  aec->linoutlevel,
//                                                  aec->nlpoutlevel);
//  }
//
//  if (aec->farlevel.averagelevel.EndOfBlock()) {
//    if (aec->farlevel.minlevel < noisyPower) {
//      actThreshold = actThresholdClean;
//    } else {
//      actThreshold = actThresholdNoisy;
//    }
//
//    const float far_average_level = aec->farlevel.averagelevel.GetLatestMean();
//
//    // The last condition is to let estimation be made in active far-end
//    // segments only.
//    if ((aec->stateCounter > (0.5f * kCountLen * kSubCountLen)) &&
//        (aec->farlevel.framelevel.EndOfBlock()) &&
//        (far_average_level > (actThreshold * aec->farlevel.minlevel))) {
//
//      // ERL: error return loss.
//      const float near_average_level =
//          aec->nearlevel.averagelevel.GetLatestMean();
//      UpdateLogRatioMetric(&aec->erl, far_average_level, near_average_level);
//
//      // A_NLP: error return loss enhanced before the nonlinear suppression.
//      const float linout_average_level =
//          aec->linoutlevel.averagelevel.GetLatestMean();
//      UpdateLogRatioMetric(&aec->aNlp, near_average_level,
//                           linout_average_level);
//
//      // ERLE: error return loss enhanced.
//      const float nlpout_average_level =
//          aec->nlpoutlevel.averagelevel.GetLatestMean();
//      UpdateLogRatioMetric(&aec->erle, near_average_level,
//                           nlpout_average_level);
//    }
//
//    aec->stateCounter = 0;
//  }
//}

static void UpdateDelayMetrics(AecCore* self) {
  int i = 0;
  int delay_values = 0;
  int median = 0;
  int lookahead = WebRtc_lookahead(self->delay_estimator);
  const int kMsPerBlock = PART_LEN / (self->mult * 8);
  int64_t l1_norm = 0;

  if (self->num_delay_values == 0) {
    // We have no new delay value data. Even though -1 is a valid |median| in
    // the sense that we allow negative values, it will practically never be
    // used since multiples of |kMsPerBlock| will always be returned.
    // We therefore use -1 to indicate in the logs that the delay estimator was
    // not able to estimate the delay.
    self->delay_median = -1;
    self->delay_std = -1;
    self->fraction_poor_delays = -1;
    return;
  }

  // Start value for median count down.
  delay_values = self->num_delay_values >> 1;
  // Get median of delay values since last update.
  for (i = 0; i < kHistorySizeBlocks; i++) {
    delay_values -= self->delay_histogram[i];
    if (delay_values < 0) {
      median = i;
      break;
    }
  }
  // Account for lookahead.
  self->delay_median = (median - lookahead) * kMsPerBlock;

  // Calculate the L1 norm, with median value as central moment.
  for (i = 0; i < kHistorySizeBlocks; i++) {
    l1_norm += abs(i - median) * self->delay_histogram[i];
  }
  self->delay_std =
      static_cast<int>((l1_norm + self->num_delay_values / 2) /
                       self->num_delay_values) * kMsPerBlock;

  // Determine fraction of delays that are out of bounds, that is, either
  // negative (anti-causal system) or larger than the AEC filter length.
  {
    int num_delays_out_of_bounds = self->num_delay_values;
    const int histogram_length =
        sizeof(self->delay_histogram) / sizeof(self->delay_histogram[0]);
    for (i = lookahead; i < lookahead + self->num_partitions; ++i) {
      if (i < histogram_length)
        num_delays_out_of_bounds -= self->delay_histogram[i];
    }
    self->fraction_poor_delays =
        static_cast<float>(num_delays_out_of_bounds) / self->num_delay_values;
  }

  // Reset histogram.
  memset(self->delay_histogram, 0, sizeof(self->delay_histogram));
  self->num_delay_values = 0;

  return;
}

#ifdef AEC_OPTIMIZE
static void UpdateMetrics_New(AecCoreNew* aec) {
	const float actThresholdNoisy = 8.0f;
	const float actThresholdClean = 40.0f;

	const float noisyPower = 300000.0f;

	float actThreshold;

	if (aec->echoState) {  // Check if echo is likely present
		aec->stateCounter++;
	}

	if (aec->linoutlevel.framelevel.EndOfBlock()) {
		aec->divergent_filter_fraction.AddObservation(aec->nearlevel,
			aec->linoutlevel,
			aec->nlpoutlevel);
	}

	if (aec->farlevel.averagelevel.EndOfBlock()) {
		if (aec->farlevel.minlevel < noisyPower) {
			actThreshold = actThresholdClean;
		}
		else {
			actThreshold = actThresholdNoisy;
		}

		const float far_average_level = aec->farlevel.averagelevel.GetLatestMean();

		// The last condition is to let estimation be made in active far-end
		// segments only.
		if ((aec->stateCounter > (0.5f * kCountLen * kSubCountLen)) &&
			(aec->farlevel.framelevel.EndOfBlock()) &&
			(far_average_level > (actThreshold * aec->farlevel.minlevel))) {

			// ERL: error return loss.
			const float near_average_level =
				aec->nearlevel.averagelevel.GetLatestMean();
			UpdateLogRatioMetric(&aec->erl, far_average_level, near_average_level);

			// A_NLP: error return loss enhanced before the nonlinear suppression.
			const float linout_average_level =
				aec->linoutlevel.averagelevel.GetLatestMean();
			UpdateLogRatioMetric(&aec->aNlp, near_average_level,
				linout_average_level);

			// ERLE: error return loss enhanced.
			const float nlpout_average_level =
				aec->nlpoutlevel.averagelevel.GetLatestMean();
			UpdateLogRatioMetric(&aec->erle, near_average_level,
				nlpout_average_level);
		}

		aec->stateCounter = 0;
	}
}

static void UpdateDelayMetrics_New(AecCoreNew* self) {
	int i = 0;
	int delay_values = 0;
	int median = 0;
	int lookahead = WebRtc_lookahead(self->delay_estimator);
	const int kMsPerBlock = PART_LEN / (self->mult * 8);
	int64_t l1_norm = 0;

	if (self->num_delay_values == 0) {
		// We have no new delay value data. Even though -1 is a valid |median| in
		// the sense that we allow negative values, it will practically never be
		// used since multiples of |kMsPerBlock| will always be returned.
		// We therefore use -1 to indicate in the logs that the delay estimator was
		// not able to estimate the delay.
		self->delay_median = -1;
		self->delay_std = -1;
		self->fraction_poor_delays = -1;
		return;
	}

	// Start value for median count down.
	delay_values = self->num_delay_values >> 1;
	// Get median of delay values since last update.
	for (i = 0; i < kHistorySizeBlocks; i++) {
		delay_values -= self->delay_histogram[i];
		if (delay_values < 0) {
			median = i;
			break;
		}
	}
	// Account for lookahead.
	self->delay_median = (median - lookahead) * kMsPerBlock;

	// Calculate the L1 norm, with median value as central moment.
	for (i = 0; i < kHistorySizeBlocks; i++) {
		l1_norm += abs(i - median) * self->delay_histogram[i];
	}
	self->delay_std =
		static_cast<int>((l1_norm + self->num_delay_values / 2) /
			self->num_delay_values) * kMsPerBlock;

	// Determine fraction of delays that are out of bounds, that is, either
	// negative (anti-causal system) or larger than the AEC filter length.
	{
		int num_delays_out_of_bounds = self->num_delay_values;
		const int histogram_length =
			sizeof(self->delay_histogram) / sizeof(self->delay_histogram[0]);
		for (i = lookahead; i < lookahead + self->num_partitions; ++i) {
			if (i < histogram_length)
				num_delays_out_of_bounds -= self->delay_histogram[i];
		}
		self->fraction_poor_delays =
			static_cast<float>(num_delays_out_of_bounds) / self->num_delay_values;
	}

	// Reset histogram.
	memset(self->delay_histogram, 0, sizeof(self->delay_histogram));
	self->num_delay_values = 0;

	return;
}
#endif

static void ScaledInverseFft(float freq_data[2][PART_LEN1],
                             float time_data[PART_LEN2],
                             float scale,
                             int conjugate) {
  int i;
  const float normalization = scale / static_cast<float>(PART_LEN2);
  const float sign = (conjugate ? -1 : 1);
  time_data[0] = freq_data[0][0] * normalization;
  time_data[1] = freq_data[0][PART_LEN] * normalization;
  for (i = 1; i < PART_LEN; i++) {
    time_data[2 * i] = freq_data[0][i] * normalization;
    time_data[2 * i + 1] = sign * freq_data[1][i] * normalization;
  }
  aec_rdft_inverse_128(time_data);
}

static void Fft(float time_data[PART_LEN2], float freq_data[2][PART_LEN1]) {
  int i;
  aec_rdft_forward_128(time_data);

  // Reorder fft output data.
  freq_data[1][0] = 0;
  freq_data[1][PART_LEN] = 0;
  freq_data[0][0] = time_data[0];
  freq_data[0][PART_LEN] = time_data[1];
  for (i = 1; i < PART_LEN; i++) {
    freq_data[0][i] = time_data[2 * i];
    freq_data[1][i] = time_data[2 * i + 1];
  }
}

static int SignalBasedDelayCorrection(AecCore* self) {
  int delay_correction = 0;
  int last_delay = -2;
  RTC_DCHECK(self);
#if !defined(WEBRTC_ANDROID)
  // On desktops, turn on correction after |kDelayCorrectionStart| frames.  This
  // is to let the delay estimation get a chance to converge.  Also, if the
  // playout audio volume is low (or even muted) the delay estimation can return
  // a very large delay, which will break the AEC if it is applied.
  if (self->frame_count < kDelayCorrectionStart) {
    self->data_dumper->DumpRaw("aec_da_reported_delay", 1, &last_delay);
    return 0;
  }
#endif

  // 1. Check for non-negative delay estimate.  Note that the estimates we get
  //    from the delay estimation are not compensated for lookahead.  Hence, a
  //    negative |last_delay| is an invalid one.
  // 2. Verify that there is a delay change.  In addition, only allow a change
  //    if the delay is outside a certain region taking the AEC filter length
  //    into account.
  // TODO(bjornv): Investigate if we can remove the non-zero delay change check.
  // 3. Only allow delay correction if the delay estimation quality exceeds
  //    |delay_quality_threshold|.
  // 4. Finally, verify that the proposed |delay_correction| is feasible by
  //    comparing with the size of the far-end buffer.
  last_delay = WebRtc_last_delay(self->delay_estimator);
  self->data_dumper->DumpRaw("aec_da_reported_delay", 1, &last_delay);
  if ((last_delay >= 0) && (last_delay != self->previous_delay) &&
      (WebRtc_last_delay_quality(self->delay_estimator) >
       self->delay_quality_threshold)) {
    int delay = last_delay - WebRtc_lookahead(self->delay_estimator);
    // Allow for a slack in the actual delay, defined by a |lower_bound| and an
    // |upper_bound|.  The adaptive echo cancellation filter is currently
    // |num_partitions| (of 64 samples) long.  If the delay estimate is negative
    // or at least 3/4 of the filter length we open up for correction.
    const int lower_bound = 0;
    const int upper_bound = self->num_partitions * 3 / 4;
    const int do_correction = delay <= lower_bound || delay > upper_bound;
    if (do_correction == 1) {
      int available_read = self->farend_block_buffer_.Size();
      // With |shift_offset| we gradually rely on the delay estimates.  For
      // positive delays we reduce the correction by |shift_offset| to lower the
      // risk of pushing the AEC into a non causal state.  For negative delays
      // we rely on the values up to a rounding error, hence compensate by 1
      // element to make sure to push the delay into the causal region.
      delay_correction = -delay;
      delay_correction += delay > self->shift_offset ? self->shift_offset : 1;
      self->shift_offset--;
      self->shift_offset = (self->shift_offset <= 1 ? 1 : self->shift_offset);
      if (delay_correction > available_read - self->mult - 1) {
        // There is not enough data in the buffer to perform this shift.  Hence,
        // we do not rely on the delay estimate and do nothing.
        delay_correction = 0;
      } else {
        self->previous_delay = last_delay;
        ++self->delay_correction_count;
      }
    }
  }
  // Update the |delay_quality_threshold| once we have our first delay
  // correction.
  if (self->delay_correction_count > 0) {
    float delay_quality = WebRtc_last_delay_quality(self->delay_estimator);
    delay_quality =
        (delay_quality > kDelayQualityThresholdMax ? kDelayQualityThresholdMax
                                                   : delay_quality);
    self->delay_quality_threshold =
        (delay_quality > self->delay_quality_threshold
             ? delay_quality
             : self->delay_quality_threshold);
  }
  self->data_dumper->DumpRaw("aec_da_delay_correction", 1, &delay_correction);

  return delay_correction;
}


#ifdef AEC_OPTIMIZE
static int SignalBasedDelayCorrection_New(AecCoreNew* self)
{
	int delay_correction = 0;
	int last_delay = -2;
	RTC_DCHECK(self);
#if !defined(WEBRTC_ANDROID)
	// On desktops, turn on correction after |kDelayCorrectionStartNew| frames.  This
	// is to let the delay estimation get a chance to converge.  Also, if the
	// playout audio volume is low (or even muted) the delay estimation can return
	// a very large delay, which will break the AEC if it is applied.
	if (self->frame_count < kDelayCorrectionStartNew)
	{
		self->data_dumper->DumpRaw("aec_da_reported_delay", 1, &last_delay);
		return 0;
	}
#endif

	// 1. Check for non-negative delay estimate.  Note that the estimates we get
	//    from the delay estimation are not compensated for lookahead.  Hence, a
	//    negative |last_delay| is an invalid one.
	// 2. Verify that there is a delay change.  In addition, only allow a change
	//    if the delay is outside a certain region taking the AEC filter length
	//    into account.
	// TODO(bjornv): Investigate if we can remove the non-zero delay change check.
	// 3. Only allow delay correction if the delay estimation quality exceeds
	//    |delay_quality_threshold|.
	// 4. Finally, verify that the proposed |delay_correction| is feasible by
	//    comparing with the size of the far-end buffer.
	last_delay = WebRtc_last_delay(self->delay_estimator);
	self->data_dumper->DumpRaw("aec_da_reported_delay", 1, &last_delay);
	if ((last_delay >= 0) && (last_delay != self->previous_delay) &&
		(WebRtc_last_delay_quality(self->delay_estimator) >
			self->delay_quality_threshold)) {
		int delay = last_delay - WebRtc_lookahead(self->delay_estimator);
		// Allow for a slack in the actual delay, defined by a |lower_bound| and an
		// |upper_bound|.  The adaptive echo cancellation filter is currently
		// |num_partitions| (of 64 samples) long.  If the delay estimate is negative
		// or at least 3/4 of the filter length we open up for correction.
		const int lower_bound = 0;
		const int upper_bound = self->num_partitions * 3 / 4;
		const int do_correction = delay <= lower_bound || delay > upper_bound;
		if (do_correction == 1) {
			int available_read = self->farend_block_buffer_.Size();
			// With |shift_offset| we gradually rely on the delay estimates.  For
			// positive delays we reduce the correction by |shift_offset| to lower the
			// risk of pushing the AEC into a non causal state.  For negative delays
			// we rely on the values up to a rounding error, hence compensate by 1
			// element to make sure to push the delay into the causal region.
			delay_correction = -delay;
			delay_correction += delay > self->shift_offset ? self->shift_offset : 1;
			self->shift_offset--;
			self->shift_offset = (self->shift_offset <= 1 ? 1 : self->shift_offset);
			if (delay_correction > available_read - self->mult - 1) {
				// There is not enough data in the buffer to perform this shift.  Hence,
				// we do not rely on the delay estimate and do nothing.
				delay_correction = 0;
			}
			else {
				self->previous_delay = last_delay;
				++self->delay_correction_count;
			}
		}
	}
	// Update the |delay_quality_threshold| once we have our first delay
	// correction.
	if (self->delay_correction_count > 0) {
		float delay_quality = WebRtc_last_delay_quality(self->delay_estimator);
		delay_quality =
			(delay_quality > kDelayQualityThresholdMax ? kDelayQualityThresholdMax
				: delay_quality);
		self->delay_quality_threshold =
			(delay_quality > self->delay_quality_threshold
				? delay_quality
				: self->delay_quality_threshold);
	}
	self->data_dumper->DumpRaw("aec_da_delay_correction", 1, &delay_correction);

	return delay_correction;
}
#endif

static void RegressorPower(int num_partitions,
                           int latest_added_partition,
                           float x_fft_buf[2]
                                          [kExtendedNumPartitions * PART_LEN1],
                           float x_pow[PART_LEN1]) {
  RTC_DCHECK_LT(latest_added_partition, num_partitions);
  memset(x_pow, 0, PART_LEN1 * sizeof(x_pow[0]));

  int partition = latest_added_partition;
  int x_fft_buf_position = partition * PART_LEN1;
  for (int i = 0; i < num_partitions; ++i) {
    for (int bin = 0; bin < PART_LEN1; ++bin) {
      float re = x_fft_buf[0][x_fft_buf_position];
      float im = x_fft_buf[1][x_fft_buf_position];
      x_pow[bin] += re * re + im * im;
      ++x_fft_buf_position;
    }

    ++partition;
    if (partition == num_partitions) {
      partition = 0;
      RTC_DCHECK_EQ(num_partitions * PART_LEN1, x_fft_buf_position);
      x_fft_buf_position = 0;
    }
  }
}

static void EchoSubtraction(int num_partitions,
                            int extended_filter_enabled,
                            int* extreme_filter_divergence,
                            float filter_step_size,
                            float error_threshold,
                            float* x_fft,
                            int* x_fft_buf_block_pos,
                            float x_fft_buf[2]
                                           [kExtendedNumPartitions * PART_LEN1],
                            float* const y,
                            float x_pow[PART_LEN1],
                            float h_fft_buf[2]
                                           [kExtendedNumPartitions * PART_LEN1],
                            float echo_subtractor_output[PART_LEN]) {
  float s_fft[2][PART_LEN1];
  float e_extended[PART_LEN2];
  float s_extended[PART_LEN2];
  float* s;
  float e[PART_LEN];
  float e_fft[2][PART_LEN1];
  int i;

  // Update the x_fft_buf block position.
  (*x_fft_buf_block_pos)--;
  if ((*x_fft_buf_block_pos) == -1) {
    *x_fft_buf_block_pos = num_partitions - 1;
  }

  // Buffer x_fft.
  memcpy(x_fft_buf[0] + (*x_fft_buf_block_pos) * PART_LEN1, x_fft,
         sizeof(float) * PART_LEN1);
  memcpy(x_fft_buf[1] + (*x_fft_buf_block_pos) * PART_LEN1, &x_fft[PART_LEN1],
         sizeof(float) * PART_LEN1);

  memset(s_fft, 0, sizeof(s_fft));

  // Conditionally reset the echo subtraction filter if the filter has diverged
  // significantly.
  if (!extended_filter_enabled && *extreme_filter_divergence) {
    memset(h_fft_buf, 0,
           2 * kExtendedNumPartitions * PART_LEN1 * sizeof(h_fft_buf[0][0]));
    *extreme_filter_divergence = 0;
  }

  // Produce echo estimate s_fft.
  WebRtcAec_FilterFar(num_partitions, *x_fft_buf_block_pos, x_fft_buf,
                      h_fft_buf, s_fft);

  // Compute the time-domain echo estimate s.
  ScaledInverseFft(s_fft, s_extended, 2.0f, 0);
  s = &s_extended[PART_LEN];

  // Compute the time-domain echo prediction error.
  for (i = 0; i < PART_LEN; ++i) {
    e[i] = y[i] - s[i];
  }

  // Compute the frequency domain echo prediction error.
  memset(e_extended, 0, sizeof(float) * PART_LEN);
  memcpy(e_extended + PART_LEN, e, sizeof(float) * PART_LEN);
  Fft(e_extended, e_fft);

  // Scale error signal inversely with far power.
  WebRtcAec_ScaleErrorSignal(filter_step_size, error_threshold, x_pow, e_fft);
  WebRtcAec_FilterAdaptation(num_partitions, *x_fft_buf_block_pos, x_fft_buf,
                             e_fft, h_fft_buf);
  memcpy(echo_subtractor_output, e, sizeof(float) * PART_LEN);
}

//static void FormSuppressionGain(AecCore* aec,
//                                float cohde[PART_LEN1],
//                                float cohxd[PART_LEN1],
//                                float hNl[PART_LEN1]) {
//  float hNlDeAvg, hNlXdAvg;
//  float hNlPref[kPrefBandSize];
//  float hNlFb = 0, hNlFbLow = 0;
//  const int prefBandSize = kPrefBandSize / aec->mult;
//  const float prefBandQuant = 0.75f, prefBandQuantLow = 0.5f;
//  const int minPrefBand = 4 / aec->mult;
//  // Power estimate smoothing coefficients.
//  const float* min_overdrive = aec->extended_filter_enabled
//                                   ? kExtendedMinOverDrive
//                                   : kNormalMinOverDrive;
//
//  hNlXdAvg = 0;
//  for (int i = minPrefBand; i < prefBandSize + minPrefBand; ++i) {
//    hNlXdAvg += cohxd[i];
//  }
//  hNlXdAvg /= prefBandSize;
//  hNlXdAvg = 1 - hNlXdAvg;
//
//  hNlDeAvg = 0;
//  for (int i = minPrefBand; i < prefBandSize + minPrefBand; ++i) {
//    hNlDeAvg += cohde[i];
//  }
//  hNlDeAvg /= prefBandSize;
//
//  if (hNlXdAvg < 0.75f && hNlXdAvg < aec->hNlXdAvgMin) {
//    aec->hNlXdAvgMin = hNlXdAvg;
//  }
//
//  if (hNlDeAvg > 0.98f && hNlXdAvg > 0.9f) {
//    aec->stNearState = 1;
//  } else if (hNlDeAvg < 0.95f || hNlXdAvg < 0.8f) {
//    aec->stNearState = 0;
//  }
//
//  if (aec->hNlXdAvgMin == 1) {
//    aec->echoState = 0;
//    aec->overDrive = min_overdrive[aec->nlp_mode];
//
//    if (aec->stNearState == 1) {
//      memcpy(hNl, cohde, sizeof(hNl[0]) * PART_LEN1);
//      hNlFb = hNlDeAvg;
//      hNlFbLow = hNlDeAvg;
//    } else {
//      for (int i = 0; i < PART_LEN1; ++i) {
//        hNl[i] = 1 - cohxd[i];
//      }
//      hNlFb = hNlXdAvg;
//      hNlFbLow = hNlXdAvg;
//    }
//  } else {
//    if (aec->stNearState == 1) {
//      aec->echoState = 0;
//      memcpy(hNl, cohde, sizeof(hNl[0]) * PART_LEN1);
//      hNlFb = hNlDeAvg;
//      hNlFbLow = hNlDeAvg;
//    } else {
//      aec->echoState = 1;
//      for (int i = 0; i < PART_LEN1; ++i) {
//        hNl[i] = WEBRTC_SPL_MIN(cohde[i], 1 - cohxd[i]);
//      }
//
//      // Select an order statistic from the preferred bands.
//      // TODO(peah): Using quicksort now, but a selection algorithm may be
//      // preferred.
//      memcpy(hNlPref, &hNl[minPrefBand], sizeof(float) * prefBandSize);
//      qsort(hNlPref, prefBandSize, sizeof(float), CmpFloat);
//      hNlFb = hNlPref[static_cast<int>(floor(prefBandQuant *
//                                             (prefBandSize - 1)))];
//      hNlFbLow = hNlPref[static_cast<int>(floor(prefBandQuantLow *
//                                                (prefBandSize - 1)))];
//    }
//  }
//
//  // Track the local filter minimum to determine suppression overdrive.
//  if (hNlFbLow < 0.6f && hNlFbLow < aec->hNlFbLocalMin) {
//    aec->hNlFbLocalMin = hNlFbLow;
//    aec->hNlFbMin = hNlFbLow;
//    aec->hNlNewMin = 1;
//    aec->hNlMinCtr = 0;
//  }
//  aec->hNlFbLocalMin =
//      WEBRTC_SPL_MIN(aec->hNlFbLocalMin + 0.0008f / aec->mult, 1);
//  aec->hNlXdAvgMin = WEBRTC_SPL_MIN(aec->hNlXdAvgMin + 0.0006f / aec->mult, 1);
//
//  if (aec->hNlNewMin == 1) {
//    aec->hNlMinCtr++;
//  }
//  if (aec->hNlMinCtr == 2) {
//    aec->hNlNewMin = 0;
//    aec->hNlMinCtr = 0;
//    aec->overDrive =
//        WEBRTC_SPL_MAX(kTargetSupp[aec->nlp_mode] /
//                       static_cast<float>(log(aec->hNlFbMin + 1e-10f) + 1e-10f),
//                       min_overdrive[aec->nlp_mode]);
//  }
//
//  // Smooth the overdrive.
//  if (aec->overDrive < aec->overdrive_scaling) {
//    aec->overdrive_scaling =
//        0.99f * aec->overdrive_scaling + 0.01f * aec->overDrive;
//  } else {
//    aec->overdrive_scaling =
//        0.9f * aec->overdrive_scaling + 0.1f * aec->overDrive;
//  }
//
//  // Apply the overdrive.
//  WebRtcAec_Overdrive(aec->overdrive_scaling, hNlFb, hNl);
//}

//static void EchoSuppression(AecCore* aec,
//                            float* nearend_extended_block_lowest_band,
//                            float farend_extended_block[PART_LEN2],
//                            float* echo_subtractor_output,
//                            float output[NUM_HIGH_BANDS_MAX + 1][PART_LEN]) {
//  float efw[2][PART_LEN1];
//  float xfw[2][PART_LEN1];
//  float dfw[2][PART_LEN1];
//  float comfortNoiseHband[2][PART_LEN1];
//  float fft[PART_LEN2];
//  float nlpGainHband;
//  int i;
//  size_t j;
//
//  // Coherence and non-linear filter
//  float cohde[PART_LEN1], cohxd[PART_LEN1];
//  float hNl[PART_LEN1];
//
//  // Filter energy
//  const int delayEstInterval = 10 * aec->mult;
//
//  float* xfw_ptr = NULL;
//
//  // Update eBuf with echo subtractor output.
//  memcpy(aec->eBuf + PART_LEN, echo_subtractor_output,
//         sizeof(float) * PART_LEN);
//
//  // Analysis filter banks for the echo suppressor.
//  // Windowed near-end ffts.
//  WindowData(fft, nearend_extended_block_lowest_band);
//  aec_rdft_forward_128(fft);
//  StoreAsComplex(fft, dfw);
//
//  // Windowed echo suppressor output ffts.
//  WindowData(fft, aec->eBuf);
//  aec_rdft_forward_128(fft);
//  StoreAsComplex(fft, efw);
//
//  // NLP
//
//  // Convert far-end partition to the frequency domain with windowing.
//  WindowData(fft, farend_extended_block);
//  Fft(fft, xfw);
//  xfw_ptr = &xfw[0][0];
//
//  // Buffer far.
//  memcpy(aec->xfwBuf, xfw_ptr, sizeof(float) * 2 * PART_LEN1);
//
//  aec->delayEstCtr++;
//  if (aec->delayEstCtr == delayEstInterval) {
//    aec->delayEstCtr = 0;
//    aec->delayIdx = WebRtcAec_PartitionDelay(aec->num_partitions, aec->wfBuf);
//  }
//
//  aec->data_dumper->DumpRaw("aec_nlp_delay", 1, &aec->delayIdx);
//
//  // Use delayed far.
//  memcpy(xfw, aec->xfwBuf + aec->delayIdx * PART_LEN1,
//         sizeof(xfw[0][0]) * 2 * PART_LEN1);
//
//  WebRtcAec_UpdateCoherenceSpectra(aec->mult, aec->extended_filter_enabled == 1,
//                                   efw, dfw, xfw, &aec->coherence_state,
//                                   &aec->divergeState,
//                                   &aec->extreme_filter_divergence);
//
//  WebRtcAec_ComputeCoherence(&aec->coherence_state, cohde, cohxd);
//
//  // Select the microphone signal as output if the filter is deemed to have
//  // diverged.
//  if (aec->divergeState) {
//    memcpy(efw, dfw, sizeof(efw[0][0]) * 2 * PART_LEN1);
//  }
//
//  FormSuppressionGain(aec, cohde, cohxd, hNl);
//
//  aec->data_dumper->DumpRaw("aec_nlp_gain", PART_LEN1, hNl);
//
//  WebRtcAec_Suppress(hNl, efw);
//
//  // Add comfort noise.
//  ComfortNoise(aec->num_bands > 1, &aec->seed, efw, comfortNoiseHband,
//               aec->noisePow, hNl);
//
//  // Inverse error fft.
//  ScaledInverseFft(efw, fft, 2.0f, 1);
//
//  // Overlap and add to obtain output.
//  for (i = 0; i < PART_LEN; i++) {
//    output[0][i] = (fft[i] * WebRtcAec_sqrtHanning[i] +
//                    aec->outBuf[i] * WebRtcAec_sqrtHanning[PART_LEN - i]);
//
//    // Saturate output to keep it in the allowed range.
//    output[0][i] = WEBRTC_SPL_SAT(WEBRTC_SPL_WORD16_MAX, output[0][i],
//                                  WEBRTC_SPL_WORD16_MIN);
//  }
//  memcpy(aec->outBuf, &fft[PART_LEN], PART_LEN * sizeof(aec->outBuf[0]));
//
//  // For H band
//  if (aec->num_bands > 1) {
//    // H band gain
//    // average nlp over low band: average over second half of freq spectrum
//    // (4->8khz)
//    GetHighbandGain(hNl, &nlpGainHband);
//
//    // Inverse comfort_noise
//    ScaledInverseFft(comfortNoiseHband, fft, 2.0f, 0);
//
//    // compute gain factor
//    for (j = 1; j < aec->num_bands; ++j) {
//      for (i = 0; i < PART_LEN; i++) {
//        output[j][i] = aec->previous_nearend_block[j][i] * nlpGainHband;
//      }
//    }
//
//    // Add some comfort noise where Hband is attenuated.
//    for (i = 0; i < PART_LEN; i++) {
//      output[1][i] += cnScaleHband * fft[i];
//    }
//
//    // Saturate output to keep it in the allowed range.
//    for (j = 1; j < aec->num_bands; ++j) {
//      for (i = 0; i < PART_LEN; i++) {
//        output[j][i] = WEBRTC_SPL_SAT(WEBRTC_SPL_WORD16_MAX, output[j][i],
//                                      WEBRTC_SPL_WORD16_MIN);
//      }
//    }
//  }
//
//  // Copy the current block to the old position.
//  memcpy(aec->eBuf, aec->eBuf + PART_LEN, sizeof(float) * PART_LEN);
//
//  memmove(aec->xfwBuf + PART_LEN1, aec->xfwBuf,
//          sizeof(aec->xfwBuf) - sizeof(complex_t) * PART_LEN1);
//}

static void ProcessNearendBlock(
    AecCore* aec,
    float farend_extended_block_lowest_band[PART_LEN2],
    float nearend_block[NUM_HIGH_BANDS_MAX + 1][PART_LEN],
    float output_block[NUM_HIGH_BANDS_MAX + 1][PART_LEN]) {
#ifdef AEC_RECORD_DEBUG
	short temp_near[PART_LEN] = { 0 };
	short temp_far[PART_LEN] = { 0 };
	short temp_linear_out[PART_LEN] = { 0 };
	short temp_out[PART_LEN] = { 0 };
	short index = 0;
	if (aec->near_in != NULL)
	{
		for (index = 0; index < PART_LEN; index++)
		{
			temp_near[index] = nearend_block[0][index];
		}
		fwrite(temp_near, sizeof(short), PART_LEN, aec->near_in);
	}
	if (aec->far_in != NULL)
	{
		for (index = 0; index < PART_LEN; index++)
		{
			temp_far[index] = farend_extended_block_lowest_band[index];
		}
		fwrite(temp_far, sizeof(short), PART_LEN, aec->far_in);
	}
#endif
#ifndef AEC_OPTIMIZE
  size_t i;

  float fft[PART_LEN2];
  float nearend_extended_block_lowest_band[PART_LEN2];
  float farend_fft[2][PART_LEN1];
  float nearend_fft[2][PART_LEN1];
  float far_spectrum = 0.0f;
  float near_spectrum = 0.0f;
  float abs_far_spectrum[PART_LEN1];
  float abs_near_spectrum[PART_LEN1];

  const float gPow[2] = {0.9f, 0.1f};

  // Noise estimate constants.
  const int noiseInitBlocks = 500 * aec->mult;
  const float step = 0.1f;
  const float ramp = 1.0002f;
  const float gInitNoise[2] = {0.999f, 0.001f};

  float echo_subtractor_output[PART_LEN];

  aec->data_dumper->DumpWav("aec_far", PART_LEN,
                            &farend_extended_block_lowest_band[PART_LEN],
                            std::min(aec->sampFreq, 16000), 1);
  aec->data_dumper->DumpWav("aec_near", PART_LEN, &nearend_block[0][0],
                            std::min(aec->sampFreq, 16000), 1);

  if (aec->metricsMode == 1) {
    // Update power levels
    UpdateLevel(
        &aec->farlevel,
        CalculatePower(&farend_extended_block_lowest_band[PART_LEN], PART_LEN));
    UpdateLevel(&aec->nearlevel,
                CalculatePower(&nearend_block[0][0], PART_LEN));
  }

  // Convert far-end signal to the frequency domain.
  memcpy(fft, farend_extended_block_lowest_band, sizeof(float) * PART_LEN2);
  Fft(fft, farend_fft);

  // Form extended nearend frame.
  memcpy(&nearend_extended_block_lowest_band[0],
         &aec->previous_nearend_block[0][0], sizeof(float) * PART_LEN);
  memcpy(&nearend_extended_block_lowest_band[PART_LEN], &nearend_block[0][0],
         sizeof(float) * PART_LEN);

  // Convert near-end signal to the frequency domain.
  memcpy(fft, nearend_extended_block_lowest_band, sizeof(float) * PART_LEN2);
  Fft(fft, nearend_fft);

  // Power smoothing.
  if (aec->refined_adaptive_filter_enabled) {
    for (i = 0; i < PART_LEN1; ++i) {
      far_spectrum = farend_fft[0][i] * farend_fft[0][i] +
                     farend_fft[1][i] * farend_fft[1][i];
      // Calculate the magnitude spectrum.
      abs_far_spectrum[i] = sqrtf(far_spectrum);
    }
    RegressorPower(aec->num_partitions, aec->xfBufBlockPos, aec->xfBuf,
                   aec->xPow);
  } else {
    for (i = 0; i < PART_LEN1; ++i) {
      far_spectrum = farend_fft[0][i] * farend_fft[0][i] +
                     farend_fft[1][i] * farend_fft[1][i];
      aec->xPow[i] =
          gPow[0] * aec->xPow[i] + gPow[1] * aec->num_partitions * far_spectrum;
      // Calculate the magnitude spectrum.
      abs_far_spectrum[i] = sqrtf(far_spectrum);
    }
  }

  for (i = 0; i < PART_LEN1; ++i) {
    near_spectrum = nearend_fft[0][i] * nearend_fft[0][i] +
                    nearend_fft[1][i] * nearend_fft[1][i];
    aec->dPow[i] = gPow[0] * aec->dPow[i] + gPow[1] * near_spectrum;
    // Calculate the magnitude spectrum.
    abs_near_spectrum[i] = sqrtf(near_spectrum);
  }

  // Estimate noise power. Wait until dPow is more stable.
  if (aec->noiseEstCtr > 50) {
    for (i = 0; i < PART_LEN1; i++) {
      if (aec->dPow[i] < aec->dMinPow[i]) {
        aec->dMinPow[i] =
            (aec->dPow[i] + step * (aec->dMinPow[i] - aec->dPow[i])) * ramp;
      } else {
        aec->dMinPow[i] *= ramp;
      }
    }
  }

  // Smooth increasing noise power from zero at the start,
  // to avoid a sudden burst of comfort noise.
  if (aec->noiseEstCtr < noiseInitBlocks) {
    aec->noiseEstCtr++;
    for (i = 0; i < PART_LEN1; i++) {
      if (aec->dMinPow[i] > aec->dInitMinPow[i]) {
        aec->dInitMinPow[i] = gInitNoise[0] * aec->dInitMinPow[i] +
                              gInitNoise[1] * aec->dMinPow[i];
      } else {
        aec->dInitMinPow[i] = aec->dMinPow[i];
      }
    }
    aec->noisePow = aec->dInitMinPow;
  } else {
    aec->noisePow = aec->dMinPow;
  }

  // Block wise delay estimation used for logging
  if (aec->delay_logging_enabled) {
    if (WebRtc_AddFarSpectrumFloat(aec->delay_estimator_farend,
                                   abs_far_spectrum, PART_LEN1) == 0) {
      int delay_estimate = WebRtc_DelayEstimatorProcessFloat(
          aec->delay_estimator, abs_near_spectrum, PART_LEN1);
      if (delay_estimate >= 0) {
        // Update delay estimate buffer.
        aec->delay_histogram[delay_estimate]++;
        aec->num_delay_values++;
      }
      if (aec->delay_metrics_delivered == 1 &&
          aec->num_delay_values >= kDelayMetricsAggregationWindow) {
        UpdateDelayMetrics(aec);
      }
    }
  }

  // Perform echo subtraction.
  EchoSubtraction(aec->num_partitions, aec->extended_filter_enabled,
                  &aec->extreme_filter_divergence, aec->filter_step_size,
                  aec->error_threshold, &farend_fft[0][0], &aec->xfBufBlockPos,
                  aec->xfBuf, &nearend_block[0][0], aec->xPow, aec->wfBuf,
                  echo_subtractor_output);
  aec->data_dumper->DumpRaw("aec_h_fft", PART_LEN1 * aec->num_partitions,
                            &aec->wfBuf[0][0]);
  aec->data_dumper->DumpRaw("aec_h_fft", PART_LEN1 * aec->num_partitions,
                            &aec->wfBuf[1][0]);

  aec->data_dumper->DumpWav("aec_out_linear", PART_LEN, echo_subtractor_output,
                            std::min(aec->sampFreq, 16000), 1);

  if (aec->metricsMode == 1) {
    UpdateLevel(&aec->linoutlevel,
                CalculatePower(echo_subtractor_output, PART_LEN));
  }

  // Perform echo suppression.
  EchoSuppression(aec, nearend_extended_block_lowest_band,
                  farend_extended_block_lowest_band, echo_subtractor_output,
                  output_block);

  if (aec->metricsMode == 1) {
    UpdateLevel(&aec->nlpoutlevel,
                CalculatePower(&output_block[0][0], PART_LEN));
    UpdateMetrics(aec);
  }

  // Store the nearend signal until the next frame.
  for (i = 0; i < aec->num_bands; ++i) {
    memcpy(&aec->previous_nearend_block[i][0], &nearend_block[i][0],
           sizeof(float) * PART_LEN);
  }
#ifdef AEC_RECORD_DEBUG
  if (aec->aec_out != NULL)
  {
	  for (int indexj = 0; indexj < PART_LEN; indexj++)
	  {
		  temp_out[indexj] = output_block[0][indexj];
	  }
	  fwrite(temp_out, sizeof(short), PART_LEN, aec->aec_out);
	}
#endif

  aec->data_dumper->DumpWav("aec_out", PART_LEN, &output_block[0][0],
                            std::min(aec->sampFreq, 16000), 1);
#else
	float *nearend_in[NUM_HIGH_BANDS_MAX + 1];
	float *out[NUM_HIGH_BANDS_MAX + 1];
	int bands = 1;

	out[0] = output_block[0];
	out[1] = output_block[1];
	out[2] = output_block[2];
	nearend_in[0] = nearend_block[0];
	nearend_in[1] = nearend_block[1];
	nearend_in[2] = nearend_block[2];

	if (aec->sampFreq == 8000)
	{
		bands = 1;
	}
	else
	{
		bands = aec->sampFreq / 16000;
	}
	WebRtcAec_BufferFarend_New(aec->aecnewInst, (const float*)(farend_extended_block_lowest_band), PART_LEN);
	WebRtcAec_Process_New(aec->aecnewInst, (const float* const*)(&nearend_in[0]), bands, (float* const*)(&out[0]), PART_LEN, 0, 0);

#ifdef AEC_RECORD_DEBUG
	if (aec->aec_out != NULL)
	{
		for (index = 0; index < PART_LEN; index++)
		{
			temp_out[index] = output_block[0][index];
		}
		fwrite(temp_out, sizeof(short), PART_LEN, aec->aec_out);
	}
#endif
#endif
}

AecCore* WebRtcAec_CreateAec(int instance_count) {
  AecCore* aec = new AecCore(instance_count);

  if (!aec) {
    return NULL;
  }
  aec->nearend_buffer_size = 0;
  memset(&aec->nearend_buffer[0], 0, sizeof(aec->nearend_buffer));
  // Start the output buffer with zeros to be able to produce
  // a full output frame in the first frame.
  aec->output_buffer_size = PART_LEN - (FRAME_LEN - PART_LEN);
  memset(&aec->output_buffer[0], 0, sizeof(aec->output_buffer));

  aec->delay_estimator_farend =
      WebRtc_CreateDelayEstimatorFarend(PART_LEN1, kHistorySizeBlocks);
  if (aec->delay_estimator_farend == NULL) {
    WebRtcAec_FreeAec(aec);
    return NULL;
  }
  // We create the delay_estimator with the same amount of maximum lookahead as
  // the delay history size (kHistorySizeBlocks) for symmetry reasons.
  aec->delay_estimator = WebRtc_CreateDelayEstimator(
      aec->delay_estimator_farend, kHistorySizeBlocks);
  if (aec->delay_estimator == NULL) {
    WebRtcAec_FreeAec(aec);
    return NULL;
  }
#ifdef WEBRTC_ANDROID
  aec->delay_agnostic_enabled = 1;  // DA-AEC enabled by default.
  // DA-AEC assumes the system is causal from the beginning and will self adjust
  // the lookahead when shifting is required.
  WebRtc_set_lookahead(aec->delay_estimator, 0);
#else
  aec->delay_agnostic_enabled = 0;
  WebRtc_set_lookahead(aec->delay_estimator, kLookaheadBlocks);
#endif
  aec->extended_filter_enabled = 0;
  aec->aec3_enabled = 0;
  aec->refined_adaptive_filter_enabled = false;
#ifndef AEC_OPTIMIZE
  // Assembly optimization
  WebRtcAec_FilterFar = FilterFar;
  WebRtcAec_ScaleErrorSignal = ScaleErrorSignal;
  WebRtcAec_FilterAdaptation = FilterAdaptation;
  WebRtcAec_Overdrive = Overdrive;
  WebRtcAec_Suppress = Suppress;
  WebRtcAec_ComputeCoherence = ComputeCoherence;
  WebRtcAec_UpdateCoherenceSpectra = UpdateCoherenceSpectra;
  WebRtcAec_StoreAsComplex = StoreAsComplex;
  WebRtcAec_PartitionDelay = PartitionDelay;
  WebRtcAec_WindowData = WindowData;
#endif
#if defined(WEBRTC_ARCH_X86_FAMILY)
  if (WebRtc_GetCPUInfo(kSSE2)) {
    WebRtcAec_InitAec_SSE2();
  }
#endif

#if defined(MIPS_FPU_LE)
  WebRtcAec_InitAec_mips();
#endif

#if defined(WEBRTC_HAS_NEON)
  WebRtcAec_InitAec_neon();
#endif

  aec_rdft_init();
#ifdef AEC_RECORD_DEBUG
  aec->near_in = NULL;
  aec->far_in = NULL;
  aec->aec_out = NULL;
#endif
#ifdef AEC_OPTIMIZE
  aec->aecnewInst = WebRtcAec_Create_New();
#endif

  return aec;
}

void WebRtcAec_FreeAec(AecCore* aec) {
  if (aec == NULL) {
    return;
  }
#ifdef AEC_RECORD_DEBUG
  if (aec->near_in != NULL)
  {
	  fclose(aec->near_in);
	  aec->near_in = NULL;
  }
  if (aec->far_in != NULL)
  {
	  fclose(aec->far_in);
	  aec->far_in = NULL;
  }
  if (aec->aec_out != NULL)
  {
	  fclose(aec->aec_out);
	  aec->aec_out = NULL;
  }
#endif
  WebRtc_FreeDelayEstimator(aec->delay_estimator);
  WebRtc_FreeDelayEstimatorFarend(aec->delay_estimator_farend);
#ifdef AEC_OPTIMIZE
  WebRtcAec_Free_New(aec->aecnewInst);
#endif
  delete aec;
}

static void SetAdaptiveFilterStepSize(AecCore* aec) {
  // Extended filter adaptation parameter.
  // TODO(ajm): No narrowband tuning yet.
  const float kExtendedMu = 0.4f;

  if (aec->refined_adaptive_filter_enabled) {
    aec->filter_step_size = 0.05f;
  } else {
    if (aec->extended_filter_enabled) {
      aec->filter_step_size = kExtendedMu;
    } else {
      if (aec->sampFreq == 8000) {
        aec->filter_step_size = 0.6f;
      } else {
        aec->filter_step_size = 0.5f;
      }
    }
  }
}

static void SetErrorThreshold(AecCore* aec) {
  // Extended filter adaptation parameter.
  // TODO(ajm): No narrowband tuning yet.
  static const float kExtendedErrorThreshold = 1.0e-6f;

  if (aec->extended_filter_enabled) {
    aec->error_threshold = kExtendedErrorThreshold;
  } else {
    if (aec->sampFreq == 8000) {
      aec->error_threshold = 2e-6f;
    } else {
      aec->error_threshold = 1.5e-6f;
    }
  }
}

int WebRtcAec_InitAec(AecCore* aec, int sampFreq) {
  int i;
  aec->data_dumper->InitiateNewSetOfRecordings();

  aec->sampFreq = sampFreq;

  SetAdaptiveFilterStepSize(aec);
  SetErrorThreshold(aec);

  if (sampFreq == 8000) {
    aec->num_bands = 1;
  } else {
    aec->num_bands = (size_t)(sampFreq / 16000);
  }

  // Start the output buffer with zeros to be able to produce
  // a full output frame in the first frame.
  aec->output_buffer_size = PART_LEN - (FRAME_LEN - PART_LEN);
  memset(&aec->output_buffer[0], 0, sizeof(aec->output_buffer));
  aec->nearend_buffer_size = 0;
  memset(&aec->nearend_buffer[0], 0, sizeof(aec->nearend_buffer));

  // Initialize far-end buffer.
  aec->farend_block_buffer_.ReInit();

  aec->system_delay = 0;

  if (WebRtc_InitDelayEstimatorFarend(aec->delay_estimator_farend) != 0) {
    return -1;
  }
  if (WebRtc_InitDelayEstimator(aec->delay_estimator) != 0) {
    return -1;
  }
  aec->delay_logging_enabled = 0;
  aec->delay_metrics_delivered = 0;
  memset(aec->delay_histogram, 0, sizeof(aec->delay_histogram));
  aec->num_delay_values = 0;
  aec->delay_median = -1;
  aec->delay_std = -1;
  aec->fraction_poor_delays = -1.0f;

  aec->previous_delay = -2;  // (-2): Uninitialized.
  aec->delay_correction_count = 0;
  aec->shift_offset = kInitialShiftOffset;
  aec->delay_quality_threshold = kDelayQualityThresholdMin;

  aec->num_partitions = kNormalNumPartitions;

  // Update the delay estimator with filter length.  We use half the
  // |num_partitions| to take the echo path into account.  In practice we say
  // that the echo has a duration of maximum half |num_partitions|, which is not
  // true, but serves as a crude measure.
  WebRtc_set_allowed_offset(aec->delay_estimator, aec->num_partitions / 2);
  // TODO(bjornv): I currently hard coded the enable.  Once we've established
  // that AECM has no performance regression, robust_validation will be enabled
  // all the time and the APIs to turn it on/off will be removed.  Hence, remove
  // this line then.
  WebRtc_enable_robust_validation(aec->delay_estimator, 1);
  aec->frame_count = 0;

  // Default target suppression mode.
  aec->nlp_mode = 1;

  // Sampling frequency multiplier w.r.t. 8 kHz.
  // In case of multiple bands we process the lower band in 16 kHz, hence the
  // multiplier is always 2.
  if (aec->num_bands > 1) {
    aec->mult = 2;
  } else {
    aec->mult = static_cast<int16_t>(aec->sampFreq) / 8000;
  }

  aec->farBufWritePos = 0;
  aec->farBufReadPos = 0;

  aec->inSamples = 0;
  aec->outSamples = 0;
  aec->knownDelay = 0;

  // Initialize buffers
  memset(aec->previous_nearend_block, 0, sizeof(aec->previous_nearend_block));
  memset(aec->eBuf, 0, sizeof(aec->eBuf));

  memset(aec->xPow, 0, sizeof(aec->xPow));
  memset(aec->dPow, 0, sizeof(aec->dPow));
  memset(aec->dInitMinPow, 0, sizeof(aec->dInitMinPow));
  aec->noisePow = aec->dInitMinPow;
  aec->noiseEstCtr = 0;

  // Initial comfort noise power
  for (i = 0; i < PART_LEN1; i++) {
    aec->dMinPow[i] = 1.0e6f;
  }

  // Holds the last block written to
  aec->xfBufBlockPos = 0;
  // TODO(peah): Investigate need for these initializations. Deleting them
  // doesn't change the output at all and yields 0.4% overall speedup.
  memset(aec->xfBuf, 0, sizeof(complex_t) * kExtendedNumPartitions * PART_LEN1);
  memset(aec->wfBuf, 0, sizeof(complex_t) * kExtendedNumPartitions * PART_LEN1);
  memset(aec->coherence_state.sde, 0, sizeof(complex_t) * PART_LEN1);
  memset(aec->coherence_state.sxd, 0, sizeof(complex_t) * PART_LEN1);
  memset(aec->xfwBuf, 0,
         sizeof(complex_t) * kExtendedNumPartitions * PART_LEN1);
  memset(aec->coherence_state.se, 0, sizeof(float) * PART_LEN1);

  // To prevent numerical instability in the first block.
  for (i = 0; i < PART_LEN1; i++) {
    aec->coherence_state.sd[i] = 1;
  }
  for (i = 0; i < PART_LEN1; i++) {
    aec->coherence_state.sx[i] = 1;
  }

  memset(aec->hNs, 0, sizeof(aec->hNs));
  memset(aec->outBuf, 0, sizeof(float) * PART_LEN);

  aec->hNlFbMin = 1;
  aec->hNlFbLocalMin = 1;
  aec->hNlXdAvgMin = 1;
  aec->hNlNewMin = 0;
  aec->hNlMinCtr = 0;
  aec->overDrive = 2;
  aec->overdrive_scaling = 2;
  aec->delayIdx = 0;
  aec->stNearState = 0;
  aec->echoState = 0;
  aec->divergeState = 0;

  aec->seed = 777;
  aec->delayEstCtr = 0;

  aec->extreme_filter_divergence = 0;

  // Metrics disabled by default
  aec->metricsMode = 0;
  InitMetrics(aec);
#ifdef AEC_RECORD_DEBUG
  if (aec->near_in == NULL)
  {
	  aec->near_in = fopen("e://aec_near_in.pcm", "wb");
  }
  if (aec->far_in == NULL)
  {
	  aec->far_in = fopen("e://aec_far_in.pcm", "wb");
  }
  if (aec->aec_out == NULL)
  {
	  aec->aec_out = fopen("e://aec_out.pcm", "wb");
  }
#endif
#ifdef AEC_OPTIMIZE
  WebRtcAec_Init_New(aec->aecnewInst, sampFreq, sampFreq);
#endif

  return 0;
}

void WebRtcAec_BufferFarendBlock(AecCore* aec, const float* farend) {
  // Check if the buffer is full, and in that case flush the oldest data.
  if (aec->farend_block_buffer_.AvaliableSpace() < 1) {
    aec->farend_block_buffer_.AdjustSize(1);
  }
  aec->farend_block_buffer_.Insert(farend);
}

int WebRtcAec_AdjustFarendBufferSizeAndSystemDelay(AecCore* aec,
                                                   int buffer_size_decrease) {
  int achieved_buffer_size_decrease =
      aec->farend_block_buffer_.AdjustSize(buffer_size_decrease);
  aec->system_delay -= achieved_buffer_size_decrease * PART_LEN;
  return achieved_buffer_size_decrease;
}

void FormNearendBlock(
    size_t nearend_start_index,
    size_t num_bands,
    const float* const* nearend_frame,
    size_t num_samples_from_nearend_frame,
    const float nearend_buffer[NUM_HIGH_BANDS_MAX + 1]
                              [PART_LEN - (FRAME_LEN - PART_LEN)],
    float nearend_block[NUM_HIGH_BANDS_MAX + 1][PART_LEN]) {
  RTC_DCHECK_LE(num_samples_from_nearend_frame, static_cast<size_t>(PART_LEN));
  const int num_samples_from_buffer = PART_LEN - num_samples_from_nearend_frame;

  if (num_samples_from_buffer > 0) {
    for (size_t i = 0; i < num_bands; ++i) {
      memcpy(&nearend_block[i][0], &nearend_buffer[i][0],
             num_samples_from_buffer * sizeof(float));
    }
  }

  for (size_t i = 0; i < num_bands; ++i) {
    memcpy(&nearend_block[i][num_samples_from_buffer],
           &nearend_frame[i][nearend_start_index],
           num_samples_from_nearend_frame * sizeof(float));
  }
}

void BufferNearendFrame(
    size_t nearend_start_index,
    size_t num_bands,
    const float* const* nearend_frame,
    size_t num_samples_to_buffer,
    float nearend_buffer[NUM_HIGH_BANDS_MAX + 1]
                        [PART_LEN - (FRAME_LEN - PART_LEN)]) {
  for (size_t i = 0; i < num_bands; ++i) {
    memcpy(
        &nearend_buffer[i][0],
        &nearend_frame[i]
                      [nearend_start_index + FRAME_LEN - num_samples_to_buffer],
        num_samples_to_buffer * sizeof(float));
  }
}

void BufferOutputBlock(size_t num_bands,
                       const float output_block[NUM_HIGH_BANDS_MAX + 1]
                                               [PART_LEN],
                       size_t* output_buffer_size,
                       float output_buffer[NUM_HIGH_BANDS_MAX + 1]
                                          [2 * PART_LEN]) {
  for (size_t i = 0; i < num_bands; ++i) {
    memcpy(&output_buffer[i][*output_buffer_size], &output_block[i][0],
           PART_LEN * sizeof(float));
  }
  (*output_buffer_size) += PART_LEN;
}

void FormOutputFrame(size_t output_start_index,
                     size_t num_bands,
                     size_t* output_buffer_size,
                     float output_buffer[NUM_HIGH_BANDS_MAX + 1][2 * PART_LEN],
                     float* const* output_frame) {
  RTC_DCHECK_LE(static_cast<size_t>(FRAME_LEN), *output_buffer_size);
  for (size_t i = 0; i < num_bands; ++i) {
    memcpy(&output_frame[i][output_start_index], &output_buffer[i][0],
           FRAME_LEN * sizeof(float));
  }
  (*output_buffer_size) -= FRAME_LEN;
  if (*output_buffer_size > 0) {
    RTC_DCHECK_GE(static_cast<size_t>(2 * PART_LEN - FRAME_LEN),
                  (*output_buffer_size));
    for (size_t i = 0; i < num_bands; ++i) {
      memcpy(&output_buffer[i][0], &output_buffer[i][FRAME_LEN],
             (*output_buffer_size) * sizeof(float));
    }
  }
}

void WebRtcAec_ProcessFrames(AecCore* aec,
                             const float* const* nearend,
                             size_t num_bands,
                             size_t num_samples,
                             int knownDelay,
                             float* const* out) {
  RTC_DCHECK(num_samples == 80 || num_samples == 160);

  aec->frame_count++;
  // For each frame the process is as follows:
  // 1) If the system_delay indicates on being too small for processing a
  //    frame we stuff the buffer with enough data for 10 ms.
  // 2 a) Adjust the buffer to the system delay, by moving the read pointer.
  //   b) Apply signal based delay correction, if we have detected poor AEC
  //    performance.
  // 3) TODO(bjornv): Investigate if we need to add this:
  //    If we can't move read pointer due to buffer size limitations we
  //    flush/stuff the buffer.
  // 4) Process as many partitions as possible.
  // 5) Update the |system_delay| with respect to a full frame of FRAME_LEN
  //    samples. Even though we will have data left to process (we work with
  //    partitions) we consider updating a whole frame, since that's the
  //    amount of data we input and output in audio_processing.
  // 6) Update the outputs.

  // The AEC has two different delay estimation algorithms built in.  The
  // first relies on delay input values from the user and the amount of
  // shifted buffer elements is controlled by |knownDelay|.  This delay will
  // give a guess on how much we need to shift far-end buffers to align with
  // the near-end signal.  The other delay estimation algorithm uses the
  // far- and near-end signals to find the offset between them.  This one
  // (called "signal delay") is then used to fine tune the alignment, or
  // simply compensate for errors in the system based one.
  // Note that the two algorithms operate independently.  Currently, we only
  // allow one algorithm to be turned on.

  RTC_DCHECK_EQ(aec->num_bands, num_bands);

  for (size_t j = 0; j < num_samples; j += FRAME_LEN) {
    // 1) At most we process |aec->mult|+1 partitions in 10 ms. Make sure we
    // have enough far-end data for that by stuffing the buffer if the
    // |system_delay| indicates others.
    if (aec->system_delay < FRAME_LEN) {
      // We don't have enough data so we rewind 10 ms.
      WebRtcAec_AdjustFarendBufferSizeAndSystemDelay(aec, -(aec->mult + 1));
    }

    if (!aec->delay_agnostic_enabled) {
      // 2 a) Compensate for a possible change in the system delay.

      // TODO(bjornv): Investigate how we should round the delay difference;
      // right now we know that incoming |knownDelay| is underestimated when
      // it's less than |aec->knownDelay|. We therefore, round (-32) in that
      // direction. In the other direction, we don't have this situation, but
      // might flush one partition too little. This can cause non-causality,
      // which should be investigated. Maybe, allow for a non-symmetric
      // rounding, like -16.
      int move_elements = (aec->knownDelay - knownDelay - 32) / PART_LEN;
      int moved_elements = aec->farend_block_buffer_.AdjustSize(move_elements);
      MaybeLogDelayAdjustment(moved_elements * (aec->sampFreq == 8000 ? 8 : 4),
                              DelaySource::kSystemDelay);
      aec->knownDelay -= moved_elements * PART_LEN;
    } else {
      // 2 b) Apply signal based delay correction.
      int move_elements = SignalBasedDelayCorrection(aec);
      int moved_elements = aec->farend_block_buffer_.AdjustSize(move_elements);
      MaybeLogDelayAdjustment(moved_elements * (aec->sampFreq == 8000 ? 8 : 4),
                              DelaySource::kDelayAgnostic);
      int far_near_buffer_diff =
          aec->farend_block_buffer_.Size() -
          (aec->nearend_buffer_size + FRAME_LEN) / PART_LEN;
      WebRtc_SoftResetDelayEstimator(aec->delay_estimator, moved_elements);
      WebRtc_SoftResetDelayEstimatorFarend(aec->delay_estimator_farend,
                                           moved_elements);
      // If we rely on reported system delay values only, a buffer underrun here
      // can never occur since we've taken care of that in 1) above.  Here, we
      // apply signal based delay correction and can therefore end up with
      // buffer underruns since the delay estimation can be wrong.  We therefore
      // stuff the buffer with enough elements if needed.
      if (far_near_buffer_diff < 0) {
        WebRtcAec_AdjustFarendBufferSizeAndSystemDelay(aec,
                                                       far_near_buffer_diff);
      }
    }

    static_assert(
        16 == (FRAME_LEN - PART_LEN),
        "These constants need to be properly related for this code to work");
    float output_block[NUM_HIGH_BANDS_MAX + 1][PART_LEN];
    float nearend_block[NUM_HIGH_BANDS_MAX + 1][PART_LEN];
    float farend_extended_block_lowest_band[PART_LEN2];

    // Form and process a block of nearend samples, buffer the output block of
    // samples.
    aec->farend_block_buffer_.ExtractExtendedBlock(
        farend_extended_block_lowest_band);
    FormNearendBlock(j, num_bands, nearend, PART_LEN - aec->nearend_buffer_size,
                     aec->nearend_buffer, nearend_block);
    ProcessNearendBlock(aec, farend_extended_block_lowest_band, nearend_block,
                        output_block);
    BufferOutputBlock(num_bands, output_block, &aec->output_buffer_size,
                      aec->output_buffer);

    if ((FRAME_LEN - PART_LEN + aec->nearend_buffer_size) == PART_LEN) {
      // When possible (every fourth frame) form and process a second block of
      // nearend samples, buffer the output block of samples.
      aec->farend_block_buffer_.ExtractExtendedBlock(
          farend_extended_block_lowest_band);
      FormNearendBlock(j + FRAME_LEN - PART_LEN, num_bands, nearend, PART_LEN,
                       aec->nearend_buffer, nearend_block);
      ProcessNearendBlock(aec, farend_extended_block_lowest_band, nearend_block,
                          output_block);
      BufferOutputBlock(num_bands, output_block, &aec->output_buffer_size,
                        aec->output_buffer);

      // Reset the buffer size as there are no samples left in the nearend input
      // to buffer.
      aec->nearend_buffer_size = 0;
    } else {
      // Buffer the remaining samples in the nearend input.
      aec->nearend_buffer_size += FRAME_LEN - PART_LEN;
      BufferNearendFrame(j, num_bands, nearend, aec->nearend_buffer_size,
                         aec->nearend_buffer);
    }

    // 5) Update system delay with respect to the entire frame.
    aec->system_delay -= FRAME_LEN;

    // 6) Form the output frame.
    FormOutputFrame(j, num_bands, &aec->output_buffer_size, aec->output_buffer,
                    out);
  }
}

int WebRtcAec_GetDelayMetricsCore(AecCore* self,
                                  int* median,
                                  int* std,
                                  float* fraction_poor_delays) {
  RTC_DCHECK(self);
  RTC_DCHECK(median);
  RTC_DCHECK(std);

  if (self->delay_logging_enabled == 0) {
    // Logging disabled.
    return -1;
  }

  if (self->delay_metrics_delivered == 0) {
    UpdateDelayMetrics(self);
    self->delay_metrics_delivered = 1;
  }
  *median = self->delay_median;
  *std = self->delay_std;
  *fraction_poor_delays = self->fraction_poor_delays;

  return 0;
}

int WebRtcAec_echo_state(AecCore* self) {
  return self->echoState;
}

void WebRtcAec_GetEchoStats(AecCore* self,
                            Stats* erl,
                            Stats* erle,
                            Stats* a_nlp,
                            float* divergent_filter_fraction) {
  RTC_DCHECK(erl);
  RTC_DCHECK(erle);
  RTC_DCHECK(a_nlp);
  *erl = self->erl;
  *erle = self->erle;
  *a_nlp = self->aNlp;
  *divergent_filter_fraction =
      self->divergent_filter_fraction.GetLatestFraction();
}

void WebRtcAec_SetConfigCore(AecCore* self,
                             int nlp_mode,
                             int metrics_mode,
                             int delay_logging) {
  RTC_DCHECK_GE(nlp_mode, 0);
  RTC_DCHECK_LT(nlp_mode, 3);
  self->nlp_mode = nlp_mode;
  self->metricsMode = metrics_mode;
  if (self->metricsMode) {
    InitMetrics(self);
  }
  // Turn on delay logging if it is either set explicitly or if delay agnostic
  // AEC is enabled (which requires delay estimates).
  self->delay_logging_enabled = delay_logging || self->delay_agnostic_enabled;
  if (self->delay_logging_enabled) {
    memset(self->delay_histogram, 0, sizeof(self->delay_histogram));
  }
}

void WebRtcAec_enable_delay_agnostic(AecCore* self, int enable) {
  self->delay_agnostic_enabled = enable;
}

int WebRtcAec_delay_agnostic_enabled(AecCore* self) {
  return self->delay_agnostic_enabled;
}

void WebRtcAec_enable_aec3(AecCore* self, int enable) {
  self->aec3_enabled = (enable != 0);
}

int WebRtcAec_aec3_enabled(AecCore* self) {
  RTC_DCHECK(self->aec3_enabled == 0 || self->aec3_enabled == 1);
  return self->aec3_enabled;
}

void WebRtcAec_enable_refined_adaptive_filter(AecCore* self, bool enable) {
  self->refined_adaptive_filter_enabled = enable;
  SetAdaptiveFilterStepSize(self);
  SetErrorThreshold(self);
}

bool WebRtcAec_refined_adaptive_filter_enabled(const AecCore* self) {
  return self->refined_adaptive_filter_enabled;
}

void WebRtcAec_enable_extended_filter(AecCore* self, int enable) {
  self->extended_filter_enabled = enable;
  SetAdaptiveFilterStepSize(self);
  SetErrorThreshold(self);
  self->num_partitions = enable ? kExtendedNumPartitions : kNormalNumPartitions;
  // Update the delay estimator with filter length.  See InitAEC() for details.
  WebRtc_set_allowed_offset(self->delay_estimator, self->num_partitions / 2);
}

int WebRtcAec_extended_filter_enabled(AecCore* self) {
  return self->extended_filter_enabled;
}

int WebRtcAec_system_delay(AecCore* self) {
  return self->system_delay;
}

void WebRtcAec_SetSystemDelay(AecCore* self, int delay) {
  RTC_DCHECK_GE(delay, 0);
  self->system_delay = delay;
}

#ifdef AEC_OPTIMIZE
static void FormSuppressionGain_New(AecCoreNew* aec,
	float cohde[PART_LEN1],
	float cohxd[PART_LEN1],
	float hNl[PART_LEN1]) {
	float hNlDeAvg, hNlXdAvg;
	float hNlPref[kPrefBandSize];
	float hNlFb = 0, hNlFbLow = 0;
	const int prefBandSize = kPrefBandSize / aec->mult;
	const float prefBandQuant = 0.75f, prefBandQuantLow = 0.5f;
	const int minPrefBand = 4 / aec->mult;
	// Power estimate smoothing coefficients.
	const float* min_overdrive = aec->extended_filter_enabled
		? kExtendedMinOverDrive
		: kNormalMinOverDrive;

	hNlXdAvg = 0;
	for (int i = minPrefBand; i < prefBandSize + minPrefBand; ++i) {
		hNlXdAvg += cohxd[i];
	}
	hNlXdAvg /= prefBandSize;
	hNlXdAvg = 1 - hNlXdAvg;

	hNlDeAvg = 0;
	for (int i = minPrefBand; i < prefBandSize + minPrefBand; ++i) {
		hNlDeAvg += cohde[i];
	}
	hNlDeAvg /= prefBandSize;

	if (hNlXdAvg < 0.75f && hNlXdAvg < aec->hNlXdAvgMin) {
		aec->hNlXdAvgMin = hNlXdAvg;
	}

	if (hNlDeAvg > 0.98f && hNlXdAvg > 0.9f) {
		aec->stNearState = 1;
	}
	else if (hNlDeAvg < 0.95f || hNlXdAvg < 0.8f) {
		aec->stNearState = 0;
	}

	if (aec->hNlXdAvgMin == 1) {
		aec->echoState = 0;
		aec->overDrive = min_overdrive[aec->nlp_mode];

		if (aec->stNearState == 1) {
			memcpy(hNl, cohde, sizeof(hNl[0]) * PART_LEN1);
			hNlFb = hNlDeAvg;
			hNlFbLow = hNlDeAvg;
		}
		else {
			for (int i = 0; i < PART_LEN1; ++i) {
				hNl[i] = 1 - cohxd[i];
			}
			hNlFb = hNlXdAvg;
			hNlFbLow = hNlXdAvg;
		}
	}
	else {
		if (aec->stNearState == 1) {
			aec->echoState = 0;
			memcpy(hNl, cohde, sizeof(hNl[0]) * PART_LEN1);
			hNlFb = hNlDeAvg;
			hNlFbLow = hNlDeAvg;
		}
		else {
			aec->echoState = 1;
			for (int i = 0; i < PART_LEN1; ++i) {
				hNl[i] = WEBRTC_SPL_MIN(cohde[i], 1 - cohxd[i]);
			}

			// Select an order statistic from the preferred bands.
			// TODO(peah): Using quicksort now, but a selection algorithm may be
			// preferred.
			memcpy(hNlPref, &hNl[minPrefBand], sizeof(float) * prefBandSize);
			qsort(hNlPref, prefBandSize, sizeof(float), CmpFloat);
			hNlFb = hNlPref[static_cast<int>(floor(prefBandQuant *
				(prefBandSize - 1)))];
			hNlFbLow = hNlPref[static_cast<int>(floor(prefBandQuantLow *
				(prefBandSize - 1)))];
		}
	}

	// Track the local filter minimum to determine suppression overdrive.
	if (hNlFbLow < 0.6f && hNlFbLow < aec->hNlFbLocalMin) {
		aec->hNlFbLocalMin = hNlFbLow;
		aec->hNlFbMin = hNlFbLow;
		aec->hNlNewMin = 1;
		aec->hNlMinCtr = 0;
	}
	aec->hNlFbLocalMin =
		WEBRTC_SPL_MIN(aec->hNlFbLocalMin + 0.0008f / aec->mult, 1);
	aec->hNlXdAvgMin = WEBRTC_SPL_MIN(aec->hNlXdAvgMin + 0.0006f / aec->mult, 1);

	if (aec->hNlNewMin == 1) {
		aec->hNlMinCtr++;
	}
	if (aec->hNlMinCtr == 2) {
		aec->hNlNewMin = 0;
		aec->hNlMinCtr = 0;
		aec->overDrive =
			WEBRTC_SPL_MAX(kTargetSupp[aec->nlp_mode] /
				static_cast<float>(log(aec->hNlFbMin + 1e-10f) + 1e-10f),
				min_overdrive[aec->nlp_mode]);
	}

	// Smooth the overdrive.
	if (aec->overDrive < aec->overdrive_scaling) {
		aec->overdrive_scaling =
			0.99f * aec->overdrive_scaling + 0.01f * aec->overDrive;
	}
	else {
		aec->overdrive_scaling =
			0.9f * aec->overdrive_scaling + 0.1f * aec->overDrive;
	}

	// Apply the overdrive.
	WebRtcAec_Overdrive(aec->overdrive_scaling, hNlFb, hNl);
}

static void EchoSuppression(AecCoreNew* aec,
	float* nearend_extended_block_lowest_band,
	float farend_extended_block[PART_LEN2],
	float* echo_subtractor_output,
	float output[NUM_HIGH_BANDS_MAX + 1][PART_LEN]) {
	float efw[2][PART_LEN1];
	float xfw[2][PART_LEN1];
	float dfw[2][PART_LEN1];
	float comfortNoiseHband[2][PART_LEN1];
	float fft[PART_LEN2];
	float nlpGainHband;
	int i;
	size_t j;

	// Coherence and non-linear filter
	float cohde[PART_LEN1], cohxd[PART_LEN1];
	float hNl[PART_LEN1];

	// Filter energy
	const int delayEstInterval = 10 * aec->mult;

	float* xfw_ptr = NULL;

	// Update eBuf with echo subtractor output.
	memcpy(aec->eBuf + PART_LEN, echo_subtractor_output,
		sizeof(float) * PART_LEN);

	// Analysis filter banks for the echo suppressor.
	// Windowed near-end ffts.
	WindowData(fft, nearend_extended_block_lowest_band);
	aec_rdft_forward_128(fft);
	StoreAsComplex(fft, dfw);

	// Windowed echo suppressor output ffts.
	WindowData(fft, aec->eBuf);
	aec_rdft_forward_128(fft);
	StoreAsComplex(fft, efw);

	// NLP

	// Convert far-end partition to the frequency domain with windowing.
	WindowData(fft, farend_extended_block);
	Fft(fft, xfw);
	xfw_ptr = &xfw[0][0];

	// Buffer far.
	memcpy(aec->xfwBuf, xfw_ptr, sizeof(float) * 2 * PART_LEN1);

	aec->delayEstCtr++;
	if (aec->delayEstCtr == delayEstInterval) {
		aec->delayEstCtr = 0;
		aec->delayIdx = WebRtcAec_PartitionDelay(aec->num_partitions, aec->wfBuf);
	}

	aec->data_dumper->DumpRaw("aec_nlp_delay", 1, &aec->delayIdx);

	// Use delayed far.
	memcpy(xfw, aec->xfwBuf + aec->delayIdx * PART_LEN1,
		sizeof(xfw[0][0]) * 2 * PART_LEN1);

	WebRtcAec_UpdateCoherenceSpectra(aec->mult, aec->extended_filter_enabled == 1,
		efw, dfw, xfw, &aec->coherence_state,
		&aec->divergeState,
		&aec->extreme_filter_divergence);

	WebRtcAec_ComputeCoherence(&aec->coherence_state, cohde, cohxd);

	// Select the microphone signal as output if the filter is deemed to have
	// diverged.
	if (aec->divergeState) {
		memcpy(efw, dfw, sizeof(efw[0][0]) * 2 * PART_LEN1);
	}

	FormSuppressionGain_New(aec, cohde, cohxd, hNl);

	aec->data_dumper->DumpRaw("aec_nlp_gain", PART_LEN1, hNl);
#if 1
	int index = 0;
	int count = 0;
	float temphNl[PART_LEN1] = { 0.0 };

	for (index = 0; index < PART_LEN1; index++)
	{
		temphNl[index] = hNl[index];
	}
	for (index = 16; index < PART_LEN1; index++)
	{
		if (hNl[index] < 0.5)
		{
			count++;
		}
	}

	if (count >= 16)
	{
		for (index = 16; index < PART_LEN1; index++)
		{
			if (temphNl[index] > 0.125)
			{
				temphNl[index] = 0.125;
			}
		}
	}

#if 1//add by wuzhaoyang20170428 start
	count = 0;
	for (index = 0; index < PART_LEN1; index++)
	{
		if (hNl[index] >= 0.25)
		{
			count++;
		}
	}

	if (count >= 1 && count < 5)
	{
		for (index = 0; index < PART_LEN1; index++)
		{
			if (temphNl[index] >= 0.25)
			{
				temphNl[index] = 0.0625;
			}
		}
	}

	if (aec->echoState == 1)
		//目前因为状态检测不太准，导致一些双讲情况出现双讲，待优化，优化思路可以为增加双讲检测，
		//只对纯回声状态进行，而且频带可以适当调高，例如从1.5khz处开始弄
	{
		for (index = 1; index < PART_LEN1; index++)
		{
			if (temphNl[index] > temphNl[index - 1])
			{
				temphNl[index] = temphNl[index - 1];
			}
		}
	}
#endif//add by wuzhaoyang20170428 end
#endif

	WebRtcAec_Suppress(temphNl, efw);//modify by wuzhaoyang20170426

#ifdef HOWL_DETECTED
	int jj = 0;
	float tempPow = 0.0;
	for (jj = 0; jj < PART_LEN1; jj++)
	{
		tempPow = efw[0][jj] * efw[0][jj] + efw[1][jj] * efw[1][jj];
		aec->SmoothHowlPow[jj] = 0.9 * aec->SmoothHowlPow[jj] + 0.1 * tempPow;
	}

	for (jj = 16; jj < PART_LEN - 1; jj++)
	{
		if (aec->SmoothHowlPow[jj] > 2 * aec->SmoothHowlPow[jj - 2] && aec->SmoothHowlPow[jj] > 2 * aec->SmoothHowlPow[jj + 2])
		{
			efw[0][jj] = 0.0;
			efw[1][jj] = 0.0;
		}
	}
#if 1//add by wuzhaoyang20170428 start
	tempPow = 0.0;
	int indexj = 0;
	for (index = 0; index < PART_LEN; index += 4)
	{
		tempPow = efw[0][index] * efw[0][index] + efw[1][index] * efw[1][index];
		tempPow += efw[0][index + 1] * efw[0][index + 1] + efw[1][index + 1] * efw[1][index + 1];
		tempPow += efw[0][index + 2] * efw[0][index + 2] + efw[1][index + 2] * efw[1][index + 2];
		tempPow += efw[0][index + 3] * efw[0][index + 3] + efw[1][index + 3] * efw[1][index + 3];
		aec->BandPow[indexj] = 0.9 * aec->BandPow[indexj] + 0.1 * tempPow;
		indexj++;
	}
	if (aec->echoState == 1)
	{
		for (index = 4; index < 16; index++)
		{
			if (aec->BandPow[index] > 2 * aec->BandPow[index - 1] && aec->BandPow[index] > 2 * aec->BandPow[index + 1])
			{
				efw[0][4 * index] = 0.0;
				efw[1][4 * index] = 0.0;
				efw[0][4 * index + 1] = 0.0;
				efw[1][4 * index + 1] = 0.0;
				efw[0][4 * index + 2] = 0.0;
				efw[1][4 * index + 2] = 0.0;
				efw[0][4 * index + 3] = 0.0;
				efw[1][4 * index + 3] = 0.0;
			}
		}
	}
#endif
#endif
	// Add comfort noise.
	ComfortNoise(aec->num_bands > 1, &aec->seed, efw, comfortNoiseHband, aec->noisePow, hNl);

	// Inverse error fft.
	ScaledInverseFft(efw, fft, 2.0f, 1);

	// Overlap and add to obtain output.
	for (i = 0; i < PART_LEN; i++) {
		output[0][i] = (fft[i] * WebRtcAec_sqrtHanning[i] +
			aec->outBuf[i] * WebRtcAec_sqrtHanning[PART_LEN - i]);

		// Saturate output to keep it in the allowed range.
		output[0][i] = WEBRTC_SPL_SAT(WEBRTC_SPL_WORD16_MAX, output[0][i],
			WEBRTC_SPL_WORD16_MIN);
	}
	memcpy(aec->outBuf, &fft[PART_LEN], PART_LEN * sizeof(aec->outBuf[0]));

	// For H band
	if (aec->num_bands > 1) {
		// H band gain
		// average nlp over low band: average over second half of freq spectrum
		// (4->8khz)
		GetHighbandGain(hNl, &nlpGainHband);

		// Inverse comfort_noise
		ScaledInverseFft(comfortNoiseHband, fft, 2.0f, 0);

		// compute gain factor
		for (j = 1; j < aec->num_bands; ++j) {
			for (i = 0; i < PART_LEN; i++) {
				output[j][i] = aec->previous_nearend_block[j][i] * nlpGainHband;
			}
		}

		// Add some comfort noise where Hband is attenuated.
		for (i = 0; i < PART_LEN; i++) {
			output[1][i] += cnScaleHband * fft[i];
		}

		// Saturate output to keep it in the allowed range.
		for (j = 1; j < aec->num_bands; ++j) {
			for (i = 0; i < PART_LEN; i++) {
				output[j][i] = WEBRTC_SPL_SAT(WEBRTC_SPL_WORD16_MAX, output[j][i],
					WEBRTC_SPL_WORD16_MIN);
			}
		}
	}

	// Copy the current block to the old position.
	memcpy(aec->eBuf, aec->eBuf + PART_LEN, sizeof(float) * PART_LEN);

	memmove(aec->xfwBuf + PART_LEN1, aec->xfwBuf,
		sizeof(aec->xfwBuf) - sizeof(complex_t) * PART_LEN1);
}

static void ProcessNearendBlock_New(
	AecCoreNew* aec,
	float farend_extended_block_lowest_band[PART_LEN2],
	float nearend_block[NUM_HIGH_BANDS_MAX + 1][PART_LEN],
	float output_block[NUM_HIGH_BANDS_MAX + 1][PART_LEN])
{
	size_t i;

	float fft[PART_LEN2];
	float nearend_extended_block_lowest_band[PART_LEN2];
	float farend_fft[2][PART_LEN1];
	float nearend_fft[2][PART_LEN1];
	float far_spectrum = 0.0f;
	float near_spectrum = 0.0f;
	float abs_far_spectrum[PART_LEN1];
	float abs_near_spectrum[PART_LEN1];

	const float gPow[2] = { 0.9f, 0.1f };

	// Noise estimate constants.
	const int noiseInitBlocks = 500 * aec->mult;
	const float step = 0.1f;
	const float ramp = 1.0002f;
	const float gInitNoise[2] = { 0.999f, 0.001f };

	float echo_subtractor_output[PART_LEN];

	aec->data_dumper->DumpWav("aec_far", PART_LEN,
		&farend_extended_block_lowest_band[PART_LEN],
		std::min(aec->sampFreq, 16000), 1);
	aec->data_dumper->DumpWav("aec_near", PART_LEN, &nearend_block[0][0],
		std::min(aec->sampFreq, 16000), 1);

	if (aec->metricsMode == 1)
	{
		// Update power levels
		UpdateLevel(&aec->farlevel, CalculatePower(&farend_extended_block_lowest_band[PART_LEN], PART_LEN));
		UpdateLevel(&aec->nearlevel, CalculatePower(&nearend_block[0][0], PART_LEN));
	}

	// Convert far-end signal to the frequency domain.
	memcpy(fft, farend_extended_block_lowest_band, sizeof(float) * PART_LEN2);
	Fft(fft, farend_fft);

	// Form extended nearend frame.
	memcpy(&nearend_extended_block_lowest_band[0], &aec->previous_nearend_block[0][0], sizeof(float) * PART_LEN);
	memcpy(&nearend_extended_block_lowest_band[PART_LEN], &nearend_block[0][0], sizeof(float) * PART_LEN);

	// Convert near-end signal to the frequency domain.
	memcpy(fft, nearend_extended_block_lowest_band, sizeof(float) * PART_LEN2);
	Fft(fft, nearend_fft);

	// Power smoothing.
	if (aec->refined_adaptive_filter_enabled)
	{
		for (i = 0; i < PART_LEN1; ++i)
		{
			far_spectrum = farend_fft[0][i] * farend_fft[0][i] + farend_fft[1][i] * farend_fft[1][i];
			// Calculate the magnitude spectrum.
			abs_far_spectrum[i] = sqrtf(far_spectrum);
		}
		RegressorPower(aec->num_partitions, aec->xfBufBlockPos, aec->xfBuf, aec->xPow);
	}
	else
	{
		for (i = 0; i < PART_LEN1; ++i)
		{
			far_spectrum = farend_fft[0][i] * farend_fft[0][i] + farend_fft[1][i] * farend_fft[1][i];
			aec->xPow[i] = gPow[0] * aec->xPow[i] + gPow[1] * aec->num_partitions * far_spectrum;
			// Calculate the magnitude spectrum.
			abs_far_spectrum[i] = sqrtf(far_spectrum);
		}
	}

	for (i = 0; i < PART_LEN1; ++i)
	{
		near_spectrum = nearend_fft[0][i] * nearend_fft[0][i] + nearend_fft[1][i] * nearend_fft[1][i];
		aec->dPow[i] = gPow[0] * aec->dPow[i] + gPow[1] * near_spectrum;
		// Calculate the magnitude spectrum.
		abs_near_spectrum[i] = sqrtf(near_spectrum);
	}

	// Estimate noise power. Wait until dPow is more stable.
	if (aec->noiseEstCtr > 50)
	{
		for (i = 0; i < PART_LEN1; i++)
		{
			if (aec->dPow[i] < aec->dMinPow[i])
			{
				aec->dMinPow[i] = (aec->dPow[i] + step * (aec->dMinPow[i] - aec->dPow[i])) * ramp;
			}
			else
			{
				aec->dMinPow[i] *= ramp;
			}
		}
	}

	// Smooth increasing noise power from zero at the start,
	// to avoid a sudden burst of comfort noise.
	if (aec->noiseEstCtr < noiseInitBlocks)
	{
		aec->noiseEstCtr++;
		for (i = 0; i < PART_LEN1; i++)
		{
			if (aec->dMinPow[i] > aec->dInitMinPow[i])
			{
				aec->dInitMinPow[i] = gInitNoise[0] * aec->dInitMinPow[i] + gInitNoise[1] * aec->dMinPow[i];
			}
			else
			{
				aec->dInitMinPow[i] = aec->dMinPow[i];
			}
		}
		aec->noisePow = aec->dInitMinPow;
	}
	else
	{
		aec->noisePow = aec->dMinPow;
	}

	// Block wise delay estimation used for logging
	if (aec->delay_logging_enabled)
	{
		if (WebRtc_AddFarSpectrumFloat(aec->delay_estimator_farend, abs_far_spectrum, PART_LEN1) == 0)
		{
			int delay_estimate = WebRtc_DelayEstimatorProcessFloat(aec->delay_estimator, abs_near_spectrum, PART_LEN1);

			if (delay_estimate >= 0)
			{
				// Update delay estimate buffer.
				aec->delay_histogram[delay_estimate]++;
				aec->num_delay_values++;
			}
			if (aec->delay_metrics_delivered == 1 && aec->num_delay_values >= kDelayMetricsAggregationWindow)
			{
				UpdateDelayMetrics_New(aec);
			}
		}
	}

	// Perform echo subtraction.
	EchoSubtraction(aec->num_partitions, aec->extended_filter_enabled,
		&aec->extreme_filter_divergence, aec->filter_step_size,
		aec->error_threshold, &farend_fft[0][0], &aec->xfBufBlockPos,
		aec->xfBuf, &nearend_block[0][0], aec->xPow, aec->wfBuf,
		echo_subtractor_output);
	aec->data_dumper->DumpRaw("aec_h_fft", PART_LEN1 * aec->num_partitions, &aec->wfBuf[0][0]);
	aec->data_dumper->DumpRaw("aec_h_fft", PART_LEN1 * aec->num_partitions, &aec->wfBuf[1][0]);
	aec->data_dumper->DumpWav("aec_out_linear", PART_LEN, echo_subtractor_output, std::min(aec->sampFreq, 16000), 1);

	if (aec->metricsMode == 1)
	{
		UpdateLevel(&aec->linoutlevel, CalculatePower(echo_subtractor_output, PART_LEN));
	}

	// Perform echo suppression.
	EchoSuppression(aec, nearend_extended_block_lowest_band,
		farend_extended_block_lowest_band, echo_subtractor_output,
		output_block);

	if (aec->metricsMode == 1)
	{
		UpdateLevel(&aec->nlpoutlevel, CalculatePower(&output_block[0][0], PART_LEN));
		UpdateMetrics_New(aec);
	}

	// Store the nearend signal until the next frame.
	for (i = 0; i < aec->num_bands; ++i)
	{
		memcpy(&aec->previous_nearend_block[i][0], &nearend_block[i][0], sizeof(float) * PART_LEN);
	}
	aec->data_dumper->DumpWav("aec_out", PART_LEN, &output_block[0][0], std::min(aec->sampFreq, 16000), 1);
}

AecCoreNew* WebRtcAec_CreateAec_New(int instance_count) {
	AecCoreNew* aec = new AecCoreNew(instance_count);

	if (!aec) {
		return NULL;
	}
	aec->nearend_buffer_size = 0;
	memset(&aec->nearend_buffer[0], 0, sizeof(aec->nearend_buffer));
	// Start the output buffer with zeros to be able to produce
	// a full output frame in the first frame.
	aec->output_buffer_size = PART_LEN - (FRAME_LEN_NEW - PART_LEN);
	memset(&aec->output_buffer[0], 0, sizeof(aec->output_buffer));

	aec->delay_estimator_farend =
		WebRtc_CreateDelayEstimatorFarend(PART_LEN1, kHistorySizeBlocks);
	if (aec->delay_estimator_farend == NULL) {
		WebRtcAec_FreeAec_New(aec);
		return NULL;
	}
	// We create the delay_estimator with the same amount of maximum lookahead as
	// the delay history size (kHistorySizeBlocks) for symmetry reasons.
	aec->delay_estimator = WebRtc_CreateDelayEstimator(
		aec->delay_estimator_farend, kHistorySizeBlocks);
	if (aec->delay_estimator == NULL) {
		WebRtcAec_FreeAec_New(aec);
		return NULL;
	}
#ifdef WEBRTC_ANDROID
	aec->delay_agnostic_enabled = 1;  // DA-AEC enabled by default.
									  // DA-AEC assumes the system is causal from the beginning and will self adjust
									  // the lookahead when shifting is required.
	WebRtc_set_lookahead(aec->delay_estimator, 0);
#else
	aec->delay_agnostic_enabled = 1;//modify by wuzhaoyang
	WebRtc_set_lookahead(aec->delay_estimator, kLookaheadBlocks);
#endif
	aec->extended_filter_enabled = 0;
	aec->aec3_enabled = 0;
	aec->refined_adaptive_filter_enabled = false;

	// Assembly optimization
	WebRtcAec_FilterFar = FilterFar;
	WebRtcAec_ScaleErrorSignal = ScaleErrorSignal;
	WebRtcAec_FilterAdaptation = FilterAdaptation;
	WebRtcAec_Overdrive = Overdrive;
	WebRtcAec_Suppress = Suppress;
	WebRtcAec_ComputeCoherence = ComputeCoherence;
	WebRtcAec_UpdateCoherenceSpectra = UpdateCoherenceSpectra;
	WebRtcAec_StoreAsComplex = StoreAsComplex;
	WebRtcAec_PartitionDelay = PartitionDelay;
	WebRtcAec_WindowData = WindowData;

#if defined(WEBRTC_ARCH_X86_FAMILY)
	if (WebRtc_GetCPUInfo(kSSE2)) {
		WebRtcAec_InitAec_SSE2();
	}
#endif

#if defined(MIPS_FPU_LE)
	WebRtcAec_InitAec_mips();
#endif

#if defined(WEBRTC_HAS_NEON)
	WebRtcAec_InitAec_neon();
#endif

	aec_rdft_init();

	return aec;
}

void WebRtcAec_FreeAec_New(AecCoreNew* aec) {
	if (aec == NULL) {
		return;
	}

	WebRtc_FreeDelayEstimator(aec->delay_estimator);
	WebRtc_FreeDelayEstimatorFarend(aec->delay_estimator_farend);

	delete aec;
}

static void SetAdaptiveFilterStepSize(AecCoreNew* aec) {
	// Extended filter adaptation parameter.
	// TODO(ajm): No narrowband tuning yet.
	const float kExtendedMu = 0.4f;

	if (aec->refined_adaptive_filter_enabled) {
		aec->filter_step_size = 0.05f;
	}
	else {
		if (aec->extended_filter_enabled) {
			aec->filter_step_size = kExtendedMu;
		}
		else {
			if (aec->sampFreq == 8000) {
				aec->filter_step_size = 0.6f;
			}
			else {
				aec->filter_step_size = 0.5f;
			}
		}
	}
}

static void SetErrorThreshold(AecCoreNew* aec) {
	// Extended filter adaptation parameter.
	// TODO(ajm): No narrowband tuning yet.
	static const float kExtendedErrorThreshold = 1.0e-6f;

	if (aec->extended_filter_enabled) {
		aec->error_threshold = kExtendedErrorThreshold;
	}
	else {
		if (aec->sampFreq == 8000) {
			aec->error_threshold = 2e-6f;
		}
		else {
			aec->error_threshold = 1.5e-6f;
		}
	}
}

int WebRtcAec_InitAec_New(AecCoreNew* aec, int sampFreq) {
	int i;
	aec->data_dumper->InitiateNewSetOfRecordings();

	aec->sampFreq = sampFreq;

	SetAdaptiveFilterStepSize(aec);
	SetErrorThreshold(aec);

	if (sampFreq == 8000) {
		aec->num_bands = 1;
	}
	else {
		aec->num_bands = (size_t)(sampFreq / 16000);
	}

	// Start the output buffer with zeros to be able to produce
	// a full output frame in the first frame.
	aec->output_buffer_size = PART_LEN - (FRAME_LEN_NEW - PART_LEN);
	memset(&aec->output_buffer[0], 0, sizeof(aec->output_buffer));
	aec->nearend_buffer_size = 0;
	memset(&aec->nearend_buffer[0], 0, sizeof(aec->nearend_buffer));

	// Initialize far-end buffer.
	aec->farend_block_buffer_.ReInit();

	aec->system_delay = 0;

	if (WebRtc_InitDelayEstimatorFarend(aec->delay_estimator_farend) != 0) {
		return -1;
	}
	if (WebRtc_InitDelayEstimator(aec->delay_estimator) != 0) {
		return -1;
	}
	aec->delay_logging_enabled = 0;
	aec->delay_metrics_delivered = 0;
	memset(aec->delay_histogram, 0, sizeof(aec->delay_histogram));
	aec->num_delay_values = 0;
	aec->delay_median = -1;
	aec->delay_std = -1;
	aec->fraction_poor_delays = -1.0f;

	aec->previous_delay = -2;  // (-2): Uninitialized.
	aec->delay_correction_count = 0;
	aec->shift_offset = kInitialShiftOffset;
	aec->delay_quality_threshold = kDelayQualityThresholdMin;

	aec->num_partitions = kNormalNumPartitions;

	// Update the delay estimator with filter length.  We use half the
	// |num_partitions| to take the echo path into account.  In practice we say
	// that the echo has a duration of maximum half |num_partitions|, which is not
	// true, but serves as a crude measure.
	WebRtc_set_allowed_offset(aec->delay_estimator, aec->num_partitions / 2);
	// TODO(bjornv): I currently hard coded the enable.  Once we've established
	// that AECM has no performance regression, robust_validation will be enabled
	// all the time and the APIs to turn it on/off will be removed.  Hence, remove
	// this line then.
	WebRtc_enable_robust_validation(aec->delay_estimator, 1);
	aec->frame_count = 0;

	// Default target suppression mode.
	aec->nlp_mode = 1;

	// Sampling frequency multiplier w.r.t. 8 kHz.
	// In case of multiple bands we process the lower band in 16 kHz, hence the
	// multiplier is always 2.
	if (aec->num_bands > 1) {
		aec->mult = 2;
	}
	else {
		aec->mult = static_cast<int16_t>(aec->sampFreq) / 8000;
	}

	aec->farBufWritePos = 0;
	aec->farBufReadPos = 0;

	aec->inSamples = 0;
	aec->outSamples = 0;
	aec->knownDelay = 0;

	// Initialize buffers
	memset(aec->previous_nearend_block, 0, sizeof(aec->previous_nearend_block));
	memset(aec->eBuf, 0, sizeof(aec->eBuf));

	memset(aec->xPow, 0, sizeof(aec->xPow));
	memset(aec->dPow, 0, sizeof(aec->dPow));
	memset(aec->dInitMinPow, 0, sizeof(aec->dInitMinPow));
	aec->noisePow = aec->dInitMinPow;
	aec->noiseEstCtr = 0;

	// Initial comfort noise power
	for (i = 0; i < PART_LEN1; i++) {
		aec->dMinPow[i] = 1.0e6f;
	}

	// Holds the last block written to
	aec->xfBufBlockPos = 0;
	// TODO(peah): Investigate need for these initializations. Deleting them
	// doesn't change the output at all and yields 0.4% overall speedup.
	memset(aec->xfBuf, 0, sizeof(complex_t) * kExtendedNumPartitions * PART_LEN1);
	memset(aec->wfBuf, 0, sizeof(complex_t) * kExtendedNumPartitions * PART_LEN1);
	memset(aec->coherence_state.sde, 0, sizeof(complex_t) * PART_LEN1);
	memset(aec->coherence_state.sxd, 0, sizeof(complex_t) * PART_LEN1);
	memset(aec->xfwBuf, 0,
		sizeof(complex_t) * kExtendedNumPartitions * PART_LEN1);
	memset(aec->coherence_state.se, 0, sizeof(float) * PART_LEN1);

	// To prevent numerical instability in the first block.
	for (i = 0; i < PART_LEN1; i++) {
		aec->coherence_state.sd[i] = 1;
	}
	for (i = 0; i < PART_LEN1; i++) {
		aec->coherence_state.sx[i] = 1;
	}

	memset(aec->hNs, 0, sizeof(aec->hNs));
	memset(aec->outBuf, 0, sizeof(float) * PART_LEN);
#ifdef HOWL_DETECTED
	memset(aec->SmoothHowlPow, 0, sizeof(aec->SmoothHowlPow));
	memset(aec->BandPow, 0, sizeof(aec->BandPow));
#endif
	aec->hNlFbMin = 1;
	aec->hNlFbLocalMin = 1;
	aec->hNlXdAvgMin = 1;
	aec->hNlNewMin = 0;
	aec->hNlMinCtr = 0;
	aec->overDrive = 2;
	aec->overdrive_scaling = 2;
	aec->delayIdx = 0;
	aec->stNearState = 0;
	aec->echoState = 0;
	aec->divergeState = 0;

	aec->seed = 777;
	aec->delayEstCtr = 0;

	aec->extreme_filter_divergence = 0;

	// Metrics disabled by default
	aec->metricsMode = 0;
	InitMetrics_New(aec);

	return 0;
}

void WebRtcAec_BufferFarendBlock_New(AecCoreNew* aec, const float* farend) {
	// Check if the buffer is full, and in that case flush the oldest data.
	if (aec->farend_block_buffer_.AvaliableSpace() < 1) {
		aec->farend_block_buffer_.AdjustSize(1);
	}
	aec->farend_block_buffer_.Insert(farend);
}

int WebRtcAec_AdjustFarendBufferSizeAndSystemDelay_New(AecCoreNew* aec,
	int buffer_size_decrease) {
	int achieved_buffer_size_decrease =
		aec->farend_block_buffer_.AdjustSize(buffer_size_decrease);
	aec->system_delay -= achieved_buffer_size_decrease * PART_LEN;
	return achieved_buffer_size_decrease;
}
void FormNearendBlock_New(
	size_t nearend_start_index,
	size_t num_bands,
	const float* const* nearend_frame,
	size_t num_samples_from_nearend_frame,
	const float nearend_buffer[NUM_HIGH_BANDS_MAX + 1]
	[PART_LEN - (FRAME_LEN_NEW - PART_LEN)],
	float nearend_block[NUM_HIGH_BANDS_MAX + 1][PART_LEN]) {
	RTC_DCHECK_LE(num_samples_from_nearend_frame, static_cast<size_t>(PART_LEN));
	const int num_samples_from_buffer = PART_LEN - num_samples_from_nearend_frame;

	if (num_samples_from_buffer > 0) {
		for (size_t i = 0; i < num_bands; ++i) {
			memcpy(&nearend_block[i][0], &nearend_buffer[i][0],
				num_samples_from_buffer * sizeof(float));
		}
	}

	for (size_t i = 0; i < num_bands; ++i) {
		memcpy(&nearend_block[i][num_samples_from_buffer],
			&nearend_frame[i][nearend_start_index],
			num_samples_from_nearend_frame * sizeof(float));
	}
}

void BufferNearendFrame_New(
	size_t nearend_start_index,
	size_t num_bands,
	const float* const* nearend_frame,
	size_t num_samples_to_buffer,
	float nearend_buffer[NUM_HIGH_BANDS_MAX + 1]
	[PART_LEN - (FRAME_LEN_NEW - PART_LEN)]) {
	for (size_t i = 0; i < num_bands; ++i) {
		memcpy(
			&nearend_buffer[i][0],
			&nearend_frame[i]
			[nearend_start_index + FRAME_LEN_NEW - num_samples_to_buffer],
			num_samples_to_buffer * sizeof(float));
	}
}

void BufferOutputBlock_New(size_t num_bands,
	const float output_block[NUM_HIGH_BANDS_MAX + 1]
	[PART_LEN],
	size_t* output_buffer_size,
	float output_buffer[NUM_HIGH_BANDS_MAX + 1]
	[2 * PART_LEN]) {
	for (size_t i = 0; i < num_bands; ++i) {
		memcpy(&output_buffer[i][*output_buffer_size], &output_block[i][0],
			PART_LEN * sizeof(float));
	}
	(*output_buffer_size) += PART_LEN;
}

void FormOutputFrame_New(size_t output_start_index,
	size_t num_bands,
	size_t* output_buffer_size,
	float output_buffer[NUM_HIGH_BANDS_MAX + 1][2 * PART_LEN],
	float* const* output_frame) {
	RTC_DCHECK_LE(static_cast<size_t>(FRAME_LEN_NEW), *output_buffer_size);
	for (size_t i = 0; i < num_bands; ++i) {
		memcpy(&output_frame[i][output_start_index], &output_buffer[i][0],
			FRAME_LEN_NEW * sizeof(float));
	}
	(*output_buffer_size) -= FRAME_LEN_NEW;
	if (*output_buffer_size > 0) {
		RTC_DCHECK_GE(static_cast<size_t>(2 * PART_LEN - FRAME_LEN_NEW),
			(*output_buffer_size));
		for (size_t i = 0; i < num_bands; ++i) {
			memcpy(&output_buffer[i][0], &output_buffer[i][FRAME_LEN_NEW],
				(*output_buffer_size) * sizeof(float));
		}
	}
}

void WebRtcAec_ProcessFrames_New(AecCoreNew* aec,
	const float* const* nearend,
	size_t num_bands,
	size_t num_samples,
	int knownDelay,
	float* const* out)
{
	//RTC_DCHECK(num_samples == 80 || num_samples == 160);

	aec->frame_count++;
	// For each frame the process is as follows:
	// 1) If the system_delay indicates on being too small for processing a
	//    frame we stuff the buffer with enough data for 10 ms.
	// 2 a) Adjust the buffer to the system delay, by moving the read pointer.
	//   b) Apply signal based delay correction, if we have detected poor AEC
	//    performance.
	// 3) TODO(bjornv): Investigate if we need to add this:
	//    If we can't move read pointer due to buffer size limitations we
	//    flush/stuff the buffer.
	// 4) Process as many partitions as possible.
	// 5) Update the |system_delay| with respect to a full frame of FRAME_LEN_NEW
	//    samples. Even though we will have data left to process (we work with
	//    partitions) we consider updating a whole frame, since that's the
	//    amount of data we input and output in audio_processing.
	// 6) Update the outputs.

	// The AEC has two different delay estimation algorithms built in.  The
	// first relies on delay input values from the user and the amount of
	// shifted buffer elements is controlled by |knownDelay|.  This delay will
	// give a guess on how much we need to shift far-end buffers to align with
	// the near-end signal.  The other delay estimation algorithm uses the
	// far- and near-end signals to find the offset between them.  This one
	// (called "signal delay") is then used to fine tune the alignment, or
	// simply compensate for errors in the system based one.
	// Note that the two algorithms operate independently.  Currently, we only
	// allow one algorithm to be turned on.

	RTC_DCHECK_EQ(aec->num_bands, num_bands);

	for (size_t j = 0; j < num_samples; j += FRAME_LEN_NEW)
	{
		// 1) At most we process |aec->mult|+1 partitions in 10 ms. Make sure we
		// have enough far-end data for that by stuffing the buffer if the
		// |system_delay| indicates others.
		if (aec->system_delay < FRAME_LEN_NEW)
		{
			// We don't have enough data so we rewind 10 ms.
			WebRtcAec_AdjustFarendBufferSizeAndSystemDelay_New(aec, -(aec->mult + 1));
		}

		if (!aec->delay_agnostic_enabled)
		{
			// 2 a) Compensate for a possible change in the system delay.

			// TODO(bjornv): Investigate how we should round the delay difference;
			// right now we know that incoming |knownDelay| is underestimated when
			// it's less than |aec->knownDelay|. We therefore, round (-32) in that
			// direction. In the other direction, we don't have this situation, but
			// might flush one partition too little. This can cause non-causality,
			// which should be investigated. Maybe, allow for a non-symmetric
			// rounding, like -16.
			int move_elements = (aec->knownDelay - knownDelay - 32) / PART_LEN;
			int moved_elements = aec->farend_block_buffer_.AdjustSize(move_elements);
			MaybeLogDelayAdjustmentNew(moved_elements * (aec->sampFreq == 8000 ? 8 : 4), DelaySource::kSystemDelay);
			aec->knownDelay -= moved_elements * PART_LEN;
		}
		else
		{
			// 2 b) Apply signal based delay correction.
			int move_elements = SignalBasedDelayCorrection_New(aec);
			int moved_elements = aec->farend_block_buffer_.AdjustSize(move_elements);
			MaybeLogDelayAdjustmentNew(moved_elements * (aec->sampFreq == 8000 ? 8 : 4), DelaySource::kDelayAgnostic);
			int far_near_buffer_diff = aec->farend_block_buffer_.Size() - (aec->nearend_buffer_size + FRAME_LEN_NEW) / PART_LEN;

			WebRtc_SoftResetDelayEstimator(aec->delay_estimator, moved_elements);
			WebRtc_SoftResetDelayEstimatorFarend(aec->delay_estimator_farend, moved_elements);
			// If we rely on reported system delay values only, a buffer underrun here
			// can never occur since we've taken care of that in 1) above.  Here, we
			// apply signal based delay correction and can therefore end up with
			// buffer underruns since the delay estimation can be wrong.  We therefore
			// stuff the buffer with enough elements if needed.
			if (far_near_buffer_diff < 0)
			{
				WebRtcAec_AdjustFarendBufferSizeAndSystemDelay_New(aec, far_near_buffer_diff);
			}
		}

		//static_assert(16 == (FRAME_LEN_NEW - PART_LEN), "These constants need to be properly related for this code to work");
		float output_block[NUM_HIGH_BANDS_MAX + 1][PART_LEN];
		float nearend_block[NUM_HIGH_BANDS_MAX + 1][PART_LEN];
		float farend_extended_block_lowest_band[PART_LEN2];

		// Form and process a block of nearend samples, buffer the output block of
		// samples.
		aec->farend_block_buffer_.ExtractExtendedBlock(farend_extended_block_lowest_band);
		FormNearendBlock_New(j, num_bands, nearend, PART_LEN - aec->nearend_buffer_size, aec->nearend_buffer, nearend_block);
		ProcessNearendBlock_New(aec, farend_extended_block_lowest_band, nearend_block, output_block);
		BufferOutputBlock_New(num_bands, output_block, &aec->output_buffer_size, aec->output_buffer);

		if ((FRAME_LEN_NEW - PART_LEN + aec->nearend_buffer_size) == PART_LEN)
		{
			// When possible (every fourth frame) form and process a second block of
			// nearend samples, buffer the output block of samples.
			aec->farend_block_buffer_.ExtractExtendedBlock(farend_extended_block_lowest_band);
			FormNearendBlock_New(j + FRAME_LEN_NEW - PART_LEN, num_bands, nearend, PART_LEN, aec->nearend_buffer, nearend_block);
			ProcessNearendBlock_New(aec, farend_extended_block_lowest_band, nearend_block, output_block);
			BufferOutputBlock_New(num_bands, output_block, &aec->output_buffer_size, aec->output_buffer);

			// Reset the buffer size as there are no samples left in the nearend input
			// to buffer.
			aec->nearend_buffer_size = 0;
		}
		else
		{
			// Buffer the remaining samples in the nearend input.
			aec->nearend_buffer_size += FRAME_LEN_NEW - PART_LEN;
			BufferNearendFrame_New(j, num_bands, nearend, aec->nearend_buffer_size, aec->nearend_buffer);
		}

		// 5) Update system delay with respect to the entire frame.
		aec->system_delay -= FRAME_LEN_NEW;

		// 6) Form the output frame.
		FormOutputFrame_New(j, num_bands, &aec->output_buffer_size, aec->output_buffer, out);
	}
}

int WebRtcAec_GetDelayMetricsCore_New(AecCoreNew* self,
	int* median,
	int* std,
	float* fraction_poor_delays) {
	RTC_DCHECK(self);
	RTC_DCHECK(median);
	RTC_DCHECK(std);

	if (self->delay_logging_enabled == 0) {
		// Logging disabled.
		return -1;
	}

	if (self->delay_metrics_delivered == 0) {
		UpdateDelayMetrics_New(self);
		self->delay_metrics_delivered = 1;
	}
	*median = self->delay_median;
	*std = self->delay_std;
	*fraction_poor_delays = self->fraction_poor_delays;

	return 0;
}

int WebRtcAec_echo_state_New(AecCoreNew* self) {
	return self->echoState;
}

void WebRtcAec_GetEchoStats_New(AecCoreNew* self,
	Stats* erl,
	Stats* erle,
	Stats* a_nlp,
	float* divergent_filter_fraction) {
	RTC_DCHECK(erl);
	RTC_DCHECK(erle);
	RTC_DCHECK(a_nlp);
	*erl = self->erl;
	*erle = self->erle;
	*a_nlp = self->aNlp;
	*divergent_filter_fraction =
		self->divergent_filter_fraction.GetLatestFraction();
}

void WebRtcAec_SetConfigCore_New(AecCoreNew* self,
	int nlp_mode,
	int metrics_mode,
	int delay_logging) {
	RTC_DCHECK_GE(nlp_mode, 0);
	RTC_DCHECK_LT(nlp_mode, 3);
	self->nlp_mode = nlp_mode;
	self->metricsMode = metrics_mode;
	if (self->metricsMode) {
		InitMetrics_New(self);
	}
	// Turn on delay logging if it is either set explicitly or if delay agnostic
	// AEC is enabled (which requires delay estimates).
	self->delay_logging_enabled = delay_logging || self->delay_agnostic_enabled;
	if (self->delay_logging_enabled) {
		memset(self->delay_histogram, 0, sizeof(self->delay_histogram));
	}
}

void WebRtcAec_enable_delay_agnostic_New(AecCoreNew* self, int enable) {
	self->delay_agnostic_enabled = enable;
}

int WebRtcAec_delay_agnostic_enabled_New(AecCoreNew* self) {
	return self->delay_agnostic_enabled;
}

void WebRtcAec_enable_aec3_New(AecCoreNew* self, int enable) {
	self->aec3_enabled = (enable != 0);
}

int WebRtcAec_aec3_enabled_New(AecCoreNew* self) {
	RTC_DCHECK(self->aec3_enabled == 0 || self->aec3_enabled == 1);
	return self->aec3_enabled;
}

void WebRtcAec_enable_refined_adaptive_filter_New(AecCoreNew* self, bool enable) {
	self->refined_adaptive_filter_enabled = enable;
	SetAdaptiveFilterStepSize(self);
	SetErrorThreshold(self);
}

bool WebRtcAec_refined_adaptive_filter_enabled_New(const AecCoreNew* self) {
	return self->refined_adaptive_filter_enabled;
}

void WebRtcAec_enable_extended_filter_New(AecCoreNew* self, int enable) {
	self->extended_filter_enabled = enable;
	SetAdaptiveFilterStepSize(self);
	SetErrorThreshold(self);
	self->num_partitions = enable ? kExtendedNumPartitions : kNormalNumPartitions;
	// Update the delay estimator with filter length.  See InitAEC() for details.
	WebRtc_set_allowed_offset(self->delay_estimator, self->num_partitions / 2);
}

int WebRtcAec_extended_filter_enabled_New(AecCoreNew* self) {
	return self->extended_filter_enabled;
}

int WebRtcAec_system_delay_New(AecCoreNew* self) {
	return self->system_delay;
}

void WebRtcAec_SetSystemDelay_New(AecCoreNew* self, int delay) {
	RTC_DCHECK_GE(delay, 0);
	self->system_delay = delay;
}
#endif


}  // namespace webrtc
