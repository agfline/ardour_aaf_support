/*
 * Copyright (C) 2012-2017 Robin Gareus <robin@gareus.org>
 * Copyright (C) 2012-2019 Paul Davis <paul@linuxaudiosystems.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "temporal/time.h"

#include "ardour/audioengine.h"
#include "ardour/audio_port.h"
#include "ardour/debug.h"
#include "ardour/io.h"
#include "ardour/session.h"
#include "ardour/transport_master.h"
#include "ardour/transport_master_manager.h"

#include "pbd/i18n.h"

using namespace std;
using namespace ARDOUR;
using namespace PBD;
using namespace Timecode;

/* really verbose timing debug */
//#define LTC_GEN_FRAMEDBUG
//#define LTC_GEN_TXDBUG

#ifndef MAX
#define MAX(a,b) ( (a) > (b) ? (a) : (b) )
#endif
#ifndef MIN
#define MIN(a,b) ( (a) < (b) ? (a) : (b) )
#endif

/* LTC signal should have a rise time of 25 us +/- 5 us.
 * yet with most sound-cards a square-wave of 1-2 sample
 * introduces ringing and small oscillations.
 * https://en.wikipedia.org/wiki/Gibbs_phenomenon
 * A low-pass filter in libltc can reduce this at
 * the cost of being slightly out of spec WRT to rise-time.
 *
 * This filter is adaptive so that fast vari-speed signals
 * will not be affected by it.
 */
#define LTC_RISE_TIME(speed) MIN (100, MAX(40, (4000000 / ((speed==0)?1:speed) / engine().sample_rate())))

#define TV_STANDARD(tcf) \
	(timecode_to_frames_per_second(tcf)==25.0 ? LTC_TV_625_50 : \
	 timecode_has_drop_frames(tcf)? LTC_TV_525_60 : LTC_TV_FILM_24)

void
Session::ltc_tx_initialize()
{
	assert (!ltc_encoder && !ltc_enc_buf);
	ltc_enc_tcformat = config.get_timecode_format();

	ltc_tx_parse_offset();
	DEBUG_TRACE (DEBUG::TXLTC, string_compose("LTC TX init sr: %1 fps: %2\n", nominal_sample_rate(), timecode_to_frames_per_second(ltc_enc_tcformat)));
	ltc_encoder = ltc_encoder_create(nominal_sample_rate(),
			timecode_to_frames_per_second(ltc_enc_tcformat),
			TV_STANDARD(ltc_enc_tcformat), 0);

	ltc_encoder_set_bufsize(ltc_encoder, nominal_sample_rate(), 23.0);
	ltc_encoder_set_filter(ltc_encoder, LTC_RISE_TIME(1.0));

	/* buffersize for 1 LTC sample: (1 + sample-rate / fps) bytes
	 * usually returned by ltc_encoder_get_buffersize(encoder)
	 *
	 * since the fps can change and A3's  min fps: 24000/1001 */
	ltc_enc_buf = (ltcsnd_sample_t*) calloc((nominal_sample_rate() / 23), sizeof(ltcsnd_sample_t));
	ltc_speed = 0;
	ltc_prev_cycle = -1;
	ltc_tx_reset();
	ltc_tx_resync_latency (true);
	Xrun.connect_same_thread (ltc_tx_connections, std::bind (&Session::ltc_tx_reset, this));
	LatencyUpdated.connect_same_thread (ltc_tx_connections, std::bind (&Session::ltc_tx_resync_latency, this, _1));
	restarting = false;
}

void
Session::ltc_tx_cleanup()
{
	DEBUG_TRACE (DEBUG::TXLTC, "cleanup\n");
	ltc_tx_connections.drop_connections ();
	free(ltc_enc_buf);
	ltc_enc_buf = NULL;
	ltc_encoder_free(ltc_encoder);
	ltc_encoder = NULL;
}

void
Session::ltc_tx_resync_latency (bool playback)
{
	if (deletion_in_progress() || !playback) {
		return;
	}
	std::shared_ptr<Port> ltcport = ltc_output_port();
	if (ltcport) {
		ltcport->get_connected_latency_range(ltc_out_latency, true);
		DEBUG_TRACE (DEBUG::TXLTC, string_compose ("resync latency: %1\n", ltc_out_latency.max));
	}
}

