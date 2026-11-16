#include <audio/audio.h>
#include <cpu/guest_thread.h>
#include <kernel/heap.h>

#include <kernel/function.h>
#include <audio/driver/sdl3_driver.h>
#include <audio/xadpcm.h>

#include <iostream>
#include <fstream>
#include <unordered_set>
#include <atomic>
#include <os/logger.h>
#include <user/config.h>
#include <ui/game_window.h>
#include <patches/global.h>
#include <os/media.h>

// Memo: 
// Two way of using buffers: Static (sfx, voices - Buffer is filled once with complete audio, Pull just read the buffer) 
// Dynamic (Music... - Buffer starts empty or partially filled, game writes chunks continuously while playing, Pull read mWrittenSize)
// Research shows that sound effect use XADPCM, a format for Xbox that need to be decoded, while music likely use PCM directly.
// We need to use host memory because the classes size can grow dynamically, but still need to trick the game that pointer exist with guest memory.

#define SAFE_DELETE(p)       { if(p) { delete (p);     (p)=NULL; } }
#define FORMAT_TO_SIZE(format) (SDL_AUDIO_BITSIZE(format) >> 3)
#define DSE_FLAG_XADPCM                 (1 << 1)
#define DSoundBufferGetPCMBufferSize(EmuFlags, size) (EmuFlags & DSE_FLAG_XADPCM) > 0 ? DWORD((size / float(XBOX_ADPCM_SRCSIZE)) * XBOX_ADPCM_DSTSIZE) : size
#define DSoundBufferGetXboxBufferSize(EmuFlags, size) (EmuFlags & DSE_FLAG_XADPCM) > 0 ? DWORD((size / float(XBOX_ADPCM_DSTSIZE)) * XBOX_ADPCM_SRCSIZE) : size

SHTHR_SOUNDDEVICE* ppDS = nullptr;
static std::atomic<bool> g_audioShouldExit;

static float GetMasterVolume()
{
	if (Config::MuteOnFocusLost && !GameWindow::s_isFocused)
		return 0.0f;

	return Config::MasterVolume.Value;
}

static float SoftClip(float sample)
{
	constexpr float knee = 0.8f;
	constexpr float headroom = 1.0f - knee;
	const float magnitude = fabsf(sample);
	if (magnitude <= knee)
	{
		return sample;
	}
	return copysignf(knee + headroom * tanhf((magnitude - knee) / headroom), sample);
}

// IsndADX::CreateTask (sub_25CEA0)
// 2 TObjBGM (music, jingles)
// 2 TObjSERIF (voices)
// any other ADX is invoked by SFDs
static constexpr uint32_t g_TOBJBGM = 0x5502EC;
static constexpr uint32_t g_TOBJSERIF = 0x5502FC;
static constexpr uint32_t BGM_ADXT = 0x44;
static constexpr uint32_t BGM_NAME = 0x50;
static constexpr uint32_t BGM_NAME_SIZE = 0x20;
static constexpr uint32_t SERIF_ADXT = 0x3C;
static constexpr uint32_t ADXT_RNA = 0xC;
static constexpr uint32_t ADXT_NAME = 0xAC;
static constexpr uint32_t ADXT_NAME_MAX = 0x100;

static std::unordered_set<std::string> g_eventTracks; // Added incase a mod adds pauseable events/sfds
static std::mutex g_eventTracksMutex;

// the rna plays its stream through its mwSnd port, so the port buffer key is the guest key
static bool IsAdxtBuffer(uint32_t adxt, int32_t key)
{
	auto base = g_memory.base;
	if (!adxt)
	{
		return false;
	}
	const uint32_t rna = X86_LOAD_U32(adxt + ADXT_RNA);
	if (!rna)
	{
		return false;
	}
	const uint32_t port = X86_LOAD_U32(rna + RNA_PORT);
	return port && int32_t(X86_LOAD_U32(port + PORT_BUFFER)) == key;
}

// Get the type TObjBGM / TObjSERIF
static uint32_t FindIsndObject(uint32_t objects, uint32_t adxtOffset, int32_t key)
{
	auto base = g_memory.base;
	for (uint32_t i = 0; i < 2; ++i) // There are 2 TObjBGM and TObjSERIF
	{
		const uint32_t object = X86_LOAD_U32(objects + i * 4);
		if (object && IsAdxtBuffer(X86_LOAD_U32(object + adxtOffset), key))
		{
			return object;
		}
	}
	return 0;
}

static std::string GetAdxtFileName(uint32_t adxt)
{
	auto base = g_memory.base;
	const uint32_t name = X86_LOAD_U32(adxt + ADXT_NAME);
	if (!name) // SFDs invoke directly
	{
		return {};
	}
	auto* str = reinterpret_cast<const char*>(g_memory.Translate(name));
	return std::string(str, strnlen(str, ADXT_NAME_MAX));
}

// TPjsEventActionManager::Exec marks if something is started during an event
static bool IsEventTrack(const std::string& name)
{
	std::lock_guard<std::mutex> lock(g_eventTracksMutex);
	return g_eventTracks.contains(name);
}

// TObjBGM::Play
X86_FUNC_IMPL(__imp__sub_25CBB0);
X86_OVERRIDE_FUNC(sub_25CBB0)
{
	if (g_isStartingEventAudio)
	{
		const uint32_t bgm = ctx.ecx.u32;
		auto* name = reinterpret_cast<const char*>(base + bgm + BGM_NAME);
		std::lock_guard<std::mutex> lock(g_eventTracksMutex);
		g_eventTracks.emplace(name, strnlen(name, BGM_NAME_SIZE));
	}
	__imp__sub_25CBB0(ctx, base);
}

