#include "stdafx.h"
#include "../Utilities/orfanidis_eq.h"
#include "../Utilities/stb_vorbis.h"
#include "SoundMixer.h"
#include "CPU.h"
#include "VideoRenderer.h"
#include "OggMixer.h"
#include "Console.h"
#include "BaseMapper.h"

SoundMixer::SoundMixer(shared_ptr<Console> console)
{
	_clockRate = 0;
	_console = console;
	_settings = _console->GetSettings();
	_eqFrequencyGrid.reset(new orfanidis_eq::freq_grid());
	_oggMixer.reset();
	_outputBuffer = new int16_t[SoundMixer::MaxSamplesPerFrame];
	_blipBufLeft = blip_new(SoundMixer::MaxSamplesPerFrame);
	_blipBufRight = blip_new(SoundMixer::MaxSamplesPerFrame);
	_sampleRate = _settings->GetSampleRate();
	_model = NesModel::NTSC;

	// Reserve enough space in the output audio buffer
	// for PAL content at a sample rate of x kHz
	_audioSampleBuffer.resize(SoundMixer::MaxSamplesPerFrame);
	_audioSampleBufferPos = 0;
}

SoundMixer::~SoundMixer()
{
	StopRecording();

	delete[] _outputBuffer;
	_outputBuffer = nullptr;

	blip_delete(_blipBufLeft);
	blip_delete(_blipBufRight);

	_audioSampleBuffer.clear();
	_audioSampleBufferPos = 0;
}

void SoundMixer::StreamState(bool saving)
{
	Stream(_clockRate, _sampleRate, _model);
	
	if(!saving) {
		if(_model == NesModel::Auto) {
			//Older savestates - assume NTSC
			_model = NesModel::NTSC;
		}

		Reset();
		UpdateRates(true);
	}

	ArrayInfo<int16_t> currentOutput = { _currentOutput, MaxChannelCount };
	Stream(_previousOutputLeft, currentOutput, _previousOutputRight);
}

void SoundMixer::Reset()
{
	if(_oggMixer) {
		_oggMixer->Reset(_settings->GetSampleRate());
	}
	_fadeRatio = 1.0;
	_muteFrameCount = 0;

	_previousOutputLeft = 0;
	_previousOutputRight = 0;
	blip_clear(_blipBufLeft);
	blip_clear(_blipBufRight);

	_timestamps.clear();

	for(uint32_t i = 0; i < MaxChannelCount; i++) {
		_volumes[i] = 0;
		_panning[i] = 0;
	}
	memset(_channelOutput, 0, sizeof(_channelOutput));
	memset(_currentOutput, 0, sizeof(_currentOutput));

	UpdateRates(true);
	UpdateEqualizers(true);
	_previousTargetRate = _sampleRate;
}