void
Session::ltc_tx_reset()
{
	DEBUG_TRACE (DEBUG::TXLTC, "reset\n");
	assert (ltc_encoder);
	ltc_enc_pos = -9999; // force re-start
	ltc_buf_len = 0;
	ltc_buf_off = 0;
	ltc_enc_byte = 0;
	ltc_enc_cnt = 0;

	ltc_encoder_reset(ltc_encoder);
}

void
Session::ltc_tx_parse_offset() {
	Timecode::Time offset_tc;
	Timecode::parse_timecode_format(config.get_timecode_generator_offset(), offset_tc);
	offset_tc.rate = timecode_frames_per_second();
	offset_tc.drop = timecode_drop_frames();
	timecode_to_sample(offset_tc, ltc_timecode_offset, false, false);
	ltc_timecode_negative_offset = !offset_tc.negative;
	ltc_prev_cycle = -1;
}

void
Session::ltc_tx_recalculate_position()
{
	SMPTETimecode enctc;
	Timecode::Time a3tc;
	ltc_encoder_get_timecode(ltc_encoder, &enctc);

	a3tc.hours   = enctc.hours;
	a3tc.minutes = enctc.mins;
	a3tc.seconds = enctc.secs;
	a3tc.frames  = enctc.frame;
	a3tc.rate = timecode_to_frames_per_second(ltc_enc_tcformat);
	a3tc.drop = timecode_has_drop_frames(ltc_enc_tcformat);

	Timecode::timecode_to_sample (a3tc, ltc_enc_pos, true, false,
		(double)sample_rate(),
		config.get_subframes_per_frame(),
		ltc_timecode_negative_offset, ltc_timecode_offset
		);
	restarting = false;
}