void AudioShutDown()
{
	g_audioShouldExit = true;
}

void Interface::Release()
{
	delete this;
}

class SHTHR_SOUNDDEVICE_Impl;

class SHTHR_SOUND3DBUFFER_Impl : public SHTHR_SOUND3DBUFFER
{
private:
	float mMaxDistance = 1000000000.0f;
	float mMinDistance = 1.0f;
	float mPosition[3] = { 0.f, 0.f, 0.f };
public:
	int SetMaxDistance(float flMaxDistance, int dwApply) override;
	int SetMinDistance(float flMinDistance, int dwApply) override;
	int GetPosition(float* pvPosition) override;
	int SetPosition(float x, float y, float z, int dwApply) override;
	int GetMaxDistance(float* pflMaxDistance) override;
	int GetMinDistance(float* pflMinDistance) override;
};

class SHTHR_SOUNDBUFFER_Impl : public SHTHR_SOUNDBUFFER
{
private:
	std::mutex mutex;
	SDL_AudioStream* mAudioStream = nullptr;
	std::vector<Uint8> mResampleBuffer;
	std::vector<Uint8> mOutputBuffer;

	SHTHR_SOUNDDEVICE_Impl* mParent;

	struct PendingWrite {
		int offset;
		int bytes;
		bool consumed;
	};

	PendingWrite mPendingWrite{};
	unsigned char* mBuffer;
	bool mExternalBuffer = false; // track if buffer is externally owned
	DWORD mX_BufferCacheSize;

	bool isStreaming = false;
	int mBufferWriteLoc = 0;
	int mBufferReadLoc = 0;
	int mBufferSize = 0;
	int mWrittenSize = 0;
	int mCaps = 0;
	bool mPlay = false;

	int mChannelCount = 0;
	int mFrequency = 0;
	int mOriginalFrequency = 0;
	SDL_AudioFormat mFormat = SDL_AUDIO_UNKNOWN;

	int mVolume = 0;
	float mGain = 1.0f;
	float mPan = 0.0f; // -1 left, 1 right

	DWORD mPlayFlags = 0;
	DWORD mG_flags;

	bool mLooping = false;
	int mLoopStartOffset = 0;
	int mLoopLength = 0;    // 0 = loop entire buffer
	int mBakedLoopPos = 0;

	int32_t keyBuffer;

	enum SourceFormat {
		SOURCE_FORMAT_PCM,
		SOURCE_FORMAT_ADPCM_XBOX,
		SOURCE_FORMAT_FLOAT
	};
	SourceFormat mSourceFormat;

	enum SourceType {
		SOURCE_TYPE_SFX,
		SOURCE_TYPE_MUSIC,
		SOURCE_TYPE_VOICES,
		SOURCE_TYPE_EVENTS,
		SOURCE_TYPE_SFD
	};
	SourceType mSourceType = SOURCE_TYPE_SFX;

public:
	SHTHR_SOUNDBUFFER_Impl(SHTHR_SOUNDDEVICE_Impl* parent, X_DSBUFFERDESC* desc);
	~SHTHR_SOUNDBUFFER_Impl();

	PullResult Pull(unsigned char* data, int32_t size);
	int GetStatus(uint32_t* pdwStatus) override;
	int GetCurrentPosition(int* pdwCurrentPlayCursor, int* pdwCurrentWriteCursor) override;
	int SetCurrentPosition(DWORD dwNewPosition) override;
	int Lock(int dwOffset, int dwBytes, void** ppvAudioPtr1, int* pdwAudioBytes1, void** ppvAudioPtr2, int* pdwAudioBytes2, int dwFlags) override;
	int Unlock(void* pvAudioPtr1, int dwAudioBytes1, void* pvAudioPtr2, int dwAudioBytes2) override;
	int Play(int dwReserved1, int dwPriority, uint32_t dwFlags) override;
	int Stop() override;
	int StopEx(int64_t rtTimeStamp, uint32_t dwFlags) override;
	int SetVolume(int lVolume) override;
	void SetPan(float pan) override;
	void CalcVolumeLinear() override;
	float GetUserVolume() const;
	void DetermineStreamType();

	int SetFrequency(int lFrequency) override;
	int SetBufferData(void* pvBufferData, int dwBufferBytes) override;
	int SetLoopRegion(int dwLoopStartOffset, int dwLoopLength) override;
	int GetCaps() const { return mCaps; }

	void SetFormatTag(xpointer<WAVEFORMATEX> format);
	void SetBufferKey(int32_t key);
	void RebuildStream(SDL_AudioFormat srcFormat, int srcChannels, int srcFreq,
		SDL_AudioFormat dstFormat, int dstChannels, int dstFreq);
	void StopSfxEarly();

	float Calculate3DVolume();
	int mSoundID = 0; // for debugging
};

class SHTHR_SOUNDDEVICE_Impl : public SHTHR_SOUNDDEVICE
{
private:
	static void SDLCALL MixCallbackStatic(void* userdata, SDL_AudioStream* stream, int additionalAmount, int totalAmount);
	void Mix(SDL_AudioStream* stream, int size);

	std::vector<float> mMixBuffer;
	std::vector<uint8_t> mSourceBuffer;

	float mListenerPosition[3] = { 0, 0, 0 };
	float mDistanceFactor = 1.0f;  // Default: (no scaling)
	float mRolloffFactor = 1.0f; // Default: (realistic)

public:
	std::mutex mutex;

	SDL_AudioStream* mDeviceStream = nullptr;
	SDL_AudioSpec mAudioSpec;
	std::unordered_set<SHTHR_SOUNDBUFFER_Impl*> mAudioQueues;

	Listener3D paramsListener;

