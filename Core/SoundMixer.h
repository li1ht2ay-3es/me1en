#pragma once
#include "stdafx.h"
#include "EmulationSettings.h"
#include "../Utilities/LowPassFilter.h"
#include "../Utilities/blip_buf.h"
#include "../Libretro/libretro.h"
#include "Snapshotable.h"
#include "StereoPanningFilter.h"
#include "StereoDelayFilter.h"
#include "StereoCombFilter.h"
#include "ReverbFilter.h"
#include "CrossFeedFilter.h"

class Console;
class OggMixer;

namespace orfanidis_eq {
	class freq_grid;
	class eq1;
}

class SoundMixer : public Snapshotable
{
public:
	static constexpr uint32_t CycleLength = 10000;
	static constexpr uint32_t BitsPerSample = 16;

private:
	static constexpr uint32_t MaxSampleRate = 1536000;
	static constexpr uint32_t MaxSamplesPerFrame = MaxSampleRate / 50 * 4 * 2; //x4 to allow CPU overclocking up to 10x, x2 for panning stereo
	static constexpr uint32_t MaxChannelCount = 11;

	retro_audio_sample_batch_t _sendAudioSample = nullptr;
	vector<int16_t> _audioSampleBuffer;
	size_t _audioSampleBufferPos = 0;

	bool _skipMode = false;
	EmulationSettings* _settings;
	double _fadeRatio;
	uint32_t _muteFrameCount;
	unique_ptr<OggMixer> _oggMixer;
	
	unique_ptr<orfanidis_eq::freq_grid> _eqFrequencyGrid;
	unique_ptr<orfanidis_eq::eq1> _equalizerLeft;
	unique_ptr<orfanidis_eq::eq1> _equalizerRight;
	shared_ptr<Console> _console;

	CrossFeedFilter _crossFeedFilter;
	LowPassFilter _lowPassFilter;
	StereoPanningFilter _stereoPanning;
	StereoDelayFilter _stereoDelay;
	StereoCombFilter _stereoCombFilter;
	ReverbFilter _reverbFilter;

	int16_t _previousOutputLeft = 0;
	int16_t _previousOutputRight = 0;

	vector<uint32_t> _timestamps;
	int16_t _channelOutput[MaxChannelCount][CycleLength];
	int16_t _currentOutput[MaxChannelCount];

	blip_t* _blipBufLeft;
	blip_t* _blipBufRight;
	int16_t *_outputBuffer;
	double _volumes[MaxChannelCount];
	double _panning[MaxChannelCount];

	NesModel _model;
	uint32_t _sampleRate;
	uint32_t _clockRate;

	bool _hasPanning;

	double _previousTargetRate;

	__forceinline double GetChannelOutput(AudioChannel channel, bool forRightChannel);
	__forceinline int16_t GetOutputVolume(bool forRightChannel);
	void EndFrame(uint32_t time);

	void UpdateRates(bool forceUpdate);
	
	void UpdateEqualizers(bool forceUpdate);
	void ApplyEqualizer(orfanidis_eq::eq1* equalizer, size_t sampleCount);
	
	void UpdateTargetSampleRate();

protected:
	virtual void StreamState(bool saving) override;

public:
	SoundMixer(shared_ptr<Console> console);
	~SoundMixer();

	void SetNesModel(NesModel model);
	void Reset();
	
	void PlayAudioBuffer(uint32_t cycle);
	void UploadAudioSamples();
	void AddDelta(AudioChannel channel, uint32_t time, int16_t delta);

	void StartRecording(string filepath);
	void StopRecording();
	bool IsRecording();

	//For NSF/NSFe
	uint32_t GetMuteFrameCount();
	void ResetMuteFrameCount();
	void SetFadeRatio(double fadeRatio);

	OggMixer* GetOggMixer();

	void SetSendAudioSample(retro_audio_sample_batch_t sendAudioSample)
	{
		_sendAudioSample = sendAudioSample;
	}

	void SetSkipMode(bool skip)
	{
		_skipMode = skip;
	}
};
