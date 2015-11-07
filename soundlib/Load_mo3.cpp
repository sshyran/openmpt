/*
 * Load_mo3.cpp
 * ------------
 * Purpose: MO3 module loader.
 * Notes  : This makes use of an external library (unmo3.dll / libunmo3.so).
 * Authors: Johannes Schultz
 *          Based on documentation and the decompression routines from the
 *          open-source UNMO3 project (https://github.com/lclevy/unmo3).
 *          The modified decompression code has been relicensed to the BSD
 *          license with permission from Laurent Clévy.
 * The OpenMPT source code is released under the BSD license. Read LICENSE for more details.
 */


#include "stdafx.h"
#include "Loaders.h"
#include "../common/ComponentManager.h"
#ifdef MPT_BUILTIN_MO3
#include "Tables.h"
#endif // MPT_BUILTIN_MO3

#ifndef NO_MO3
// unmo3.h
#if MPT_OS_WINDOWS
#define UNMO3_API __stdcall
#else
#define UNMO3_API
#endif
#ifdef MPT_LINKED_UNMO3
extern "C" {
OPENMPT_NAMESPACE::uint32 UNMO3_API UNMO3_GetVersion(void);
void UNMO3_API UNMO3_Free(const void *data);
OPENMPT_NAMESPACE::int32 UNMO3_API UNMO3_Decode(const void **data, OPENMPT_NAMESPACE::uint32 *len, OPENMPT_NAMESPACE::uint32 flags);
}
#endif // MPT_LINKED_UNMO3
#endif // !NO_MO3


OPENMPT_NAMESPACE_BEGIN


#ifndef NO_MO3

class ComponentUnMO3 : public ComponentLibrary
{
	MPT_DECLARE_COMPONENT_MEMBERS
public:
	uint32 (UNMO3_API * UNMO3_GetVersion)();
	// Decode a MO3 file (returns the same "exit codes" as UNMO3.EXE, eg. 0=success)
	// IN: data/len = MO3 data/len
	// OUT: data/len = decoded data/len (if successful)
	// flags & 1: Don't load samples
	int32 (UNMO3_API * UNMO3_Decode_Old)(const void **data, uint32 *len);
	int32 (UNMO3_API * UNMO3_Decode_New)(const void **data, uint32 *len, uint32 flags);
	// Free the data returned by UNMO3_Decode
	void (UNMO3_API * UNMO3_Free)(const void *data);
	int32 UNMO3_Decode(const void **data, uint32 *len, uint32 flags)
	{
		return (UNMO3_Decode_New ? UNMO3_Decode_New(data, len, flags) : UNMO3_Decode_Old(data, len));
	}
public:
	ComponentUnMO3() : ComponentLibrary(ComponentTypeForeign) { }
	bool DoInitialize()
	{
#ifdef MPT_LINKED_UNMO3
		UNMO3_GetVersion = &(::UNMO3_GetVersion);
		UNMO3_Free = &(::UNMO3_Free);
		UNMO3_Decode_Old = nullptr;
		UNMO3_Decode_New = &(::UNMO3_Decode);
		return true;
#else
		AddLibrary("unmo3", mpt::LibraryPath::App(MPT_PATHSTRING("unmo3")));
		MPT_COMPONENT_BIND("unmo3", UNMO3_Free);
		if(MPT_COMPONENT_BIND_OPTIONAL("unmo3", UNMO3_GetVersion))
		{
			UNMO3_Decode_Old = nullptr;
			MPT_COMPONENT_BIND_SYMBOL("unmo3", "UNMO3_Decode", UNMO3_Decode_New);
		} else
		{
			// Old API version: No "flags" parameter.
			UNMO3_Decode_New = nullptr;
			MPT_COMPONENT_BIND_SYMBOL("unmo3", "UNMO3_Decode", UNMO3_Decode_Old);
		}
		return !HasBindFailed();
#endif
	}
};
MPT_REGISTERED_COMPONENT(ComponentUnMO3, "UnMO3")

#endif // !NO_MO3


#ifdef MPT_BUILTIN_MO3

#ifdef NEEDS_PRAGMA_PACK
#pragma pack(push, 1)
#endif

struct PACKED MO3FileHeader
{
	enum MO3HeaderFlags
	{
		linearSlides	= 0x0001,
		isS3M			= 0x0002,
		s3mFastSlides	= 0x0004,
		isMTM			= 0x0008,	// Actually this is simply "not XM". But if none of the S3M, MOD and IT flags are set, it's an MTM.
		s3mAmigaLimits	= 0x0010,
		isMOD			= 0x0080,
		isIT			= 0x0100,
		instrumentMode	= 0x0200,
		itOldFX			= 0x0400,
		itCompatGxx		= 0x0800,
		modplugMode		= 0x10000,
		unknown			= 0x20000,	// Always set
		hasPlugins		= 0x100000,
		extFilterRange	= 0x200000,
	};

	uint8  numChannels;		// 0...64 (limited by channel panning and volume)
	uint16 numOrders;
	uint16 restartPos;
	uint16 numPatterns;
	uint16 numTracks;
	uint16 numInstruments;
	uint16 numSamples;
	uint8  defaultSpeed;
	uint8  defaultTempo;
	uint32 flags;			// See MO3HeaderFlags
	uint8  globalVol;		// 0...128 in IT, 0...64 in S3M
	uint8  panSeparation;	// 0...128 in IT
	int8   sampleVolume;	// Only used in IT
	uint8  chnVolume[64];	// 0...64
	uint8  chnPan[64];		// 0...256, 127 = surround
	uint8  sfxMacros[16];
	uint8  fixedMacros[128][2];

	// Convert all multi-byte numeric values to current platform's endianness or vice versa.
	void ConvertEndianness()
	{
		SwapBytesLE(numOrders);
		SwapBytesLE(restartPos);
		SwapBytesLE(numPatterns);
		SwapBytesLE(numTracks);
		SwapBytesLE(numInstruments);
		SwapBytesLE(numSamples);
		SwapBytesLE(flags);
	}
};

STATIC_ASSERT(sizeof(MO3FileHeader) == 422);


struct PACKED MO3Envelope
{
	enum MO3EnvelopeFlags
	{
		envEnabled	= 0x01,
		envSustain	= 0x02,
		envLoop		= 0x04,
		envFilter	= 0x10,
		envCarry	= 0x20,
	};

	uint8 flags;			// See MO3EnvelopeFlags
	uint8 numNodes;
	uint8 sustainStart;
	uint8 sustainEnd;
	uint8 loopStart;
	uint8 loopEnd;
	int16 points[25][2];

	// Convert all multi-byte numeric values to current platform's endianness or vice versa.
	void ConvertEndianness()
	{
		for(size_t i = 0; i < CountOf(points); i++)
		{
			SwapBytesLE(points[i][0]);
			SwapBytesLE(points[i][1]);
		}
	}