	int32_t mChannelCount = 0;
	int32_t mFrequency = 0;
	SDL_AudioFormat mFormat = SDL_AUDIO_UNKNOWN;

	SHTHR_SOUNDDEVICE_Impl();
	~SHTHR_SOUNDDEVICE_Impl();
	int32_t CreateSoundBuffer(X_DSBUFFERDESC* ppDesc, SHTHR_SOUNDBUFFER** ppBuffer) override;
	bool ReleaseSoundBuffer(SHTHR_SOUNDBUFFER* pBuffer) override;
	int SetAllParameters(Listener3D* params) override;
	int SetRolloffFactor(float flRolloffFactor, DWORD dwApply) override;
	int SetDistanceFactor(float flDistanceFactor, DWORD dwApply) override;
	int SetPosition(float x, float y, float z, DWORD dwApply) override;
	float GetRolloffFactor() const { return mRolloffFactor; }
	float GetDistanceFactor() const { return mDistanceFactor; }
	float* GetListenerPosition() { return mListenerPosition; }
};

void SHTHR_SOUNDBUFFER_Impl::SetBufferKey(int32_t key)
{
	keyBuffer = key;
}

void SHTHR_SOUNDBUFFER_Impl::SetFormatTag(xpointer<WAVEFORMATEX> format)
{
	if (format.get() != nullptr)
	{
		WORD bitsPerSample = format->wBitsPerSample;
		switch (format->wFormatTag)
		{
		case WAVE_FORMAT_PCM:
			this->mSourceFormat = SOURCE_FORMAT_PCM;
			this->mG_flags |= DSE_FLAG_PCM;

			switch (bitsPerSample)
			{
			case 8:
				this->mFormat = SDL_AUDIO_U8;
				break;
			case 16:
			default:
				this->mFormat = SDL_AUDIO_S16LE;
				break;
			case 24:
			case 32:
				this->mFormat = SDL_AUDIO_S32LE;
				break;
			}
			mSourceType = SOURCE_TYPE_SFX;
			break;
		case WAVE_FORMAT_XBOX_ADPCM:
			this->mG_flags |= DSE_FLAG_XADPCM;
			this->mSourceFormat = SOURCE_FORMAT_ADPCM_XBOX;
			this->mFormat = SDL_AUDIO_S16LE;
			break;
		default:
			this->mG_flags |= DSE_FLAG_PCM_UNKNOWN;
			this->mFormat = SDL_AUDIO_S16LE;
			break;
		}
	}
}

int SHTHR_SOUND3DBUFFER_Impl::GetPosition(float* pvPosition)
{
	if (!pvPosition)
	{
		return DSERR_INVALIDPARAM;
	}

	pvPosition[0] = mPosition[0];
	pvPosition[1] = mPosition[1];
	pvPosition[2] = mPosition[2];
	return S_OK;
}

int SHTHR_SOUND3DBUFFER_Impl::SetMaxDistance(float flMaxDistance, int dwApply)
{
	mMaxDistance = flMaxDistance;
	return S_OK;
}

int SHTHR_SOUND3DBUFFER_Impl::SetMinDistance(float flMinDistance, int dwApply)
{
	mMinDistance = flMinDistance;
	return S_OK;
}

int SHTHR_SOUND3DBUFFER_Impl::SetPosition(float x, float y, float z, int dwApply)
{
	mPosition[0] = x;
	mPosition[1] = y;
	mPosition[2] = z;
	return S_OK;
}

int SHTHR_SOUND3DBUFFER_Impl::GetMaxDistance(float* pflMaxDistance)
{
	if (!pflMaxDistance)
	{
		return DSERR_INVALIDPARAM;
	}

	*pflMaxDistance = mMaxDistance;
	return S_OK;
}

int SHTHR_SOUND3DBUFFER_Impl::GetMinDistance(float* pflMinDistance)
{
	if (!pflMinDistance)
	{
		return DSERR_INVALIDPARAM;
	}

	*pflMinDistance = mMinDistance;
	return S_OK;
}

float SHTHR_SOUNDBUFFER_Impl::Calculate3DVolume()
{
	float vecPos[3];
	this->m3DBuffer->GetPosition(vecPos);

	float mMinDistance = 1.0f;
	this->m3DBuffer->GetMinDistance(&mMinDistance);

	float mMaxDistance = 1000000000.0f;
	this->m3DBuffer->GetMaxDistance(&mMaxDistance);

	float* listenerPos = mParent->GetListenerPosition();

	float posX = vecPos[0] - listenerPos[0];
	float posY = vecPos[1] - listenerPos[1];
	float posZ = vecPos[2] - listenerPos[2];

	float distance = sqrtf(posX * posX + posY * posY + posZ * posZ);

	//float distanceFactor = mParent->GetDistanceFactor();
	float rolloff = mParent->GetRolloffFactor();

	// DirectSound inverse distance model
	// float scaledDistance = distance * distanceFactor;
	float dist_clamped = std::max(mMinDistance, std::min(distance, mMaxDistance));
	float attenuation = mMinDistance / (mMinDistance + rolloff * (dist_clamped - mMinDistance));

	if (attenuation > 1.0f)
	{
		attenuation = 1.0f;
	}
	return attenuation;
}