void
Session::send_ltc_for_cycle (samplepos_t start_sample, samplepos_t end_sample, pframes_t n_samples)
{
	assert (n_samples > 0);

	pframes_t txf = 0;
	std::shared_ptr<Port> ltcport = ltc_output_port();

	if (!ltcport) {
		assert (deletion_in_progress ());
		return;
	}

	Buffer& buf (ltcport->get_buffer (n_samples));
	buf.silence (n_samples);

	if (!ltc_encoder || !ltc_enc_buf) {
		return;
	}

	if (!TransportMasterManager::instance().current()) {
		return;
	}

	SyncSource sync_src = TransportMasterManager::instance().current()->type();

	if (engine().freewheeling() || !Config->get_send_ltc()
	    /* TODO
	     * decide which time-sources we can generated LTC from.
	     * Internal, JACK or sample-synced slaves should be fine.
	     * talk to oofus.
	     *
	     || (config.get_external_sync() && sync_src == LTC)
	     || (config.get_external_sync() && sync_src == MTC)
	    */
	    ||(config.get_external_sync() && sync_src == MIDIClock)
		) {
		return;
	}

	Sample* out = dynamic_cast<AudioBuffer*>(&buf)->data ();

	/* range from libltc (38..218) || - 128.0  -> (-90..90) */
	const float ltcvol = Config->get_ltc_output_volume()/(90.0); // pow(10, db/20.0)/(90.0);

	DEBUG_TRACE (DEBUG::TXLTC, string_compose("LTC TX %1 to %2 / %3 | lat: %4\n", start_sample, end_sample, n_samples, ltc_out_latency.max));

	/* all systems go. Now here's the plan:
	 *
	 *  1) check if fps has changed
	 *  2) check direction of encoding, calc speed, re-sample existing buffer
	 *  3) calculate sample and byte to send aligned to jack-period size
	 *  4) check if it's the sample/byte that is already in the queue
	 *  5) if (4) mismatch, re-calculate offset of LTC sample relative to period size
	 *  6) actual LTC audio output
	 *  6a) send remaining part of already queued sample; break on n_samples
	 *  6b) encode new LTC-sample byte
	 *  6c) goto 6a
	 *  7) done
	 */

	// (1) check fps
	TimecodeFormat cur_timecode = config.get_timecode_format();
	if (cur_timecode != ltc_enc_tcformat) {
		DEBUG_TRACE (DEBUG::TXLTC, string_compose("1: TC format mismatch - reinit sr: %1 fps: %2\n", nominal_sample_rate(), timecode_to_frames_per_second(cur_timecode)));
		if (ltc_encoder_reinit(ltc_encoder, nominal_sample_rate(),
					timecode_to_frames_per_second(cur_timecode),
					TV_STANDARD(cur_timecode), 0
					)) {
			PBD::error << _("LTC encoder: invalid framerate - LTC encoding is disabled for the remainder of this session.") << endmsg;
			ltc_tx_cleanup();
			return;
		}
		ltc_encoder_set_filter(ltc_encoder, LTC_RISE_TIME(ltc_speed));
		ltc_enc_tcformat = cur_timecode;
		ltc_tx_parse_offset();
		ltc_tx_reset();
	}

	/* LTC is max. 30 fps */
	if (timecode_to_frames_per_second(cur_timecode) > 30) {
		return;
	}

	// (2) speed & direction

	/* speed 0 aka transport stopped is interpreted as rolling forward.
	 * keep repeating current sample
	 */
#define SIGNUM(a) ( (a) < 0 ? -1 : 1)

	bool speed_changed = false;
	double new_ltc_speed = (end_sample - start_sample) / (double)n_samples;

	/* port latency compensation:
	 * The _generated timecode_ is offset by the port-latency,
	 * therefore the offset depends on the direction of transport.
	 *
	 * latency is compensated by adding it to the timecode to
	 * be generated. e.g. if the signal will reach the output in
	 * N samples time from now, generate the timecode for (now + N).
	 *
	 * sample-sync is achieved by further calculating the difference
	 * between the timecode and the session-transport and offsetting the
	 * buffer.
	 *
	 * The timecode is generated directly in the Session process callback
	 * using _transport_sample (which is the audible frame at the
	 * output).
	 */
	samplepos_t cycle_start_sample;

	if (new_ltc_speed < 0) {
		cycle_start_sample = (start_sample - ltc_out_latency.max);
	} else if (new_ltc_speed > 0) {
		cycle_start_sample = (start_sample + ltc_out_latency.max);
	} else {
		/* There is no need to compensate for latency when not rolling
		 * rather send the accurate NOW timecode
		 * (LTC encoder compenates latency by sending earlier timecode)
		 */
		cycle_start_sample = start_sample;
	}

	/* LTC TV standard offset */
	if (new_ltc_speed != 0) {
		/* ditto - send "NOW" if not rolling */
		cycle_start_sample -= ltc_frame_alignment(samples_per_timecode_frame(), TV_STANDARD(cur_timecode));
	}

	/* cycle-start may become negative due to latency compensation */
	if (cycle_start_sample < 0) { cycle_start_sample = 0; }

	if (nominal_sample_rate() != sample_rate()) {
		new_ltc_speed *= (double)nominal_sample_rate() / (double)sample_rate();
	}

	if (SIGNUM(new_ltc_speed) != SIGNUM (ltc_speed)) {
		DEBUG_TRACE (DEBUG::TXLTC, "transport changed direction\n");
		ltc_tx_reset();
	}

	if (ltc_speed != new_ltc_speed
			/* but only once if, current_speed changes to 0. In that case
			 * new_ltc_speed is > 0 because (end_sample - start_sample) == jack-period for no-roll
			 * but ltc_speed will still be 0
			 */
			//&& (current_speed != 0 || ltc_speed != current_speed)
			) {
		DEBUG_TRACE (DEBUG::TXLTC, string_compose("2: speed change from: %1 to %2\n", ltc_speed, new_ltc_speed));
		speed_changed = true;
		ltc_encoder_set_filter(ltc_encoder, LTC_RISE_TIME(new_ltc_speed));
	}

	if (end_sample == start_sample || fabs(new_ltc_speed) < 0.1) {
		DEBUG_TRACE (DEBUG::TXLTC, "transport is not rolling or speed < 0.1\n");
		/* keep repeating current sample
		 *
		 * an LTC generator must be able to continue generating LTC when Ardours transport is in stop
		 * some machines do odd things if LTC goes away:
		 * e.g. a tape based machine (video or audio), some think they have gone into park if LTC goes away,
		 * so unspool the tape from the playhead. That might be inconvenient.
		 * If LTC keeps arriving they remain in a stop position with the tape on the playhead.
		 */
		new_ltc_speed = 0;
		if (!Config->get_ltc_send_continuously()) {
			ltc_speed = new_ltc_speed;
			return;
		}
		if (start_sample != ltc_prev_cycle) {
			DEBUG_TRACE (DEBUG::TXLTC, string_compose("2: no-roll seek from %1 to %2 (%3)\n", ltc_prev_cycle, start_sample, cycle_start_sample));
			ltc_tx_reset();
		}
	}

	if (fabs(new_ltc_speed) > 10.0) {
		DEBUG_TRACE (DEBUG::TXLTC, "speed is out of bounds.\n");
		ltc_tx_reset();
		return;
	}

	if (ltc_speed == 0 && new_ltc_speed != 0) {
		DEBUG_TRACE (DEBUG::TXLTC, "transport started rolling - reset\n");
		ltc_tx_reset();
	}

	/* the timecode duration corresponding to the samples that are still
	 * in the buffer. Here, the speed of previous cycle is used to calculate
	 * the alignment at the beginning of this cycle later.
	 */
	double poff = (ltc_buf_len - ltc_buf_off) * ltc_speed;

	if (speed_changed && new_ltc_speed != 0) {
		/* we need to re-sample the existing buffer.
		 * "make space for the en-coder to catch up to the new speed"
		 *
		 * since the LTC signal is a rectangular waveform we can simply squeeze it
		 * by removing samples or duplicating samples /here and there/.
		 *
		 * There may be a more elegant way to do this, in fact one could
		 * simply re-render the buffer using ltc_encoder_encode_byte()
		 * but that'd require some timecode offset buffer magic,
		 * which is left for later..
		 */

		double oldbuflen = (double)(ltc_buf_len - ltc_buf_off);
		double newbuflen = (double)(ltc_buf_len - ltc_buf_off) * fabs(ltc_speed / new_ltc_speed);

		DEBUG_TRACE (DEBUG::TXLTC, string_compose("2: bufOld %1 bufNew %2 | diff %3\n",
					(ltc_buf_len - ltc_buf_off), newbuflen, newbuflen - oldbuflen
					));

		double bufrspdiff = rint(newbuflen - oldbuflen);

		if (abs(bufrspdiff) > newbuflen || abs(bufrspdiff) > oldbuflen) {
			DEBUG_TRACE (DEBUG::TXLTC, "resampling buffer would destroy information.\n");
			ltc_tx_reset();
			poff = 0;
		} else if (bufrspdiff != 0 && newbuflen > oldbuflen) {
			int incnt = 0;
			double samples_to_insert = ceil(newbuflen - oldbuflen);
			double avg_distance = newbuflen / samples_to_insert;
			DEBUG_TRACE (DEBUG::TXLTC, string_compose("2: resample buffer insert: %1\n", samples_to_insert));

			for (int rp = ltc_buf_off; rp < ltc_buf_len - 1; ++rp) {
				const int ro = rp - ltc_buf_off;
				if (ro < (incnt*avg_distance)) continue;
				const ltcsnd_sample_t v1 = ltc_enc_buf[rp];
				const ltcsnd_sample_t v2 = ltc_enc_buf[rp+1];
				if (v1 != v2 && ro < ((incnt+1)*avg_distance)) continue;
				memmove(&ltc_enc_buf[rp+1], &ltc_enc_buf[rp], ltc_buf_len-rp);
				incnt++;
				ltc_buf_len++;
			}
		} else if (bufrspdiff != 0 && newbuflen < oldbuflen) {
			double samples_to_remove = ceil(oldbuflen - newbuflen);
			DEBUG_TRACE (DEBUG::TXLTC, string_compose("2: resample buffer - remove: %1\n", samples_to_remove));
			if (oldbuflen <= samples_to_remove) {
				ltc_buf_off = ltc_buf_len= 0;
			} else {
				double avg_distance = newbuflen / samples_to_remove;
				int rmcnt = 0;
				for (int rp = ltc_buf_off; rp < ltc_buf_len - 1; ++rp) {
					const int ro = rp - ltc_buf_off;
					if (ro < (rmcnt*avg_distance)) continue;
					const ltcsnd_sample_t v1 = ltc_enc_buf[rp];
					const ltcsnd_sample_t v2 = ltc_enc_buf[rp+1];
					if (v1 != v2 && ro < ((rmcnt+1)*avg_distance)) continue;
					memmove(&ltc_enc_buf[rp], &ltc_enc_buf[rp+1], ltc_buf_len-rp-1);
					ltc_buf_len--;
					rmcnt++;
				}
			}
		}
	}

	ltc_prev_cycle = start_sample;
	ltc_speed = new_ltc_speed;
	DEBUG_TRACE (DEBUG::TXLTC, string_compose("2: transport speed %1.\n", ltc_speed));

	// (3) bit/sample alignment
	Timecode::Time tc_start;
	samplepos_t tc_sample_start;

	/* calc timecode frame from current position - round down to nearest timecode */
	Timecode::sample_to_timecode(cycle_start_sample, tc_start, true, false,
			timecode_frames_per_second(),
			timecode_drop_frames(),
			(double)sample_rate(),
			config.get_subframes_per_frame(),
			ltc_timecode_negative_offset, ltc_timecode_offset
			);

	/* convert timecode back to sample-position */
	Timecode::timecode_to_sample (tc_start, tc_sample_start, true, false,
		(double)sample_rate(),
		config.get_subframes_per_frame(),
		ltc_timecode_negative_offset, ltc_timecode_offset
		);

	/* difference between current sample and TC sample in samples */
	sampleoffset_t soff = cycle_start_sample - tc_sample_start;
	if (new_ltc_speed == 0) {
		soff = 0;
	}
	DEBUG_TRACE (DEBUG::TXLTC, string_compose("3: A3cycle: %1 = A3tc: %2 +off: %3\n",
				cycle_start_sample, tc_sample_start, soff));


	// (4) check if alignment matches
	const double fptcf = samples_per_timecode_frame();

	/* maximum difference of bit alignment in audio-samples.
	 *
	 * if transport and LTC generator differs more than this, the LTC
	 * generator will be re-initialized
	 *
	 * due to rounding error and variations in LTC-bit duration depending
	 * on the speed, it can be off by +- ltc_speed audio-samples.
	 * When the playback speed changes, it can actually reach +- 2 * ltc_speed
	 * in the cycle _after_ the speed changed. The average delta however is 0.
	 */
	double maxdiff;

	if (transport_master_is_external()) {
		maxdiff = transport_master()->resolution();
	} else {
		maxdiff = ceil(fabs(ltc_speed))*2.0;
		if (nominal_sample_rate() != sample_rate()) {
			maxdiff *= 3.0;
		}
		if (ltc_enc_tcformat == Timecode::timecode_23976 || ltc_enc_tcformat == Timecode::timecode_24976) {
			maxdiff *= 15.0;
		}
	}

	DEBUG_TRACE (DEBUG::TXLTC, string_compose("4: enc: %1 + %2 - %3 || buf-bytes: %4 enc-byte: %5\n",
				ltc_enc_pos, ltc_enc_cnt, poff, (ltc_buf_len - ltc_buf_off), poff, ltc_enc_byte));

	DEBUG_TRACE (DEBUG::TXLTC, string_compose("4: enc-pos: %1  | d: %2\n",
				ltc_enc_pos + ltc_enc_cnt - poff,
				rint(ltc_enc_pos + ltc_enc_cnt - poff) - cycle_start_sample
				));

	const samplecnt_t wrap24h = 86400. * sample_rate();
	if (ltc_enc_pos < 0
			|| (ltc_speed != 0 && fabs(fmod(ceil(ltc_enc_pos + ltc_enc_cnt - poff), wrap24h) - (cycle_start_sample % wrap24h)) > maxdiff)
			) {

		// (5) re-align
		ltc_tx_reset();

		/* set sample to encode */
		SMPTETimecode tc;
		tc.hours = tc_start.hours % 24;
		tc.mins = tc_start.minutes;
		tc.secs = tc_start.seconds;
		tc.frame = tc_start.frames;
		ltc_encoder_set_timecode(ltc_encoder, &tc);

		/* workaround for libltc recognizing 29.97 and 30000/1001 as drop-sample TC.
		 * In A3 30000/1001 or 30 fps can be drop-sample.
		 */
		LTCFrame ltcframe;
		ltc_encoder_get_frame(ltc_encoder, &ltcframe);
		ltcframe.dfbit = timecode_has_drop_frames(cur_timecode)?1:0;
		ltc_encoder_set_frame(ltc_encoder, &ltcframe);


		DEBUG_TRACE (DEBUG::TXLTC, string_compose("4: now: %1 trs: %2 toff %3\n", cycle_start_sample, tc_sample_start, soff));

		int32_t cyc_off;
		if (soff < 0 || soff >= fptcf) {
			/* session framerate change between (2) and now */
			ltc_tx_reset();
			return;
		}

		if (ltc_speed < 0 ) {
			/* calculate the byte that starts at or after the current position */
			ltc_enc_byte = floor((10.0 * soff) / (fptcf));
			ltc_enc_cnt = ltc_enc_byte * fptcf / 10.0;

			/* calculate difference between the current position and the byte to send */
			cyc_off = soff- ceil(ltc_enc_cnt);

		} else {
			/* calculate the byte that starts at or after the current position */
			ltc_enc_byte = ceil((10.0 * soff) / fptcf);
			ltc_enc_cnt = ltc_enc_byte * fptcf / 10.0;

			/* calculate difference between the current position and the byte to send */
			cyc_off = ceil(ltc_enc_cnt) - soff;

			if (ltc_enc_byte == 10) {
				ltc_enc_byte = 0;
				ltc_encoder_inc_timecode(ltc_encoder);
			}
		}

		DEBUG_TRACE (DEBUG::TXLTC, string_compose("5 restart encoder: soff %1 byte %2 cycoff %3\n",
					soff, ltc_enc_byte, cyc_off));

		if ( (ltc_speed < 0 && ltc_enc_byte !=9 ) || (ltc_speed >= 0 && ltc_enc_byte !=0 ) ) {
			restarting = true;
		}

		if (cyc_off >= 0 && cyc_off <= (int32_t) n_samples) {
			/* offset in this cycle */
			txf= rint(cyc_off / fabs(ltc_speed));
			memset (out, 0, cyc_off * sizeof(Sample));
		} else {
			/* resync next cycle */
			return;
		}

		ltc_enc_pos = tc_sample_start % wrap24h;

		DEBUG_TRACE (DEBUG::TXLTC, string_compose("5 restart @ %1 + %2 - %3 |  byte %4\n",
					ltc_enc_pos, ltc_enc_cnt, cyc_off, ltc_enc_byte));
	}
	else if (ltc_speed != 0 && (fptcf / ltc_speed / 80) > 3 ) {
		/* reduce (low freq) jitter.
		 * The granularity of the LTC encoder speed is 1 byte =
		 * (samples-per-timecode-sample / 10) audio-samples.
		 * Thus, tiny speed changes [as produced by some transport masters]
		 * may not have any effect in the cycle when they occur,
		 * but they will add up over time.
		 *
		 * This is a linear approx to compensate for this jitter
		 * and prempt re-sync when the drift builds up.
		 *
		 * However, for very fast speeds - when 1 LTC bit is
		 * <= 3 audio-sample - adjusting speed may lead to
		 * invalid samples.
		 *
		 * To do better than this, resampling (or a rewrite of the
		 * encoder) is required.
		 */
		ltc_speed -= fmod(((ltc_enc_pos + ltc_enc_cnt - poff) - cycle_start_sample), wrap24h) / engine().sample_rate();
	}


	// (6) encode and output
	while (1) {
#ifdef LTC_GEN_TXDBUG
		DEBUG_TRACE (DEBUG::TXLTC, string_compose("6.1 @%1  [ %2 / %3 ]\n", txf, ltc_buf_off, ltc_buf_len));
#endif
		// (6a) send remaining buffer
		while ((ltc_buf_off < ltc_buf_len) && (txf < n_samples)) {
			const float v1 = ltc_enc_buf[ltc_buf_off++] - 128.0;
			const Sample val = (Sample) (v1*ltcvol);
			out[txf++] = val;
		}
#ifdef LTC_GEN_TXDBUG
		DEBUG_TRACE (DEBUG::TXLTC, string_compose("6.2 @%1  [ %2 / %3 ]\n", txf, ltc_buf_off, ltc_buf_len));
#endif

		if (txf >= n_samples) {
			DEBUG_TRACE (DEBUG::TXLTC, string_compose("7 enc: %1 [ %2 / %3 ] byte: %4 spd %5 fpp %6 || nf: %7\n",
						ltc_enc_pos, ltc_buf_off, ltc_buf_len, ltc_enc_byte, ltc_speed, n_samples, txf));
			break;
		}

		ltc_buf_len = 0;
		ltc_buf_off = 0;

		// (6b) encode LTC, bump timecode

		if (ltc_speed < 0) {
			ltc_enc_byte = (ltc_enc_byte + 9)%10;
			if (ltc_enc_byte == 9) {
				ltc_encoder_dec_timecode(ltc_encoder);
				ltc_tx_recalculate_position();
				ltc_enc_cnt = fptcf;
			}
		}

		int enc_samples;

		if (restarting) {
			/* write zero bytes -- don't touch encoder until we're at a sample-boundary
			 * otherwise the biphase polarity may be inverted.
			 */
			enc_samples = fptcf / 10.0;
			memset(&ltc_enc_buf[ltc_buf_len], 127, enc_samples * sizeof(ltcsnd_sample_t));
		} else {
			if (ltc_encoder_encode_byte(ltc_encoder, ltc_enc_byte, (ltc_speed==0)?1.0:(1.0/ltc_speed))) {
				DEBUG_TRACE (DEBUG::TXLTC, string_compose("6.3 encoder error byte %1\n", ltc_enc_byte));
				ltc_encoder_buffer_flush(ltc_encoder);
				ltc_tx_reset();
				return;
			}
			enc_samples = ltc_encoder_get_buffer(ltc_encoder, &(ltc_enc_buf[ltc_buf_len]));
		}

#ifdef LTC_GEN_FRAMEDBUG
		DEBUG_TRACE (DEBUG::TXLTC, string_compose("6.3 encoded %1 bytes for LTC-byte %2 at spd %3\n", enc_samples, ltc_enc_byte, ltc_speed));
#endif
		if (enc_samples <=0) {
			DEBUG_TRACE (DEBUG::TXLTC, "6.3 encoder empty buffer.\n");
			ltc_encoder_buffer_flush(ltc_encoder);
			ltc_tx_reset();
			return;
		}

		ltc_buf_len += enc_samples;
		if (ltc_speed < 0)
			ltc_enc_cnt -= fptcf/10.0;
		else
			ltc_enc_cnt += fptcf/10.0;

		if (ltc_speed >= 0) {
			ltc_enc_byte = (ltc_enc_byte + 1)%10;
			if (ltc_enc_byte == 0 && ltc_speed != 0) {
				ltc_encoder_inc_timecode(ltc_encoder);
#if 0 /* force fixed parity -- scope debug */
				LTCFrame f;
				ltc_encoder_get_frame(ltc_encoder, &f);
				f.biphase_mark_phase_correction=0;
				ltc_encoder_set_frame(ltc_encoder, &f);
#endif
				ltc_tx_recalculate_position();
				ltc_enc_cnt = 0;
			} else if (ltc_enc_byte == 0) {
				ltc_enc_cnt = 0;
				restarting=false;
			}
		}
#ifdef LTC_GEN_FRAMEDBUG
		DEBUG_TRACE (DEBUG::TXLTC, string_compose("6.4 enc-pos: %1 + %2 [ %4 / %5 ] spd %6\n", ltc_enc_pos, ltc_enc_cnt, ltc_buf_off, ltc_buf_len, ltc_speed));
#endif
	}
}