	// Convert MO3 envelope data into OpenMPT's internal envelope format
	void ConvertToMPT(InstrumentEnvelope &mptEnv, uint8 envShift) const
	{
		if(flags & envEnabled) mptEnv.dwFlags.set(ENV_ENABLED);
		if(flags & envSustain) mptEnv.dwFlags.set(ENV_SUSTAIN);
		if(flags & envLoop) mptEnv.dwFlags.set(ENV_LOOP);
		if(flags & envFilter) mptEnv.dwFlags.set(ENV_FILTER);
		if(flags & envCarry) mptEnv.dwFlags.set(ENV_CARRY);
		mptEnv.nNodes = std::min<uint8>(numNodes, 25);
		mptEnv.nSustainStart = sustainStart;
		mptEnv.nSustainEnd = sustainEnd;
		mptEnv.nLoopStart = loopStart;
		mptEnv.nLoopEnd = loopEnd;
		for(uint32 ev = 0; ev < mptEnv.nNodes; ev++)
		{
			mptEnv.Ticks[ev] = points[ev][0];
			if(ev > 0 && mptEnv.Ticks[ev] < mptEnv.Ticks[ev - 1])
				mptEnv.Ticks[ev] = mptEnv.Ticks[ev - 1] + 1;
			mptEnv.Values[ev] = static_cast<uint8>(Clamp(points[ev][1] >> envShift, 0, 64));
		}
	}
};

STATIC_ASSERT(sizeof(MO3Envelope) == 106);


struct PACKED MO3Instrument
{
	enum MO3InstrumentFlags
	{
		playOnMIDI  = 0x01,
		mute		= 0x02,
	};

	uint32 flags;		// See MO3InstrumentFlags
	uint16 sampleMap[120][2];
	MO3Envelope volEnv;
	MO3Envelope panEnv;
	MO3Envelope pitchEnv;
	struct XMVibratoSettings
	{
		uint8  type;
		uint8  sweep;
		uint8  depth;
		uint8  rate;
	} vibrato;			// Applies to all samples of this instrument (XM)
	uint16 fadeOut;
	uint8  midiChannel;
	uint8  midiBank;
	uint8  midiPatch;
	uint8  midiBend;
	uint8  globalVol;	// 0...128
	uint16 panning;		// 0...256 if enabled, 0xFFFF otherwise
	uint8  nna;
	uint8  pps;
	uint8  ppc;
	uint8  dct;
	uint8  dca;
	uint16 volSwing;	// 0...100
	uint16 panSwing;	// 0...256
	uint8  cutoff;		// 0...127, + 128 if enabled
	uint8  resonance;	// 0...127, + 128 if enabled

	// Convert all multi-byte numeric values to current platform's endianness or vice versa.
	void ConvertEndianness()
	{
		for(size_t i = 0; i < CountOf(sampleMap); i++)
		{
			SwapBytesLE(sampleMap[i][0]);
			SwapBytesLE(sampleMap[i][1]);
		}

		volEnv.ConvertEndianness();
		panEnv.ConvertEndianness();
		pitchEnv.ConvertEndianness();
		SwapBytesLE(fadeOut);
		SwapBytesLE(panning);
		SwapBytesLE(volSwing);
		SwapBytesLE(panSwing);
	}

	// Convert MO3 instrument data into OpenMPT's internal instrument format
	void ConvertToMPT(ModInstrument &mptIns, MODTYPE type) const
	{
		for(size_t i = 0; i < CountOf(sampleMap); i++)
		{
			mptIns.NoteMap[i] = static_cast<uint8>(sampleMap[i][0] + NOTE_MIN);
			mptIns.Keyboard[i] = sampleMap[i][1] + 1;
		}
		volEnv.ConvertToMPT(mptIns.VolEnv, 0);
		panEnv.ConvertToMPT(mptIns.PanEnv, 0);
		pitchEnv.ConvertToMPT(mptIns.PitchEnv, 5);
		mptIns.nFadeOut = fadeOut;
		if(midiChannel >= 128)
		{
			// Plugin
			mptIns.nMixPlug = midiChannel - 127;
		} else if(midiChannel < 17 && (flags & playOnMIDI))
		{
			// XM
			mptIns.nMidiChannel = midiChannel + MidiFirstChannel;
		} else if(midiChannel > 0 && midiChannel < 17)
		{
			// IT (yes, channel 0 is represented the same way as "no channel")
			mptIns.nMidiChannel = midiChannel + MidiFirstChannel;
		}
		mptIns.wMidiBank = midiBank;
		mptIns.nMidiProgram = midiPatch;
		mptIns.midiPWD =  midiBend;
		if(type == MOD_TYPE_IT)
			mptIns.nGlobalVol = std::min<uint8>(globalVol, 128) / 2u;
		if(panning <= 256)
		{
			mptIns.nPan = panning;
			mptIns.dwFlags.set(INS_SETPANNING);
		}
		mptIns.nNNA = nna;
		mptIns.nPPS = pps;
		mptIns.nPPC = ppc;
		mptIns.nDCT = dct;
		mptIns.nDNA = dca;
		mptIns.nVolSwing = static_cast<uint8>(std::min(volSwing, uint16(100)));
		mptIns.nPanSwing = static_cast<uint8>(std::min(panSwing, uint16(64)) / 4u);
		mptIns.SetCutoff(cutoff & 0x7F, (cutoff & 0x80) != 0);
		mptIns.SetResonance(resonance & 0x7F, (resonance & 0x80) != 0);
	}
};

STATIC_ASSERT(sizeof(MO3Instrument) == 826);


struct PACKED MO3Sample
{
	enum MO3SampleFlags
	{
		smp16Bit			= 0x01,
		smpLoop				= 0x10,
		smpPingPongLoop		= 0x20,
		smpSustain			= 0x100,
		smpSustainPingPong	= 0x200,
		smpStereo			= 0x400,
		smpCompressionMP3	= 0x1000,					// MP3 sample
		smpCompressionOGG	= 0x1000 | 0x2000,			// OGG sample
		smpSharedOGG		= 0x1000 | 0x2000 | 0x4000,	// OGG sample with shared vorbis header
		smpDeltaCompression	= 0x2000,					// Deltas + compression
		smpDeltaPrediction	= 0x4000,					// Delta prediction + compression
		smpCompressionMask	= 0x1000 | 0x2000 | 0x4000
	};

	int32  freqFinetune;	// Frequency in S3M and IT, finetune (0...255) in MOD, MTM, XM
	int8   transpose;
	uint8  defaultVolume;	// 0...64
	uint16 panning;			// 0...256 if enabled, 0xFFFF otherwise
	uint32 length;
	uint32 loopStart;
	uint32 loopEnd;
	uint16 flags;			// See MO3SampleFlags
	uint8  vibType;
	uint8  vibSweep;
	uint8  vibDepth;
	uint8  vibRate;
	uint8  globalVol;		// 0...64
	uint32 sustainStart;
	uint32 sustainEnd;
	int32  compressedSize;
	uint16 encoderDelay;