SHTHR_SOUNDBUFFER_Impl::SHTHR_SOUNDBUFFER_Impl(SHTHR_SOUNDDEVICE_Impl* parent, X_DSBUFFERDESC* desc) : mParent(parent)
{
	SetFormatTag(desc->lpwfxFormat);
	mCaps = desc->dwFlags;
	mChannelCount = desc->lpwfxFormat->nChannels;
	mFrequency = desc->lpwfxFormat->nSamplesPerSec;
	mOriginalFrequency = mFrequency;

	mBufferSize = desc->dwBufferBytes;
	mBuffer = (unsigned char*)g_userHeap.Alloc(mBufferSize);

	// translator between what the game provides and what audio device expects. (ex: 22050 hs vs 41800)
	RebuildStream(mFormat, mChannelCount, mFrequency, mParent->mFormat, mParent->mChannelCount, mParent->mFrequency);
	if (mCaps & XTL_DSBCAPS_CTRL3D)
	{
		m3DBuffer = new SHTHR_SOUND3DBUFFER_Impl();
	}
}

std::unordered_map<int32_t, SHTHR_SOUNDBUFFER*> SoundBufferList;
// The game uses the soundBuffer list with multiple threads at once
static std::mutex SoundBufferListMutex;

SHTHR_SOUNDBUFFER_Impl::~SHTHR_SOUNDBUFFER_Impl()
{
	mPlay = false;
	if (mBuffer && !mExternalBuffer)
	{
		g_userHeap.Free(mBuffer);
		mBuffer = nullptr;
	}

	{
		std::lock_guard<std::mutex> lock(SoundBufferListMutex);
		SoundBufferList.erase(keyBuffer);
		auto guestMemory = g_memory.Translate(keyBuffer);
		if (guestMemory)
		{
			g_userHeap.Free(guestMemory);
		}
	}

	if (mAudioStream != nullptr)
	{
		SDL_DestroyAudioStream(mAudioStream);
		mAudioStream = nullptr;
	}

	if (m3DBuffer != nullptr)
	{
		delete m3DBuffer;
		m3DBuffer = nullptr;
	}
}

SHTHR_SOUNDBUFFER* GetCurrentSoundBuffer(const int32_t key)
{
	std::lock_guard<std::mutex> lock(SoundBufferListMutex);
	auto it = SoundBufferList.find(key);
	return it != SoundBufferList.end() ? it->second : nullptr;
}

static std::atomic<int> globalSoundID{ 0 };
int32_t SHTHR_SOUNDDEVICE_Impl::CreateSoundBuffer(X_DSBUFFERDESC* ppDesc, SHTHR_SOUNDBUFFER** ppBuffer)
{
	if (!mDeviceStream)
	{
		return E_FAIL;
	}

	if (!ppDesc || !ppBuffer)
	{
		return E_FAIL;
	}

	// Since the sound classes have size that can grow dynamically due to vector and other stuff, we need to use host memory here
	// otherwise guest memory will overwrite some region that the class will take after growing.
	// but even if we use host memory, the guest memory still need to hold the pointer to pass it through different sounds function
	// sadly we can't just pass a host pointer to guest just like that, that will create issues due to region difference and 64 vs 32 bits moment.
	// as a workaround, we can store the sound buffers in a map and give the key to the guest in a valid 32 bits address.

	SHTHR_SOUNDBUFFER_Impl* buffer = new SHTHR_SOUNDBUFFER_Impl(this, ppDesc);
	if (!buffer)
	{
		return -1;
	}

	//alloc memory for the guest so it can hold the key of the map for this sound buffer
	int32_t key = 0;
	{
		std::lock_guard<std::mutex> lock(SoundBufferListMutex);

		auto ptr = g_userHeap.Alloc(sizeof(int));
		if (!ptr)
		{
			delete buffer;
			return -1;
		}

		key = g_memory.MapVirtual(ptr);
		buffer->SetBufferKey(key);
		SoundBufferList[key] = buffer;
	}

	buffer->mSoundID = globalSoundID++;

	mutex.lock();
	mAudioQueues.insert(buffer);
	mutex.unlock();

	return key; //give the key to the guest so it won't hold directly the pointer of the host, avoiding memory overwrite.
}

int SHTHR_SOUNDBUFFER_Impl::SetBufferData(void* pvBufferData, int dwBufferBytes)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (dwBufferBytes == 0)
	{
		return 0;
	}

	// Free old internal buffer if we own it
	if (mBuffer && !mExternalBuffer)
	{
		g_userHeap.Free(mBuffer);
		mBuffer = nullptr;
	}

	int pcmSize = DSoundBufferGetPCMBufferSize(this->mG_flags, dwBufferBytes);
	if (pvBufferData == nullptr)
	{
		mBuffer = (unsigned char*)g_userHeap.Alloc(pcmSize);
		if (!mBuffer) {
			return -1; // Out of memory
		}
		memset(mBuffer, 0, dwBufferBytes);
		this->mG_flags ^= DSE_FLAG_BUFFER_EXTERNAL;
		mExternalBuffer = false;
	}
	else if (mSourceFormat == SOURCE_FORMAT_ADPCM_XBOX)
	{
		mBuffer = (unsigned char*)g_userHeap.Alloc(pcmSize);
		if (!mBuffer) {
			return -1;
		}

		// Decode ADPCM PCM (function returns decoded size)
		int32_t actualPcmSize = TXboxAdpcmDecoder_Decode_Memory(
			(uint8_t*)pvBufferData,
			dwBufferBytes,
			(uint8_t*)mBuffer,
			mChannelCount
		);

		if (actualPcmSize <= 0)
		{
			g_userHeap.Free(mBuffer);
			mBuffer = nullptr;
			return -1;
		}

		mBufferSize = actualPcmSize;
		mExternalBuffer = false;
		mG_flags &= ~DSE_FLAG_BUFFER_EXTERNAL;

	}
	else // if not use external buffer
	{
		this->mBuffer = (unsigned char*)pvBufferData;
		this->mExternalBuffer = true;
		this->mX_BufferCacheSize = dwBufferBytes;
		this->mG_flags |= DSE_FLAG_BUFFER_EXTERNAL;
	}

	// Update size and reset positions
	mBufferReadLoc = 0;
	mBufferWriteLoc = 0;
	mLoopStartOffset = 0;
	mLoopLength = 0;
	mWrittenSize = pcmSize;
	isStreaming = false;

	this->RebuildStream(mFormat, mChannelCount, mFrequency, mParent->mFormat, mParent->mChannelCount, mParent->mFrequency);
	return 0;
}