void SoundMixer::PlayAudioBuffer(uint32_t time)
{
	UpdateTargetSampleRate();
	EndFrame(time);

	size_t sampleCount = blip_read_samples(_blipBufLeft, _outputBuffer, SoundMixer::MaxSamplesPerFrame, 1);
	ApplyEqualizer(_equalizerLeft.get(), sampleCount);

	if(_hasPanning) {
		blip_read_samples(_blipBufRight, _outputBuffer + 1, SoundMixer::MaxSamplesPerFrame, 1);
		ApplyEqualizer(_equalizerRight.get(), sampleCount);
	} else {
		//Copy left channel to right channel (optimization - when no panning is used)
		for(size_t i = 0; i < sampleCount * 2; i += 2) {
			_outputBuffer[i + 1] = _outputBuffer[i];
		}
	}

	_console->GetMapper()->ApplySamples(_outputBuffer, sampleCount, _settings->GetMasterVolume());

	if(_oggMixer) {
		_oggMixer->ApplySamples(_outputBuffer, sampleCount, _settings->GetMasterVolume());
	}

	if(_console->IsDualSystem()) {
		if(_console->IsMaster() && _settings->CheckFlag(EmulationFlags::VsDualMuteMaster)) {
			_lowPassFilter.ApplyFilter(_outputBuffer, sampleCount, 0, 0);
		} else if(!_console->IsMaster() && _settings->CheckFlag(EmulationFlags::VsDualMuteSlave)) {
			_lowPassFilter.ApplyFilter(_outputBuffer, sampleCount, 0, 0);
		}
	}

	AudioFilterSettings filterSettings = _settings->GetAudioFilterSettings();

	if(filterSettings.ReverbStrength > 0) {
		_reverbFilter.ApplyFilter(_outputBuffer, sampleCount, _sampleRate, filterSettings.ReverbStrength, filterSettings.ReverbDelay);
	} else {
		_reverbFilter.ResetFilter();
	}

	switch(filterSettings.Filter) {
		case StereoFilter::None: break;
		case StereoFilter::Delay: _stereoDelay.ApplyFilter(_outputBuffer, sampleCount, _sampleRate, filterSettings.Delay); break;
		case StereoFilter::Panning: _stereoPanning.ApplyFilter(_outputBuffer, sampleCount, filterSettings.Angle); break;
		case StereoFilter::CombFilter: _stereoCombFilter.ApplyFilter(_outputBuffer, sampleCount, _sampleRate, filterSettings.Delay, filterSettings.Strength); break;
	}

	if(filterSettings.CrossFadeRatio > 0) {
		_crossFeedFilter.ApplyFilter(_outputBuffer, sampleCount, filterSettings.CrossFadeRatio);
	}

	if(!_skipMode) {
		size_t sampleBufferSize = _audioSampleBuffer.size();
		if (sampleBufferSize - _audioSampleBufferPos < (sampleCount << 1)) {
			_audioSampleBuffer.resize((sampleBufferSize + (sampleCount << 1)) * 1.5);
		}
		for(size_t i = 0; i < (sampleCount << 1); i++) {
			_audioSampleBuffer[i + _audioSampleBufferPos] = _outputBuffer[i];
		}
		_audioSampleBufferPos += (sampleCount << 1);
	}

	if(_settings->NeedAudioSettingsUpdate()) {
		if(_settings->GetSampleRate() != _sampleRate) {
			//Update sample rate for next frame if setting changed
			_sampleRate = _settings->GetSampleRate();
			UpdateRates(true);
			UpdateEqualizers(true);
		} else {
			UpdateEqualizers(false);
			UpdateRates(false);
		}
	}
}

void SoundMixer::UploadAudioSamples()
{
	size_t sampleCount = _audioSampleBufferPos >> 1;
	if (!_sendAudioSample || !sampleCount) {
		return;
	}
	for(size_t total = 0; total < sampleCount; ) {
		total += _sendAudioSample(_audioSampleBuffer.data() + (total << 1), sampleCount - total);
	}
	_audioSampleBufferPos = 0;
}

void SoundMixer::SetNesModel(NesModel model)
{
	if(_model != model) {
		_model = model;
		UpdateRates(true);
	}
}

void SoundMixer::UpdateRates(bool forceUpdate)
{
	uint32_t newRate = _console->GetCpu()->GetClockRate(_model);

	if(_settings->CheckFlag(EmulationFlags::IntegerFpsMode)) {
		//Adjust sample rate when running at 60.0 fps instead of 60.1
		if(_model == NesModel::NTSC) {
			newRate = (uint32_t)(newRate * 60.0 / 60.0988118623484);
		} else {
			newRate = (uint32_t)(newRate * 50.0 / 50.00697796826829);
		}
	}

	double targetRate = _sampleRate;
	if(_clockRate != newRate || forceUpdate) {
		_clockRate = newRate;
		blip_set_rates(_blipBufLeft, _clockRate, targetRate);
		blip_set_rates(_blipBufRight, _clockRate, targetRate);
		if(_oggMixer)
			_oggMixer->SetSampleRate(_sampleRate);
	}

	bool hasPanning = false;
	for(uint32_t i = 0; i < MaxChannelCount; i++) {
		_volumes[i] = _settings->GetChannelVolume((AudioChannel)i);
		_panning[i] = _settings->GetChannelPanning((AudioChannel)i);
		if(_panning[i] != 1.0) {
			if(!_hasPanning) {
				blip_clear(_blipBufLeft);
				blip_clear(_blipBufRight);
			}
			hasPanning = true;
		}
	}
	_hasPanning = hasPanning;
}