	// Convert all multi-byte numeric values to current platform's endianness or vice versa.
	void ConvertEndianness()
	{
		SwapBytesLE(freqFinetune);
		SwapBytesLE(panning);
		SwapBytesLE(length);
		SwapBytesLE(loopStart);
		SwapBytesLE(loopEnd);
		SwapBytesLE(flags);
		SwapBytesLE(sustainStart);
		SwapBytesLE(sustainEnd);
		SwapBytesLE(compressedSize);
		SwapBytesLE(encoderDelay);
	}

	// Convert MO3 sample data into OpenMPT's internal instrument format
	void ConvertToMPT(ModSample &mptSmp, MODTYPE type, uint8 version) const
	{
		mptSmp.Initialize();
		if(type & (MOD_TYPE_IT | MOD_TYPE_S3M))
		{
			if(version >= 5)
				mptSmp.nC5Speed = static_cast<uint32>(freqFinetune);
			else
				mptSmp.nC5Speed = Util::Round<uint32>(15787.0 * std::pow(2.0, freqFinetune / 1536.0));
		} else if(type != MOD_TYPE_MTM)
		{
			mptSmp.nFineTune = static_cast<int8>(freqFinetune - 128);
			mptSmp.RelativeTone = transpose;
		}
		mptSmp.nVolume = std::min(defaultVolume, uint8(64)) * 4u;
		if(panning <= 256)
		{
			mptSmp.nPan = panning;
			mptSmp.uFlags.set(CHN_PANNING);
		}
		mptSmp.nLength = length;
		mptSmp.nLoopStart = loopStart;
		mptSmp.nLoopEnd = loopEnd;
		if(flags & smpLoop) mptSmp.uFlags.set(CHN_LOOP);
		if(flags & smpPingPongLoop) mptSmp.uFlags.set(CHN_PINGPONGLOOP);
		if(flags & smpSustain) mptSmp.uFlags.set(CHN_SUSTAINLOOP);
		if(flags & smpSustainPingPong) mptSmp.uFlags.set(CHN_PINGPONGSUSTAIN);

		mptSmp.nVibType = AutoVibratoIT2XM[vibType & 7];
		mptSmp.nVibSweep = vibSweep;
		mptSmp.nVibDepth = vibDepth;
		mptSmp.nVibRate = vibRate;

		if(type == MOD_TYPE_IT)
			mptSmp.nGlobalVol = std::min(globalVol, uint8(64));
		mptSmp.nSustainStart = sustainStart;
		mptSmp.nSustainEnd = sustainEnd;
	}
};

STATIC_ASSERT(sizeof(MO3Sample) == 41);


#ifdef NEEDS_PRAGMA_PACK
#pragma pack(pop)
#endif

// Unpack macros

// shift control bits until it is empty:
// a 0 bit means literal : the next data byte is copied
// a 1 means compressed data
// then the next 2 bits determines what is the LZ ptr
// ('00' same as previous, else stored in stream)

#define READ_CTRL_BIT(n) \
	data <<= 1; \
	carry = (data >= (1 << (n))); \
	data &= ((1 << (n)) - 1); \
	if(data == 0) \
	{ \
		data = file.ReadUint8(); \
		data = (data << 1) + 1; \
		carry = (data >= (1 << (n))); \
		data &= ((1 << (n)) - 1); \
	}

// length coded within control stream:
// most significant bit is 1
// than the first bit of each bits pair (noted n1),
// until second bit is 0 (noted n0)

#define DECODE_CTRL_BITS \
{ \
	strLen++; \
	do { \
		READ_CTRL_BIT(8); \
		strLen = (strLen << 1) + carry; \
		READ_CTRL_BIT(8); \
	} while(carry); \
}

static bool UnpackMO3Data(FileReader &file, uint8 *dst, uint32 size)
//------------------------------------------------------------------
{
	if(!size)
	{
		return false;
	}

	uint16 data = 0;
	int8 carry = 0;			// x86 carry (used to propagate the most significant bit from one byte to another)
	int32 strLen = 0;		// length of previous string
	int32 strOffset;		// string offset
	uint8 *initDst = dst;
	uint32 ebp, previousPtr = 0;
	uint32 initSize = size;

	// Read first uncompressed byte
	*dst++ = file.ReadUint8();
	size--;

	while(size > 0)
	{
		READ_CTRL_BIT(8);
		if(!carry)
		{
			// a 0 ctrl bit means 'copy', not compressed byte
			*dst++ = file.ReadUint8();
			size--;
		} else
		{
			// a 1 ctrl bit means compressed bytes are following
			ebp = 0; // lenth adjustement
			DECODE_CTRL_BITS; // read length, and if strLen > 3 (coded using more than 1 bits pair) also part of the offset value
			strLen -=3;
			if(strLen < 0)
			{
				// means LZ ptr with same previous relative LZ ptr (saved one)
				strOffset = previousPtr;	// restore previous Ptr
				strLen++;
			} else
			{
				// LZ ptr in ctrl stream
				strOffset = (strLen << 8) | file.ReadUint8(); // read less significant offset byte from stream
				strLen = 0;
				strOffset = ~strOffset;
				if(strOffset < -1280)
					ebp++;
				ebp++;	// length is always at least 1
				if(strOffset < -32000)
					ebp++;
				previousPtr = strOffset; // save current Ptr
			}

			// read the next 2 bits as part of strLen
			READ_CTRL_BIT(8);
			strLen = (strLen << 1) + carry;
			READ_CTRL_BIT(8);
			strLen = (strLen << 1) + carry;
			if(strLen == 0)
			{
				// length does not fit in 2 bits
				DECODE_CTRL_BITS;	// decode length: 1 is the most significant bit,
				strLen += 2;		// then first bit of each bits pairs (noted n1), until n0.
			}
			strLen = strLen + ebp; // length adjustement
			if(size >= static_cast<uint32>(strLen))
			{
				const uint8 *string = dst + strOffset; // pointer to previous string
				if(string < initDst || string >= dst)
				{
					break;
				}
				size -= strLen;
				while(strLen > 0)
				{
					*dst++ = *string++; // copies it
					strLen--;
				}
			} else
			{
				break;
			}
		}
	}
	return (dst - initDst) == static_cast<ptrdiff_t>(initSize);
}


struct MO3Delta8BitParams
{
	typedef int8 sample_t;
	typedef uint8 unsigned_t;
	static const int shift = 7;
	static const uint8 dhInit = 4;