void SHTHR_SOUNDBUFFER_Impl::RebuildStream(
	SDL_AudioFormat srcFormat, int srcChannels, int srcFreq,
	SDL_AudioFormat dstFormat, int dstChannels, int dstFreq)
{

	if (mAudioStream)
	{
		SDL_DestroyAudioStream(mAudioStream);
		mAudioStream = nullptr;
	}

	const SDL_AudioSpec srcSpec{ srcFormat, srcChannels, srcFreq };
	const SDL_AudioSpec dstSpec{ dstFormat, dstChannels, dstFreq };
	mAudioStream = SDL_CreateAudioStream(&srcSpec, &dstSpec);

	if (!mAudioStream)
	{
		LOGFN_ERROR("Failed to create audio stream: {}", SDL_GetError());
	}
}

int SHTHR_SOUNDBUFFER_Impl::SetLoopRegion(int dwLoopStartOffset, int dwLoopLength)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (mSourceFormat == SOURCE_FORMAT_ADPCM_XBOX)
	{
		dwLoopStartOffset = DSoundBufferGetPCMBufferSize(mG_flags, dwLoopStartOffset);
		dwLoopLength = DSoundBufferGetPCMBufferSize(mG_flags, dwLoopLength);
	}

	if (dwLoopStartOffset < 0 || dwLoopStartOffset >= mBufferSize) {
		return -1;
	}

	if (dwLoopLength < 0 || dwLoopStartOffset + dwLoopLength > mBufferSize) {
		return -1;
	}

	this->mLoopStartOffset = dwLoopStartOffset;
	this->mLoopLength = dwLoopLength;
	return 0;
}

void SHTHR_SOUNDBUFFER_Impl::StopSfxEarly()
{
	if (!mLooping && mWrittenSize <= 0)
	{
		mPlay = false;
		if (mAudioStream)
		{
			SDL_ClearAudioStream(mAudioStream);
		}
	}
}

PullResult SHTHR_SOUNDBUFFER_Impl::Pull(unsigned char* data, int32_t size)
{
	std::lock_guard<std::mutex> lock(mutex);
	PullResult result;
	bool sfxEmpty = mWrittenSize <= 0 && !mLooping;
	if (!mPlay || !mBuffer || sfxEmpty || !mAudioStream)
	{
		StopSfxEarly();
		return result;
	}

	int srcBytesPerSample = SDL_AUDIO_BITSIZE(mFormat) / 8 * mChannelCount;
	int dstBytesPerSample = SDL_AUDIO_BITSIZE(mParent->mFormat) / 8 * mParent->mChannelCount;
	float ratio = (float)mParent->mFrequency / (float)mFrequency
		* (float)dstBytesPerSample / (float)srcBytesPerSample;
	int read_size = (int)(size / ratio);
	read_size -= read_size % srcBytesPerSample; // align to sample boundary

	read_size = mLooping
		? read_size
		: std::min(read_size, mWrittenSize);

	if (read_size <= 0)
	{
		StopSfxEarly();
		return result;
	}

	int actualRead = read_size;
	if (this->isStreaming == false && mLooping && mLoopLength > 0)
	{
		int loopEnd = mLoopStartOffset + mLoopLength;
		mResampleBuffer.resize(read_size);
		int srcPos = mBufferReadLoc;
		int distToEnd = loopEnd - srcPos;

		if (distToEnd <= 0)
		{
			srcPos = mLoopStartOffset;
			distToEnd = mLoopLength;
		}

		if (read_size <= distToEnd)
		{
			memcpy(mResampleBuffer.data(), &mBuffer[srcPos], read_size);
			mBufferReadLoc = srcPos + read_size;
		}
		else
		{
			int copied = 0;
			while (copied < read_size)
			{
				distToEnd = loopEnd - srcPos;
				if (distToEnd <= 0)
				{
					srcPos = mLoopStartOffset;
					distToEnd = mLoopLength;
				}

				int chunk = std::min(read_size - copied, distToEnd);
				memcpy(mResampleBuffer.data() + copied, &mBuffer[srcPos], chunk);
				copied += chunk;
				srcPos += chunk;
			}
			mBufferReadLoc = srcPos;
		}
		mWrittenSize = mLoopLength;
	}
	else
	{
		mResampleBuffer.resize(read_size);
		if (mBufferReadLoc + read_size >= mBufferSize)
		{
			int remaining = mBufferSize - mBufferReadLoc;
			memcpy(mResampleBuffer.data(), &mBuffer[mBufferReadLoc], remaining);
			mBufferReadLoc = (read_size - remaining) % mBufferSize;
			memcpy(mResampleBuffer.data() + remaining, &mBuffer[0], mBufferReadLoc);
		}
		else
		{
			memcpy(mResampleBuffer.data(), &mBuffer[mBufferReadLoc], read_size);
			mBufferReadLoc += read_size;
		}
		mWrittenSize -= read_size;
		if (mWrittenSize < 0)
		{
			mWrittenSize = 0;
		}

		if (this->isStreaming == false)
		{
			if (mLooping && mLoopLength == 0)
			{
				if (mBufferReadLoc >= mBufferSize)
				{
					mBufferReadLoc %= mBufferSize;
					mWrittenSize = mBufferSize;
				}
			}
		}
	}

	if (!mLooping && mWrittenSize <= 0)
	{
		mPlay = false;
		SDL_ClearAudioStream(mAudioStream);
	}

	// Feed source data into the stream
	SDL_PutAudioStreamData(mAudioStream, mResampleBuffer.data(), actualRead);

	// SDL_AudioStream maintains filter state between calls
	int available = SDL_GetAudioStreamAvailable(mAudioStream);
	int writesize = std::min(size, available);
	if (writesize <= 0 && this->isStreaming == false)
	{
		StopSfxEarly();
		return result;
	}

	mOutputBuffer.resize(writesize);
	SDL_GetAudioStreamData(mAudioStream, mOutputBuffer.data(), writesize);
	memcpy(data, mOutputBuffer.data(), writesize);
	result.bytesRead = writesize;

	if (mPan != 0.0f)
	{
		const float farGain = 1.0f - fabsf(mPan);
		auto* samples = reinterpret_cast<float*>(data);
		const int sampleCount = writesize / int(sizeof(float));
		for (int i = mPan > 0.0f ? 0 : 1; i < sampleCount; i += 2)
		{
			samples[i] *= farGain;
		}
	}

	float vol3D = (mCaps & XTL_DSBCAPS_CTRL3D)
		? Calculate3DVolume()
		: 1.0f;

	// apply user sliders here otherwise audio fades are full volume
	result.mixVolume = mGain * vol3D * GetUserVolume();
	return result;
}