double SoundMixer::GetChannelOutput(AudioChannel channel, bool forRightChannel)
{
	if(forRightChannel)
		return _currentOutput[(int)channel] * _volumes[(int)channel] * _panning[(int)channel];
	return _currentOutput[(int)channel] * _volumes[(int)channel] * (2.0 - _panning[(int)channel]);
}

int16_t SoundMixer::GetOutputVolume(bool forRightChannel)
{
	double squareOutput = GetChannelOutput(AudioChannel::Square1, forRightChannel) + GetChannelOutput(AudioChannel::Square2, forRightChannel);
	double tndOutput = 1.0966713261 * GetChannelOutput(AudioChannel::DMC, forRightChannel) + 2.7516713261 * GetChannelOutput(AudioChannel::Triangle, forRightChannel) + 1.8493587125 * GetChannelOutput(AudioChannel::Noise, forRightChannel);

	uint16_t squareVolume = (uint16_t)((95.88*5000.0) / (8128.0 / squareOutput + 100.0));
	uint16_t tndVolume = (uint16_t)((159.79*5000.0) / (22638.0 / tndOutput + 100.0));
	
	return (int16_t)(squareVolume + tndVolume +
		GetChannelOutput(AudioChannel::FDS, forRightChannel) * 20 +
		GetChannelOutput(AudioChannel::MMC5, forRightChannel) * 43 +
		GetChannelOutput(AudioChannel::Namco163, forRightChannel) * 20 +
		GetChannelOutput(AudioChannel::Sunsoft5B, forRightChannel) * 15 +
		GetChannelOutput(AudioChannel::VRC6, forRightChannel) * 48 +
		GetChannelOutput(AudioChannel::VRC7, forRightChannel));
}

void SoundMixer::AddDelta(AudioChannel channel, uint32_t time, int16_t delta)
{
	if(delta != 0) {
		_timestamps.push_back(time);
		_channelOutput[(int)channel][time] += delta;
	}
}

void SoundMixer::EndFrame(uint32_t time)
{
	double masterVolume = _settings->GetMasterVolume() * _fadeRatio;
	sort(_timestamps.begin(), _timestamps.end());
	_timestamps.erase(std::unique(_timestamps.begin(), _timestamps.end()), _timestamps.end());

	bool muteFrame = true;
	for(size_t i = 0, len = _timestamps.size(); i < len; i++) {
		uint32_t stamp = _timestamps[i];
		for(uint32_t j = 0; j < MaxChannelCount; j++) {
			if(_channelOutput[j][stamp] != 0) {
				//Assume any change in output means sound is playing, disregarding volume options
				//NSF tracks that mute the triangle channel by setting it to a high-frequency value will not be considered silent
				muteFrame = false;
			}
			_currentOutput[j] += _channelOutput[j][stamp];
		}

		int16_t currentOutput = GetOutputVolume(false);
		blip_add_delta(_blipBufLeft, stamp, (int)((currentOutput - _previousOutputLeft) * masterVolume));
		_previousOutputLeft = currentOutput;

		if(_hasPanning) {
			currentOutput = GetOutputVolume(true);
			blip_add_delta(_blipBufRight, stamp, (int)((currentOutput - _previousOutputRight) * masterVolume));
			_previousOutputRight = currentOutput;
		}
	}

	blip_end_frame(_blipBufLeft, time);
	if(_hasPanning) {
		blip_end_frame(_blipBufRight, time);
	}

	if(muteFrame) {
		_muteFrameCount++;
	} else {
		_muteFrameCount = 0;
	}

	//Reset everything
	_timestamps.clear();
	memset(_channelOutput, 0, sizeof(_channelOutput));
}