	static inline void Decode(FileReader &file, int8 &carry, uint16 &data, uint8 &/*dh*/, unsigned_t &val)
	{
		do
		{
			READ_CTRL_BIT(8);
			val = (val << 1) + carry;
			READ_CTRL_BIT(8);
		} while(carry);
	}
};

struct MO3Delta16BitParams
{
	typedef int16 sample_t;
	typedef uint16 unsigned_t;
	static const int shift = 15;
	static const uint8 dhInit = 8;

	static inline void Decode(FileReader &file, int8 &carry, uint16 &data, uint8 &dh, unsigned_t &val)
	{
		if(dh < 5)
		{
			do
			{
				READ_CTRL_BIT(8);
				val = (val << 1) + carry;
				READ_CTRL_BIT(8);
				val = (val << 1) + carry;
				READ_CTRL_BIT(8); \
			} while(carry);
		} else
		{
			do
			{
				READ_CTRL_BIT(8);
				val = (val << 1) + carry;
				READ_CTRL_BIT(8);
			} while(carry);
		}
	}
};


template<typename Properties>
static void UnpackMO3DeltaSample(FileReader &file, typename Properties::sample_t *dst, uint32 length, uint8 numChannels)
//----------------------------------------------------------------------------------------------------------------------
{
	uint8 dh = Properties::dhInit, cl = 0;
	int8 carry = 0;
	uint16 data = 0;
	typename Properties::unsigned_t val;
	typename Properties::sample_t previous = 0;

	for(uint8 chn = 0; chn < numChannels; chn++)
	{
		typename Properties::sample_t *p = dst + chn;
		const typename Properties::sample_t * const pEnd = p + length * numChannels;
		while(p < pEnd)
		{
			val = 0;
			Properties::Decode(file, carry, data, dh, val);
			cl = dh;
			while(cl > 0)
			{
				READ_CTRL_BIT(8);
				val = (val << 1) + carry;
				cl--;
			}
			cl = 1;
			if(val >= 4)
			{
				cl = Properties::shift;
				while(((1 << cl) & val) == 0 && cl > 1)
					cl--;
			}
			dh = dh + cl;
			dh >>= 1;			// next length in bits of encoded delta second part
			carry = val & 1;	// sign of delta 1=+, 0=not
			val >>= 1;
			if(carry == 0)
				val = ~val;		// negative delta
			val += previous;	// previous value + delta
			*p = val;
			p += numChannels;
			previous = val;
		}
	}
}


template<typename Properties>
static void UnpackMO3DeltaPredictionSample(FileReader &file, typename Properties::sample_t *dst, uint32 length, uint8 numChannels)
//--------------------------------------------------------------------------------------------------------------------------------
{
	uint8 dh = Properties::dhInit, cl = 0;
	int8 carry;
	uint16 data = 0;
	int32 next = 0;
	typename Properties::unsigned_t val = 0;
	typename Properties::sample_t sval = 0, delta = 0, previous = 0;

	for(uint8 chn = 0; chn < numChannels; chn++)
	{
		typename Properties::sample_t *p = dst + chn;
		const typename Properties::sample_t * const pEnd = p + length * numChannels;
		while(p < pEnd)
		{
			val = 0;
			Properties::Decode(file, carry, data, dh, val);
			cl = dh;	// length in bits of: delta second part (right most bits of delta) and sign bit
			while(cl > 0)
			{
				READ_CTRL_BIT(8);
				val = (val << 1) + carry;
				cl--;
			}
			cl = 1;
			if(val >= 4)
			{
				cl = Properties::shift;
				while(((1 << cl) & val) == 0 && cl > 1)
					cl--;
			}
			dh = dh + cl;
			dh >>= 1;			// next length in bits of encoded delta second part
			carry = val & 1;	// sign of delta 1=+, 0=not
			val >>= 1;
			if(carry == 0)
				val = ~val;		// negative delta

			delta = static_cast<typename Properties::sample_t>(val);
			val = val + static_cast<typename Properties::unsigned_t>(next);	// predicted value + delta
			*p = val;
			p += numChannels;
			sval = static_cast<typename Properties::sample_t>(val);
			next = (sval << 1) + (delta >> 1) - previous;  // corrected next value

			Limit(next, std::numeric_limits<typename Properties::sample_t>::min(), std::numeric_limits<typename Properties::sample_t>::max());

			previous = sval;
		}
	}
}


#undef READ_CTRL_BIT
#undef DECODE_CTRL_BITS

#endif // MPT_BUILTIN_MO3