static float g_musicAttenuation = 1.0f;
static std::chrono::steady_clock::time_point g_musicAttenuationTime = std::chrono::steady_clock::now();

static void UpdateMusicAttenuation()
{
	auto now = std::chrono::steady_clock::now();
	float deltaTime = std::chrono::duration<float>(now - g_musicAttenuationTime).count();
	g_musicAttenuationTime = now;

	if (!Config::MusicAttenuation || !os::media::IsSupported())
	{
		g_musicAttenuation = 1.0f;
		return;
	}

	float target = os::media::IsExternalMediaPlaying() ? 0.0f : 1.0f;
	float time = 1.0f - expf(2.5f * -deltaTime);
	g_musicAttenuation = std::lerp(g_musicAttenuation, target, time);
}


float SHTHR_SOUNDBUFFER_Impl::GetUserVolume() const
{
	if (!this->isStreaming)
	{
		return Config::EffectsVolume.Value;
	}

	switch (mSourceType)
	{
	case SOURCE_TYPE_MUSIC:
		return Config::MusicVolume.Value * g_musicAttenuation;
	case SOURCE_TYPE_EVENTS:
	case SOURCE_TYPE_SFD:
		return Config::CutsceneVolume.Value;
	default:
		return Config::VoicesVolume.Value;
	}
}

// TObjBGM = Music or Event
// TObjSERIF = Voice
// Anything else = SFD
void SHTHR_SOUNDBUFFER_Impl::DetermineStreamType()
{
	auto base = g_memory.base;
	const uint32_t bgm = FindIsndObject(g_TOBJBGM, BGM_ADXT, keyBuffer);
	if (bgm)
	{
		const std::string name = GetAdxtFileName(X86_LOAD_U32(bgm + BGM_ADXT));
		mSourceType = IsEventTrack(name) ? SOURCE_TYPE_EVENTS : SOURCE_TYPE_MUSIC;
	}
	else if (FindIsndObject(g_TOBJSERIF, SERIF_ADXT, keyBuffer))
	{
		mSourceType = SOURCE_TYPE_VOICES;
	}
	else if (this->isStreaming)
	{
		mSourceType = SOURCE_TYPE_SFD;
	}
}

int SHTHR_SOUNDBUFFER_Impl::GetStatus(uint32_t* pdwStatus)
{
	std::lock_guard<std::mutex> lock(mutex);
	int status = 0;

	if (mLooping)
	{
		status |= X_DSBSTATUS_LOOPING;
	}

	if (mPlay)
	{
		status |= X_DSBSTATUS_PLAYING;
	}

	if (mLooping)
	{
		status |= X_DSBSTATUS_LOOPING;
	}

	return status;
}

int SHTHR_SOUNDBUFFER_Impl::GetCurrentPosition(int* pdwCurrentPlayCursor, int* pdwCurrentWriteCursor)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (pdwCurrentPlayCursor)
	{
		*pdwCurrentPlayCursor = mBufferReadLoc;
	}

	if (pdwCurrentWriteCursor)
	{
		*pdwCurrentWriteCursor = mBufferWriteLoc;
	}

	return S_OK;
}

int SHTHR_SOUNDBUFFER_Impl::SetCurrentPosition(DWORD dwNewPosition)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (dwNewPosition >= mBufferSize)
	{
		return -1;
	}

	mBufferReadLoc = dwNewPosition;
	return S_OK;
}