void SoundMixer::ApplyEqualizer(orfanidis_eq::eq1* equalizer, size_t sampleCount)
{
	if(equalizer) {
		int offset = equalizer == _equalizerRight.get() ? 1 : 0;
		for(size_t i = 0; i < sampleCount; i++) {
			double in = _outputBuffer[i * 2 + offset];
			double out;
			equalizer->sbs_process(&in, &out);
			_outputBuffer[i * 2 + offset] = (int16_t)std::max(std::min(out, 32767.0), -32768.0);
		}
	}
}

void SoundMixer::UpdateEqualizers(bool forceUpdate)
{
	EqualizerFilterType type = _settings->GetEqualizerFilterType();
	if(type != EqualizerFilterType::None) {
		vector<double> bands     = _settings->GetEqualizerBands();
		vector<double> bandGains = _settings->GetBandGains();

		if(bands.size() != _eqFrequencyGrid->get_number_of_bands()) {
			_equalizerLeft.reset();
			_equalizerRight.reset();
		}

		if((_equalizerLeft && (int)_equalizerLeft->get_eq_type() != (int)type) || !_equalizerLeft || forceUpdate) {
			bands.insert(bands.begin(), bands[0] - (bands[1] - bands[0]));
			bands.insert(bands.end(), bands[bands.size() - 1] + (bands[bands.size() - 1] - bands[bands.size() - 2]));
			_eqFrequencyGrid.reset(new orfanidis_eq::freq_grid());
			for(size_t i = 1; i < bands.size() - 1; i++) {
				_eqFrequencyGrid->add_band((bands[i] + bands[i - 1]) / 2, bands[i], (bands[i + 1] + bands[i]) / 2);
			}

			_equalizerLeft.reset(new orfanidis_eq::eq1(_eqFrequencyGrid.get(), (orfanidis_eq::filter_type)_settings->GetEqualizerFilterType()));
			_equalizerRight.reset(new orfanidis_eq::eq1(_eqFrequencyGrid.get(), (orfanidis_eq::filter_type)_settings->GetEqualizerFilterType()));
			_equalizerLeft->set_sample_rate(_sampleRate);
			_equalizerRight->set_sample_rate(_sampleRate);
		}

		for(unsigned int i = 0; i < _eqFrequencyGrid->get_number_of_bands(); i++) {
			_equalizerLeft->change_band_gain_db(i, bandGains[i]);
			_equalizerRight->change_band_gain_db(i, bandGains[i]);
		}
	} else {
		_equalizerLeft.reset();
		_equalizerRight.reset();
	}
}

void SoundMixer::StartRecording(string filepath)
{
}

void SoundMixer::StopRecording()
{
}

bool SoundMixer::IsRecording()
{
	return false;
}

void SoundMixer::SetFadeRatio(double fadeRatio)
{
	_fadeRatio = fadeRatio;
}

uint32_t SoundMixer::GetMuteFrameCount()
{
	return _muteFrameCount;
}

void SoundMixer::ResetMuteFrameCount()
{
	_muteFrameCount = 0;
}

OggMixer* SoundMixer::GetOggMixer()
{
	if(!_oggMixer) {
		_oggMixer.reset(new OggMixer());
		_oggMixer->Reset(_settings->GetSampleRate());
	}
	return _oggMixer.get();
}

void SoundMixer::UpdateTargetSampleRate()
{
	double targetRate = _sampleRate;
	if(targetRate != _previousTargetRate) {
		blip_set_rates(_blipBufLeft, _clockRate, targetRate);
		blip_set_rates(_blipBufRight, _clockRate, targetRate);
		_previousTargetRate = targetRate;
	}
}