bool CSoundFile::ReadMO3(FileReader &file, ModLoadingFlags loadFlags)
//-------------------------------------------------------------------
{
	file.Rewind();

	// No valid MO3 file (magic bytes: "MO3")
	if(!file.CanRead(12) || !file.ReadMagic("MO3"))
	{
		return false;
	}
	const uint8 version = file.ReadUint8();
	const uint32 musicSize = file.ReadUint32LE();
	if(musicSize <= 422 /*sizeof(MO3FileHeader) */)
	{
		return false;
	} else if(loadFlags == onlyVerifyHeader)
	{
		return true;
	}

#ifdef NO_MO3
	// As of November 2015, the format revision is 5; Versions > 31 are unlikely to exist in the next few years,
	// so we will just ignore those if there's no UNMO3 library to tell us if the file is valid or not
	// (avoid log entry with .MOD files that have a song name starting with "MO3".
	if(version > 31)
	{
		return false;
	}

	AddToLog(GetStrI18N("The file appears to be a MO3 file, but this OpenMPT build does not support loading MO3 files."));
	return false;

#else // !NO_MO3
	MPT_UNREFERENCED_PARAMETER(version);

	// Try to load unmo3 dynamically.
	ComponentHandle<ComponentUnMO3> unmo3;
	if(IsComponentAvailable(unmo3))
	{
		file.Rewind();
		const void *stream = file.GetRawData();
		uint32 length = mpt::saturate_cast<uint32>(file.GetLength());

		if(unmo3->UNMO3_Decode(&stream, &length, (loadFlags & loadSampleData) ? 0 : 1) != 0)
		{
			return false;
		}

		// If decoding was successful, stream and length will keep the new pointers now.
		FileReader unpackedFile(stream, length);

		bool result = false;	// Result of trying to load the module, false == fail.

		result = ReadXM(unpackedFile, loadFlags)
			|| ReadIT(unpackedFile, loadFlags)
			|| ReadS3M(unpackedFile, loadFlags)
			|| ReadMTM(unpackedFile, loadFlags)
			|| ReadMod(unpackedFile, loadFlags)
			|| ReadM15(unpackedFile, loadFlags);
		if(result)
		{
			m_ContainerType = MOD_CONTAINERTYPE_MO3;
		}

		unmo3->UNMO3_Free(stream);

		if(result)
		{
			return true;
		}
	} else
	{
#ifndef MPT_BUILTIN_MO3
		AddToLog(GetStrI18N("Loading MO3 file failed because unmo3.dll could not be loaded."));
		return false;
#endif // MPT_BUILTIN_MO3
	}
#endif // NO_MO3

#ifdef MPT_BUILTIN_MO3

	if(version > 5)
	{
		return false;
	}

	uint8 *musicData = new (std::nothrow) uint8[musicSize];
	if(musicData == nullptr)
	{
		return false;
	}
	uint32 compressedSize = uint32_max;
	if(version >= 5)
	{
		// Size of compressed music chunk
		compressedSize = file.ReadUint32LE();
	}

	if(!UnpackMO3Data(file, musicData, musicSize))
	{
		delete[] musicData;
		return false;
	}
	if(version >= 5)
	{
		file.Seek(12 + compressedSize);
	}

	InitializeGlobals();
	InitializeChannels();

	FileReader musicChunk(musicData, musicSize);
	musicChunk.ReadNullString(m_songName);
	std::string message;
	musicChunk.ReadNullString(m_songMessage);

	MO3FileHeader fileHeader;
	if(!musicChunk.ReadConvertEndianness(fileHeader)
		|| fileHeader.numChannels == 0 || fileHeader.numChannels > 64
		|| fileHeader.numInstruments >= MAX_INSTRUMENTS
		|| fileHeader.numSamples >= MAX_SAMPLES)
	{
		delete[] musicData;
		return false;
	}

	m_nChannels = fileHeader.numChannels;
	m_nRestartPos = fileHeader.restartPos;
	m_nInstruments = fileHeader.numInstruments;
	m_nSamples = fileHeader.numSamples;
	m_nDefaultSpeed = fileHeader.defaultSpeed ? fileHeader.defaultSpeed : 6;
	m_nDefaultTempo.Set(fileHeader.defaultTempo ? fileHeader.defaultTempo : 125, 0);

	m_madeWithTracker = "MO3";
	m_ContainerType = MOD_CONTAINERTYPE_MO3;
	ASSERT(!(fileHeader.flags & 0x4000));
	if(fileHeader.flags & MO3FileHeader::isIT)
		m_nType = MOD_TYPE_IT;
	else if(fileHeader.flags & MO3FileHeader::isS3M)
		m_nType = MOD_TYPE_S3M;
	else if(fileHeader.flags & MO3FileHeader::isMOD)
		m_nType = MOD_TYPE_MOD;
	else if(fileHeader.flags & MO3FileHeader::isMTM)
		m_nType = MOD_TYPE_MTM;
	else
		m_nType = MOD_TYPE_XM;

	if(fileHeader.flags & MO3FileHeader::linearSlides)
		m_SongFlags.set(SONG_LINEARSLIDES);
	if(fileHeader.flags & MO3FileHeader::s3mAmigaLimits)
		m_SongFlags.set(SONG_AMIGALIMITS);
	if(fileHeader.flags & MO3FileHeader::s3mFastSlides)
		m_SongFlags.set(SONG_FASTVOLSLIDES);
	if(fileHeader.flags & MO3FileHeader::itOldFX)
		m_SongFlags.set(SONG_ITOLDEFFECTS);
	if(fileHeader.flags & MO3FileHeader::itCompatGxx)
		m_SongFlags.set(SONG_ITCOMPATGXX);
	if(fileHeader.flags & MO3FileHeader::extFilterRange)
		m_SongFlags.set(SONG_EXFILTERRANGE);
	SetModFlag(MSF_COMPATIBLE_PLAY, !(fileHeader.flags & MO3FileHeader::modplugMode));

	if(m_nType == MOD_TYPE_IT)
	{
		m_nDefaultGlobalVolume = std::min<uint16>(fileHeader.globalVol, 128) * 2;
	} else if(m_nType == MOD_TYPE_XM)
	{
		m_nDefaultGlobalVolume = std::min<uint16>(fileHeader.globalVol, 64) * 4;
	}

	if(fileHeader.sampleVolume < 0)
		m_nSamplePreAmp = fileHeader.sampleVolume + 52;
	else
		m_nSamplePreAmp = static_cast<uint32>(std::exp(fileHeader.sampleVolume * 3.1 / 20.0)) + 51;

	STATIC_ASSERT(MAX_BASECHANNELS >= 64);
	for(CHANNELINDEX i = 0; i < 64; i++)
	{
		if(m_nType == MOD_TYPE_IT)
		{
			ChnSettings[i].nVolume = std::min(fileHeader.chnVolume[i], uint8(64));
		}
		ChnSettings[i].nPan = fileHeader.chnPan[i];
		if(ChnSettings[i].nPan == 127)
		{
			ChnSettings[i].nPan = 128;
			ChnSettings[i].dwFlags = CHN_SURROUND;
		}
	}

	bool anyMacros = false;
	for(uint32 i = 0; i < 16; i++)
	{
		if(fileHeader.sfxMacros[i])
			anyMacros = true;
	}
	for(uint32 i = 0; i < 128; i++)
	{
		if(fileHeader.fixedMacros[i][1])
			anyMacros = true;
	}

	if(anyMacros)
	{
		for(uint32 i = 0; i < 16; i++)
		{
			if(fileHeader.sfxMacros[i])
				sprintf(m_MidiCfg.szMidiSFXExt[i], "F0F0%02Xz", fileHeader.sfxMacros[i] - 1);
			else
				strcpy(m_MidiCfg.szMidiSFXExt[i], "");
		}
		for(uint32 i = 0; i < 128; i++)
		{
			if(fileHeader.fixedMacros[i][1])
				sprintf(m_MidiCfg.szMidiZXXExt[i], "F0F0%02X%02X", fileHeader.fixedMacros[i][1] - 1, fileHeader.fixedMacros[i][0]);
			else
				strcpy(m_MidiCfg.szMidiZXXExt[i], "");
		}
		m_SongFlags.set(SONG_EMBEDMIDICFG, !m_MidiCfg.IsMacroDefaultSetupUsed());
	}

	Order.ReadAsByte(musicChunk, fileHeader.numOrders, fileHeader.numOrders, 0xFF, 0xFE);

	// Track assignments for all patterns
	FileReader trackChunk = musicChunk.ReadChunk(fileHeader.numPatterns * fileHeader.numChannels * sizeof(uint16));
	FileReader patLengthChunk = musicChunk.ReadChunk(fileHeader.numPatterns * sizeof(uint16));
	std::vector<FileReader> tracks(fileHeader.numTracks);

	for(uint32 track = 0; track < fileHeader.numTracks; track++)
	{
		uint32 len = musicChunk.ReadUint32LE();
		tracks[track] = musicChunk.ReadChunk(len);
	}

	/*
	MO3 pattern commands:
	01 = Note
	02 = Instrument
	03 = CMD_ARPEGGIO (IT, XM, S3M, MOD, MTM)
	04 = CMD_PORTAMENTOUP (XM, MOD, MTM)   [for formats with separate fine slides]
	05 = CMD_PORTAMENTODOWN (XM, MOD, MTM) [for formats with separate fine slides]
	06 = CMD_TONEPORTAMENTO (IT, XM, S3M, MOD, MTM) / VOLCMD_TONEPORTA (IT, XM)
	07 = CMD_VIBRATO (IT, XM, S3M, MOD, MTM) / VOLCMD_VIBRATODEPTH (IT)
	08 = CMD_TONEPORTAVOL (XM, MOD, MTM)
	09 = CMD_VIBRATOVOL (XM, MOD, MTM)
	0A = CMD_TREMOLO (IT, XM, S3M, MOD, MTM)
	0B = CMD_PANNING8 (IT, XM, S3M, MOD, MTM) / VOLCMD_PANNING (IT, XM)
	0C = CMD_OFFSET (IT, XM, S3M, MOD, MTM)
	0D = CMD_VOLUMESLIDE (XM, MOD, MTM)
	0E = CMD_POSITIONJUMP (IT, XM, S3M, MOD, MTM)
	0F = CMD_VOLUME (XM, MOD, MTM) / VOLCMD_VOLUME (IT, XM, S3M)
	10 = CMD_PATTERNBREAK (IT, XM, MOD, MTM) - BCD-encoded in MOD/XM/S3M/MTM!
	11 = CMD_MODCMDEX (XM, MOD, MTM)
	12 = CMD_TEMPO (XM, MOD, MTM) / CMD_SPEED (XM, MOD, MTM)
	13 = CMD_TREMOR (XM)
	14 = VOLCMD_VOLSLIDEUP x=X0 (XM) / VOLCMD_VOLSLIDEDOWN x=0X (XM)
	15 = VOLCMD_FINEVOLUP x=X0 (XM) / VOLCMD_FINEVOLDOWN x=0X (XM)
	16 = CMD_GLOBALVOLUME (IT, XM, S3M)
	17 = CMD_GLOBALVOLSLIDE (XM)
	18 = CMD_KEYOFF (XM)
	19 = CMD_SETENVPOSITION (XM)
	1A = CMD_PANNINGSLIDE (XM)
	1B = VOLCMD_PANSLIDELEFT x=0X (XM) / VOLCMD_PANSLIDERIGHT x=X0 (XM)
	1C = CMD_RETRIG (XM)
	1D = CMD_XFINEPORTAUPDOWN X1x (XM)
	1E = CMD_XFINEPORTAUPDOWN X2x (XM)
	1F = VOLCMD_VIBRATOSPEED (XM)
	20 = VOLCMD_VIBRATODEPTH (XM)
	21 = CMD_SPEED (IT, S3M)
	22 = CMD_VOLUMESLIDE (IT, S3M)
	23 = CMD_PORTAMENTODOWN (IT, S3M) [for formats without separate fine slides]
	24 = CMD_PORTAMENTOUP (IT, S3M)   [for formats without separate fine slides]
	25 = CMD_TREMOR (IT, S3M)
	26 = CMD_RETRIG (IT, S3M)
	27 = CMD_FINEVIBRATO (IT, S3M)
	28 = CMD_CHANNELVOLUME (IT, S3M)
	29 = CMD_CHANNELVOLSLIDE (IT, S3M)
	2A = CMD_PANNINGSLIDE (IT, S3M)
	2B = CMD_S3MCMDEX (IT, S3M)
	2C = CMD_TEMPO (IT, S3M)
	2D = CMD_GLOBALVOLSLIDE (IT, S3M)
	2E = CMD_PANBRELLO (IT, XM, S3M)
	2F = CMD_MIDI (IT, XM, S3M)
	30 = VOLCMD_FINEVOLUP x=0...9 (IT) / VOLCMD_FINEVOLDOWN x=10...19 (IT) / VOLCMD_VOLSLIDEUP x=20...29 (IT) / VOLCMD_VOLSLIDEDOWN x=30...39 (IT)
	31 = VOLCMD_PORTADOWN (IT)
	32 = VOLCMD_PORTAUP (IT)

	Note: S3M/IT CMD_TONEPORTAVOL / CMD_VIBRATOVOL are encoded as:
	K= 07 00 22 x
	L= 06 00 22 x
	*/

	static const ModCommand::COMMAND effTrans[] =
	{
		CMD_NONE,				CMD_NONE,				CMD_NONE,				CMD_ARPEGGIO,
		CMD_PORTAMENTOUP,		CMD_PORTAMENTODOWN,		CMD_TONEPORTAMENTO,		CMD_VIBRATO,
		CMD_TONEPORTAVOL,		CMD_VIBRATOVOL,			CMD_TREMOLO,			CMD_PANNING8,
		CMD_OFFSET,				CMD_VOLUMESLIDE,		CMD_POSITIONJUMP,		CMD_VOLUME,
		CMD_PATTERNBREAK,		CMD_MODCMDEX,			CMD_TEMPO,				CMD_TREMOR,
		VOLCMD_VOLSLIDEUP,		VOLCMD_FINEVOLUP,		CMD_GLOBALVOLUME,		CMD_GLOBALVOLSLIDE,
		CMD_KEYOFF,				CMD_SETENVPOSITION,		CMD_PANNINGSLIDE,		VOLCMD_PANSLIDELEFT,
		CMD_RETRIG,				CMD_XFINEPORTAUPDOWN,	CMD_XFINEPORTAUPDOWN,	VOLCMD_VIBRATOSPEED,
		VOLCMD_VIBRATODEPTH,	CMD_SPEED,				CMD_VOLUMESLIDE,		CMD_PORTAMENTODOWN,
		CMD_PORTAMENTOUP,		CMD_TREMOR,				CMD_RETRIG,				CMD_FINEVIBRATO,
		CMD_CHANNELVOLUME,		CMD_CHANNELVOLSLIDE,	CMD_PANNINGSLIDE,		CMD_S3MCMDEX,
		CMD_TEMPO,				CMD_GLOBALVOLSLIDE,		CMD_PANBRELLO,			CMD_MIDI,
		VOLCMD_FINEVOLUP,		VOLCMD_PORTADOWN,		VOLCMD_PORTAUP,
	};

	uint8 noteOffset = NOTE_MIN;
	if(m_nType == MOD_TYPE_MTM)
		noteOffset = 13 + NOTE_MIN;
	else if(m_nType != MOD_TYPE_IT)
		noteOffset = 12 + NOTE_MIN;
	for(PATTERNINDEX pat = 0; pat < fileHeader.numPatterns; pat++)
	{
		const ROWINDEX numRows = patLengthChunk.ReadUint16LE();
		if(!(loadFlags & loadPatternData) || !Patterns.Insert(pat, numRows))
			continue;

		for(CHANNELINDEX chn = 0; chn < fileHeader.numChannels; chn++)
		{
			FileReader &track = tracks[trackChunk.ReadUint16LE()];
			track.Rewind();
			ROWINDEX row = 0;
			ModCommand *patData = Patterns[pat].GetpModCommand(0, chn);
			while(row < numRows)
			{
				const uint8 b = track.ReadUint8();
				if(!b)
					break;

				const uint8 numCommands = (b & 0x0F), rep = (b >> 4);
				ModCommand m = ModCommand::Empty();
				for(uint8 i = 0; i < numCommands; i++)
				{
					uint8 cmd[2];
					track.ReadArray(cmd);

					// Import pattern commands
					switch(cmd[0])
					{
					case 0x01:
						// Note
						m.note = cmd[1];
						if(m.note < 120) m.note += noteOffset;
						else if(m.note == 0xFF) m.note = NOTE_KEYOFF;
						else if(m.note == 0xFE) m.note = NOTE_NOTECUT;
						else m.note = NOTE_FADE;
						break;
					case 0x02:
						// Instrument
						m.instr = cmd[1] + 1;
						break;
					case 0x06:
						// Tone portamento
						if(m.volcmd == VOLCMD_NONE && m_nType == MOD_TYPE_XM && !(cmd[1] & 0x0F))
						{
							m.volcmd = VOLCMD_TONEPORTAMENTO;
							m.vol = cmd[1] >> 4;
							break;
						} else if(m.volcmd == VOLCMD_NONE && m_nType == MOD_TYPE_IT)
						{
							for(uint8 i = 0; i < 10; i++)
							{
								if(ImpulseTrackerPortaVolCmd[i] == cmd[1])
								{
									m.volcmd = VOLCMD_TONEPORTAMENTO;
									m.vol = i;
									break;
								}
							}
							if(m.volcmd != VOLCMD_NONE)
								break;
						}
						m.command = CMD_TONEPORTAMENTO;
						m.param = cmd[1];
						break;
					case 0x07:
						// Vibrato
						if(m.volcmd == VOLCMD_NONE && cmd[1] < 10 && m_nType == MOD_TYPE_IT)
						{
							m.volcmd = VOLCMD_VIBRATODEPTH;
							m.vol = cmd[1];
						} else
						{
							m.command = CMD_VIBRATO;
							m.param = cmd[1];
						}
						break;
					case 0x0B:
						// Panning
						if(m.volcmd == VOLCMD_NONE)
						{
							if(m_nType == MOD_TYPE_IT && cmd[1] == 0xFF)
							{
								m.volcmd = VOLCMD_PANNING;
								m.vol = 64;
								break;
							}
							if((m_nType == MOD_TYPE_IT && !(cmd[1] & 0x03))
								|| (m_nType == MOD_TYPE_XM && !(cmd[1] & 0x0F)))
							{
								m.volcmd = VOLCMD_PANNING;
								m.vol = cmd[1] / 4;
								break;
							}
						}
						m.command = CMD_PANNING8;
						m.param = cmd[1];
						break;
					case 0x0F:
						// Volume
						if(m_nType != MOD_TYPE_MOD && m.volcmd == VOLCMD_NONE && cmd[1] <= 64)
						{
							m.volcmd = VOLCMD_VOLUME;
							m.vol = cmd[1];
						} else
						{
							m.command = CMD_VOLUME;
							m.param = cmd[1];
						}
						break;
					case 0x10:
						// Pattern break
						m.command = CMD_PATTERNBREAK;
						m.param = cmd[1];
						if(m_nType != MOD_TYPE_IT)
							m.param = ((m.param >> 4) * 10) + (m.param & 0x0F);
						break;
					case 0x12:
						// Combined Tempo / Speed command
						m.param = cmd[1];
						if(m.param < 0x20)
							m.command = CMD_SPEED;
						else
							m.command = CMD_TEMPO;
						break;
					case 0x14:
					case 0x15:
						// XM volume column volume slides
						if(cmd[1] & 0xF0)
						{
							m.volcmd = static_cast<ModCommand::VOLCMD>((cmd[0] == 0x14) ? VOLCMD_VOLSLIDEUP : VOLCMD_FINEVOLUP);
							m.vol = cmd[1] >> 4;
						} else
						{
							m.volcmd = static_cast<ModCommand::VOLCMD>((cmd[0] == 0x14) ? VOLCMD_VOLSLIDEDOWN : VOLCMD_FINEVOLDOWN);
							m.vol = cmd[1] & 0x0F;
						}
						break;
					case 0x1B:
						// XM volume column panning slides
						if(cmd[1] & 0xF0)
						{
							m.volcmd = VOLCMD_PANSLIDERIGHT;
							m.vol = cmd[1] >> 4;
						} else
						{
							m.volcmd = VOLCMD_PANSLIDELEFT;
							m.vol = cmd[1] & 0x0F;
						}
						break;
					case 0x1D:
						// XM extra fine porta up
						m.command = CMD_XFINEPORTAUPDOWN;
						m.param = 0x10 | cmd[1];
						break;
					case 0x1E:
						// XM extra fine porta down
						m.command = CMD_XFINEPORTAUPDOWN;
						m.param = 0x20 | cmd[1];
						break;
					case 0x1F:
					case 0x20:
						// XM volume colume vibrato
						m.volcmd = effTrans[cmd[0]];
						m.vol = cmd[1];
						break;
					case 0x22:
						// IT / S3M volume slide
						if(m.command == CMD_TONEPORTAMENTO)
							m.command = CMD_TONEPORTAVOL;
						else if(m.command == CMD_VIBRATO)
							m.command = CMD_VIBRATOVOL;
						else
							m.command = CMD_VOLUMESLIDE;
						m.param = cmd[1];
						break;
					case 0x30:
						// IT volume column volume slides
						m.vol = cmd[1] % 10;
						if(cmd[1] < 10)
							m.volcmd = VOLCMD_FINEVOLUP;
						else if(cmd[1] < 20)
							m.volcmd = VOLCMD_FINEVOLDOWN;
						else if(cmd[1] < 30)
							m.volcmd = VOLCMD_VOLSLIDEUP;
						else if(cmd[1] < 40)
							m.volcmd = VOLCMD_VOLSLIDEDOWN;
						break;
					case 0x31:
					case 0x32:
						// IT volume column portamento
						m.volcmd = effTrans[cmd[0]];
						m.vol = cmd[1];
						break;
					default:
						if(cmd[0] < CountOf(effTrans))
						{
							m.command = effTrans[cmd[0]];
							m.param = cmd[1];
						}
						break;
					}
				}
#ifdef MODPLUG_TRACKER
				if(m_nType == MOD_TYPE_MTM)
					m.Convert(MOD_TYPE_MOD, MOD_TYPE_S3M, *this);
#endif
				ROWINDEX targetRow = std::min(row + rep, numRows);
				while(row < targetRow)
				{
					*patData = m;
					patData += fileHeader.numChannels;
					row++;
				}
			}
		}
	}

	const bool itSampleMode = (m_nType == MOD_TYPE_IT && !(fileHeader.flags & MO3FileHeader::instrumentMode));
	std::vector<MO3Instrument::XMVibratoSettings> instrVibrato(m_nType == MOD_TYPE_XM ? m_nInstruments : 0);
	for(INSTRUMENTINDEX ins = 1; ins <= m_nInstruments; ins++)
	{
		ModInstrument *pIns = nullptr;
		if(itSampleMode || (pIns = AllocateInstrument(ins)) == nullptr)
		{
			// Even in IT sample mode, instrument headers are still stored....
			while(musicChunk.ReadUint8() != 0);
			if(version >= 5)
			{
				while(musicChunk.ReadUint8() != 0);
			}
			musicChunk.Skip(sizeof(MO3Instrument));
			continue;
		}

		std::string name;
		musicChunk.ReadNullString(name);
		mpt::String::Copy(pIns->name, name);
		if(version >= 5)
		{
			musicChunk.ReadNullString(name);
			mpt::String::Copy(pIns->filename, name);
		}

		MO3Instrument insHeader;
		if(!musicChunk.ReadConvertEndianness(insHeader))
			break;
		insHeader.ConvertToMPT(*pIns, m_nType);

		if(m_nType == MOD_TYPE_XM)
			instrVibrato[ins - 1] = insHeader.vibrato;
	}
	if(itSampleMode)
		m_nInstruments = 0;

	bool unsupportedSamples = false;
	for(SAMPLEINDEX smp = 1; smp <= m_nSamples; smp++)
	{
		ModSample &sample = Samples[smp];
		std::string name;
		musicChunk.ReadNullString(name);
		mpt::String::Copy(m_szNames[smp], name);
		if(version >= 5)
		{
			musicChunk.ReadNullString(name);
			mpt::String::Copy(sample.filename, name);
		}

		MO3Sample smpHeader;
		if(!musicChunk.ReadConvertEndianness(smpHeader))
			break;
		smpHeader.ConvertToMPT(sample, m_nType, version);

		if(version >= 5 && (smpHeader.flags & MO3Sample::smpCompressionMask) == MO3Sample::smpSharedOGG)
		{
			// OGG header sharing
			musicChunk.Skip(2);
		}

		if(!(loadFlags & loadSampleData))
			continue;

		if(smpHeader.compressedSize > 0)
		{
			if(smpHeader.flags & MO3Sample::smp16Bit) sample.uFlags.set(CHN_16BIT);
			if(smpHeader.flags & MO3Sample::smpStereo) sample.uFlags.set(CHN_STEREO);

			FileReader sampleData = file.ReadChunk(smpHeader.compressedSize);
			uint32 compression = (smpHeader.flags & MO3Sample::smpCompressionMask);
			const uint8 numChannels = sample.GetNumChannels();
			if(!compression)
			{
				// Uncompressed sample
				SampleIO(
					(smpHeader.flags & MO3Sample::smp16Bit) ? SampleIO::_16bit : SampleIO::_8bit,
					(smpHeader.flags & MO3Sample::smpStereo) ? SampleIO::stereoSplit : SampleIO::mono,
					SampleIO::littleEndian,
					SampleIO::signedPCM)
					.ReadSample(Samples[smp], sampleData);
			} else if(compression == MO3Sample::smpDeltaCompression)
			{
				if(sample.AllocateSample())
				{
					if(smpHeader.flags & MO3Sample::smp16Bit)
						UnpackMO3DeltaSample<MO3Delta16BitParams>(sampleData, sample.pSample16, sample.nLength, numChannels);
					else
						UnpackMO3DeltaSample<MO3Delta8BitParams>(sampleData, sample.pSample8, sample.nLength, numChannels);
				}
			} else if(compression == MO3Sample::smpDeltaPrediction)
			{
				if(sample.AllocateSample())
				{
					if(smpHeader.flags & MO3Sample::smp16Bit)
						UnpackMO3DeltaPredictionSample<MO3Delta16BitParams>(sampleData, sample.pSample16, sample.nLength, numChannels);
					else
						UnpackMO3DeltaPredictionSample<MO3Delta8BitParams>(sampleData, sample.pSample8, sample.nLength, numChannels);
				}
			} else
			{
				unsupportedSamples = true;
			}
		} else if(smpHeader.compressedSize < 0 && -smpHeader.compressedSize < smp)
		{
			const ModSample &smpFrom = Samples[smp + smpHeader.compressedSize];
			LimitMax(sample.nLength, smpFrom.nLength);
			sample.uFlags.set(CHN_16BIT, smpFrom.uFlags[CHN_16BIT]);
			sample.uFlags.set(CHN_STEREO, smpFrom.uFlags[CHN_STEREO]);
			if(smpFrom.pSample != nullptr && sample.AllocateSample())
			{
				memcpy(sample.pSample, smpFrom.pSample, sample.GetSampleSizeInBytes());
			}
		}
	}

	if(m_nType == MOD_TYPE_XM)
	{
		// Transfer XM instrument vibrato to samples
		for(INSTRUMENTINDEX ins = 0; ins < m_nInstruments; ins++)
		{
			PropagateXMAutoVibrato(ins + 1, instrVibrato[ins].type, instrVibrato[ins].sweep, instrVibrato[ins].depth, instrVibrato[ins].rate);
		}
	}

#ifndef NO_VST
	if(musicChunk.CanRead(1))
	{
		// Plugin data
		uint8 pluginFlags = musicChunk.ReadUint8();
		if(pluginFlags & 1)
		{
			// Channel plugins
			for(CHANNELINDEX chn = 0; chn < m_nChannels; chn++)
			{
				ChnSettings[chn].nMixPlugin = static_cast<PLUGINDEX>(musicChunk.ReadUint32LE());
			}
		}
		while(musicChunk.CanRead(1))
		{
			PLUGINDEX plug = musicChunk.ReadUint8();
			if(!plug)
				break;
			FileReader pluginChunk = musicChunk.ReadChunk(musicChunk.ReadUint32LE());
			if(plug <= MAX_MIXPLUGINS)
			{
				ReadMixPluginChunk(pluginChunk, m_MixPlugins[plug - 1]);
			}
		}
	}
#endif // NO_VST

	delete[] musicData;

	if(unsupportedSamples)
	{
		AddToLog(LogWarning, MPT_USTRING("Some compressed samples could not be loaded because they use an unsupported codec."));
	}

	return true;
#else
	return false;
#endif // BUILTIN_MO3
}


OPENMPT_NAMESPACE_END