int SHTHR_SOUNDBUFFER_Impl::Lock(int dwOffset, int dwBytes, void** ppvAudioPtr1, int* pdwAudioBytes1, void** ppvAudioPtr2, int* pdwAudioBytes2, int dwFlags)
{
	std::lock_guard<std::mutex> lock(mutex);
	isStreaming = true;
	if (dwFlags & DSBLOCK_ENTIREBUFFER)
	{
		*ppvAudioPtr1 = mBuffer;
		*pdwAudioBytes1 = mBufferSize;
		*ppvAudioPtr2 = NULL;
		*pdwAudioBytes2 = 0;
		mPendingWrite = { 0, mBufferSize };
	}
	else
	{
		int offset = dwFlags & DSBLOCK_FROMWRITECURSOR ? mBufferWriteLoc : dwOffset;
		int bytes = dwBytes;
		if (bytes > mBufferSize)
		{
			return DSERR_INVALIDPARAM;
		}

		if (offset + bytes > mBufferSize)
		{
			if (ppvAudioPtr1 && pdwAudioBytes1 && ppvAudioPtr2 && pdwAudioBytes2)
			{
				int remaining_size = mBufferSize - offset;
				*ppvAudioPtr1 = &mBuffer[offset];
				*pdwAudioBytes1 = remaining_size;
				*ppvAudioPtr2 = &mBuffer[0];
				*pdwAudioBytes2 = bytes - remaining_size;
			}
			else
			{
				return DSERR_INVALIDPARAM;
			}
		}
		else
		{
			if (ppvAudioPtr1 && pdwAudioBytes1)
			{
				*ppvAudioPtr1 = &mBuffer[offset];
				*pdwAudioBytes1 = bytes;
			}
			else
			{
				return DSERR_INVALIDPARAM;
			}
			if (ppvAudioPtr2 && pdwAudioBytes2)
			{
				*ppvAudioPtr2 = NULL;
				*pdwAudioBytes2 = 0;
			}
		}
		mPendingWrite = { offset, bytes };
	}
	return S_OK;
}

int SHTHR_SOUNDBUFFER_Impl::Unlock(void* pvAudioPtr1, int dwAudioBytes1, void* pvAudioPtr2, int dwAudioBytes2)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!mPendingWrite.consumed)
	{
		mBufferWriteLoc = (mPendingWrite.offset + mPendingWrite.bytes) % mBufferSize;
		mWrittenSize = std::min(mWrittenSize + mPendingWrite.bytes, mBufferSize);
		mPendingWrite.consumed = true;
	}
	return S_OK;
}

int SHTHR_SOUNDBUFFER_Impl::Play(int dwReserved1, int dwPriority, uint32_t dwFlags)
{
	std::lock_guard<std::mutex> lock(mutex);
	this->mPlay = true;
	this->mPlayFlags = dwFlags;
	DetermineStreamType();
	if ((dwFlags & X_DSBPLAY_FROMSTART)) {
		mBufferReadLoc = 0;
		if (mAudioStream)
		{
			SDL_ClearAudioStream(mAudioStream);
		}
		this->mPlayFlags &= ~X_DSBPLAY_FROMSTART;
	}
	if (dwFlags & X_DSBPLAY_LOOPING)
	{
		this->mLooping = true;
	}
	else
	{
		this->mLooping = false;
		mBufferReadLoc = 0;
		if (!isStreaming)
		{
			mWrittenSize = mBufferSize;
		}
	}
	return S_OK;
}

int SHTHR_SOUNDBUFFER_Impl::Stop()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (mAudioStream)
	{
		SDL_ClearAudioStream(mAudioStream);
	}
	mPlay = false;
	return S_OK;
}

int SHTHR_SOUNDBUFFER_Impl::StopEx(int64_t rtTimeStamp, uint32_t dwFlags)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (mAudioStream)
	{
		SDL_ClearAudioStream(mAudioStream);
	}
	mPlay = false;
	if (dwFlags > X_DSBSTOPEX_ALL)
	{
		return -1;
	}
	return S_OK;
}

int SHTHR_SOUNDBUFFER_Impl::SetVolume(int lVolume)
{
	std::lock_guard<std::mutex> lock(mutex);
	// adjust to DirectSound range
	if (lVolume < DSBVOLUME_MIN)
	{
		lVolume = DSBVOLUME_MIN;
	}

	if (lVolume > DSBVOLUME_MAX)
	{
		lVolume = DSBVOLUME_MAX;
	}

	mVolume = lVolume;
	CalcVolumeLinear();
	return S_OK;
}

void SHTHR_SOUNDBUFFER_Impl::SetPan(float pan)
{
	std::lock_guard<std::mutex> lock(mutex);
	mPan = std::clamp(pan, -1.0f, 1.0f);
}

void SHTHR_SOUNDBUFFER_Impl::CalcVolumeLinear()
{
	if (mVolume <= DSBVOLUME_MIN)
	{
		mGain = 0.0f;
		return;
	}

	mGain = powf(10.0f, mVolume / 2000.0f);
	const float volScale = this->GetCaps() & XTL_DSBCAPS_CTRL3D ? SFX_3DVOLUME_SCALE : SFX_VOLUME_SCALE;
	if (this->isStreaming == false)
	{
		mGain *= volScale;
	}

	mGain = std::clamp(mGain, 0.0f, 1.0f);
}

int SHTHR_SOUNDBUFFER_Impl::SetFrequency(int lFrequency)
{
	std::lock_guard<std::mutex> lock(mutex);
	// DirectSound restores the buffer's native rate originally
	if (lFrequency == 0)
	{
		lFrequency = mOriginalFrequency;
	}

	if (lFrequency < 100 || lFrequency > 200000)
	{
		return -1;
	}

	mFrequency = lFrequency;
	RebuildStream(mFormat, mChannelCount, mFrequency, mParent->mFormat, mParent->mChannelCount, mParent->mFrequency);
	mBufferReadLoc = 0;
	mWrittenSize = 0;
	return S_OK;
}

SHTHR_SOUNDDEVICE_Impl::~SHTHR_SOUNDDEVICE_Impl()
{
	if (mDeviceStream)
	{
		SDL_DestroyAudioStream(mDeviceStream);
		mDeviceStream = nullptr;
	}

	mutex.lock();
	mAudioQueues.clear();
	mutex.unlock();
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void SDLCALL SHTHR_SOUNDDEVICE_Impl::MixCallbackStatic(void* userdata, SDL_AudioStream* stream, int additionalAmount, int totalAmount)
{
	static_cast<SHTHR_SOUNDDEVICE_Impl*>(userdata)->Mix(stream, additionalAmount);
}

void SHTHR_SOUNDDEVICE_Impl::Mix(SDL_AudioStream* stream, int size)
{
	size -= size % SDL_AUDIO_FRAMESIZE(mAudioSpec);
	if (size <= 0 || g_audioShouldExit)
	{
		return;
	}

	UpdateMusicAttenuation();

	const int sampleCount = size / int(sizeof(float));
	mMixBuffer.assign(sampleCount, 0.0f);
	mSourceBuffer.resize(size);

	{
		std::lock_guard<std::mutex> lk(mutex);
		for (auto& queue : mAudioQueues)
		{
			auto result = queue->Pull(mSourceBuffer.data(), size);
			if (result.bytesRead <= 0)
			{
				continue;
			}

			const float gain = result.mixVolume * GetMasterVolume();
			const auto* samples = reinterpret_cast<const float*>(mSourceBuffer.data());
			const int count = result.bytesRead / int(sizeof(float));
			for (int i = 0; i < count; i++)
			{
				mMixBuffer[i] += samples[i] * gain;
			}
		}
	}

	for (auto& sample : mMixBuffer)
	{
		sample = SoftClip(sample);
	}
	SDL_PutAudioStreamData(stream, mMixBuffer.data(), size);
}

int SHTHR_SOUNDDEVICE_Impl::SetAllParameters(Listener3D* params)
{
	paramsListener = *params;
	return S_OK;
}

int SHTHR_SOUNDDEVICE_Impl::SetDistanceFactor(float flDistanceFactor, DWORD dwApply)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (flDistanceFactor < 0.0f)
	{
		flDistanceFactor = 0.0f;
	}

	if (flDistanceFactor > 10.0f)
	{
		flDistanceFactor = 10.0f;
	}

	mDistanceFactor = flDistanceFactor;
	return S_OK;
}

int SHTHR_SOUNDDEVICE_Impl::SetRolloffFactor(float flRolloffFactor, DWORD dwApply)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (flRolloffFactor < 0.0f)
	{
		flRolloffFactor = 0.0f;
	}

	if (flRolloffFactor > 10.0f)
	{
		flRolloffFactor = 10.0f;
	}

	mRolloffFactor = flRolloffFactor;
	return S_OK;
}

int SHTHR_SOUNDDEVICE_Impl::SetPosition(float x, float y, float z, DWORD dwApply)
{
	std::lock_guard<std::mutex> lock(mutex);
	mListenerPosition[0] = x;
	mListenerPosition[1] = y;
	mListenerPosition[2] = z;
	return S_OK;
}

SHTHR_SOUNDDEVICE_Impl::SHTHR_SOUNDDEVICE_Impl()
{

#ifdef _WIN32
	// Force wasapi on Windows.
	SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "wasapi", SDL_HINT_OVERRIDE);
#endif

	SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
	SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "Shadow Recompiled");
#ifdef __ANDROID__
	// The audio preset chosen in the launcher. Phones differ too much for one buffer size:
	// "Compatibility" trades latency for a larger buffer and avoids AAudio's low latency
	// path (which sounds wrong on some devices), "LowLatency" does the opposite.
	const char* sampleFrames = "512";
	const char* lowLatency = "1";

	switch (Config::AndroidAudioPreset.Value)
	{
	case EAndroidAudioPreset::Compatibility:
		sampleFrames = "1024";
		lowLatency = "0";
		break;

	case EAndroidAudioPreset::LowLatency:
		sampleFrames = "256";
		break;

	case EAndroidAudioPreset::Default:
		break;
	}

	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, sampleFrames);
	SDL_SetHint(SDL_HINT_ANDROID_LOW_LATENCY_AUDIO, lowLatency);
	LOGFN("Audio preset: {} sample frames, low latency {}", sampleFrames, lowLatency);
#else
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512"); // necessary for weapon SFX timing (SDL default is 1024)
#endif

	if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
	{
		LOGFN_ERROR("Failed to init audio subsystem: {}", SDL_GetError());
		return;
	}

	mAudioSpec.freq = XAUDIO_SAMPLES_HZ;
	mAudioSpec.format = SDL_AUDIO_F32;
	mAudioSpec.channels = 2;

	mChannelCount = mAudioSpec.channels;
	mFrequency = mAudioSpec.freq;
	mFormat = mAudioSpec.format;

	mDeviceStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &mAudioSpec, MixCallbackStatic, this);
	if (mDeviceStream)
	{
		SDL_ResumeAudioStreamDevice(mDeviceStream);
		LOGN_UTILITY("Initialized SDL3 Audio");
	}
}

bool SHTHR_SOUNDDEVICE_Impl::ReleaseSoundBuffer(SHTHR_SOUNDBUFFER* pBuffer)
{
	mutex.lock();
	mAudioQueues.erase((SHTHR_SOUNDBUFFER_Impl*)pBuffer);
	mutex.unlock();
	return true;
}

int32_t SHTHRSoundCreate(SHTHR_SOUNDDEVICE** ppDevice)
{
	if (!ppDevice)
	{
		return -1;
	}

	SHTHR_SOUNDDEVICE* device = new SHTHR_SOUNDDEVICE_Impl();
	if (!device)
	{
		return -1;
	}

	if (ppDS)
	{
		delete(ppDS);
	}

	auto ptr = g_userHeap.Alloc(sizeof(int));
	if (!ptr)
	{
		return -1;
	}

	auto key = g_memory.MapVirtual(ptr);
	ppDS = device;
	LOGN_UTILITY("Created sound device");
	return key;
}
