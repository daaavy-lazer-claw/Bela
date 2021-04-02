/*
 * I2c_MultiTLVCodec.h
 *
 * Wrapper for multiple copies of the TLV320AIC310x
 * codec on the same McASP TDM bus (but with different
 * I2C addresses). Codec 0 provides the clock signals
 * via its PLL and the other codecs are clocked to it.
 *
 */

#include <vector>
#include "../include/I2c_MultiTLVCodec.h"
#include <map>
#include "../include/MiscUtilities.h"
using namespace StringUtils;

static const unsigned int kDataSize = 16;
#undef CODEC_GENERATES_WCLK

struct Address {
	unsigned int bus;
	uint8_t address;
	I2c_Codec::CodecType type;
	std::string required;
};

static const std::map<std::string, I2c_Codec::CodecType> codecTypeMap = {
        {"3104", I2c_Codec::TLV320AIC3104},
        {"3106", I2c_Codec::TLV320AIC3106},
};

[[noreturn]] static void throwErr(std::string err, std::string token = "")
{
	throw std::runtime_error("I2c_MultiTLVCodec: " + err + (token != "" ? "`" + token : "") + "`\n");
}

static I2c_Codec::CodecType getCodecTypeFromString(const std::string& str)
{
	try {
		return codecTypeMap.at(trim(str));
	} catch(std::exception e) {
		throwErr("Unrecognised codec type", str);
	}
}

I2c_MultiTLVCodec::I2c_MultiTLVCodec(const std::string& cfgString, TdmConfig tdmConfig, bool isVerbose)
: primaryCodec(nullptr), running(false), verbose(isVerbose)
{
	std::vector<Address> addresses;
	std::vector<std::string> tokens = split(cfgString, ';');
	if(tokens.size() < 1)
		throwErr("Wrong format for cfgString ", cfgString);
	if("" == trim(tokens.back()))
		tokens.pop_back();
	std::string mode;
	for(auto& token: tokens) {
		// the string will contain semicolon-separated label:value pairs
		// ADDR: <bus>,<addr>,<type>,<required>
		//  (where "required" can be:
		//   - `r` equired
		//   - `n` ot required
		//   - `d` isabled
		// MODE: <mode>
		// e.g.: ADDR: 2,24,3104,r;ADDR: 1,24,3106,o;MODE: noInit
		std::vector<std::string> tks = split(token, ':');
		if(tks.size() != 2)
			throwErr("Wrong format for token ", token);
		if("ADDR" == trim(tks[0])) {
			std::vector<std::string> ts = split(tks[1], ',');
			if(ts.size() != 4)
				throwErr("Wrong format for argument", tks[1]);
			addresses.push_back({
					.bus = (unsigned int)std::stoi(ts[0]),
					.address = (uint8_t)std::stoi(ts[1]),
					.type = getCodecTypeFromString(ts[2]),
					.required = trim(ts[3]),
				});
		} else if("MODE" == trim(tks[0]) && "" == mode) {
			mode = trim(tks[1]);
		} else {
			throwErr("Unknown token ", token);
		}
	}

	for(auto& addr : addresses) {
		unsigned int i2cBus = addr.bus;
		uint8_t address = addr.address;
		I2c_Codec::CodecType type = addr.type;
		std::string required = addr.required;
		// Check for presence of TLV codecs and take the first one we
		// find as the primary codec
		std::shared_ptr<I2c_Codec> testCodec(new I2c_Codec(i2cBus, address, type, isVerbose));
		testCodec->setMode(mode);
		if(testCodec->initCodec() != 0) {
			std::string err = "Codec requested but not found at: " + std::to_string(i2cBus) + ", " + std::to_string(address) + ", " + std::to_string(type) + "\n";
			if("r" == required)
				throwErr(err);
			if(verbose)
				fprintf(stderr, "%s", err.c_str());
		}
		else {
			// codec found
			if(verbose) {
				fprintf(stderr, "Found I2C codec on bus %d address %d, required: %s\n", i2cBus, address, required.c_str());
			}
			if("d" == required)
				disabledCodecs.push_back(testCodec);
			else
				codecs.push_back(testCodec);
		}
	}
	if(!codecs.size()) {
		return;
	}

	primaryCodec = codecs[0];
	// primaryCodec generates bclk (and possibly wclk) with its PLL
	// and occupies the first two slots starting from tdmConfig.firstSlot
	unsigned int slotNum = tdmConfig.firstSlot;
	bool primaryCodecGeneratesWclk =
#ifdef CODEC_GENERATES_WCLK
		true; // primaryCodec generates word clock
#else
		false; // AM335x or external device generates word clock
#endif

	AudioCodecParams params;
	params.slotSize = tdmConfig.slotSize;
	params.bitDelay = tdmConfig.bitDelay;
	params.dualRate = false;
	params.tdmMode = true;
	params.startingSlot = slotNum;
	params.generatesBclk = true;
	params.generatesWclk = primaryCodecGeneratesWclk;
	params.mclk = primaryCodec->getMcaspConfig().getValidAhclk(24000000);
	params.samplingRate = 44100;
	primaryCodec->setParameters(params);

	params.generatesBclk = false;
	params.generatesWclk = false;
	for(auto& codec : codecs) {
		if(codec == primaryCodec)
			continue;
		slotNum += 2;
		params.startingSlot = slotNum;
		codec->setParameters(params);
	}
	// initialise the McASP configuration.
	mcaspConfig = primaryCodec->getMcaspConfig();
	mcaspConfig.params.dataSize = kDataSize;
	mcaspConfig.params.inChannels = getNumIns();
	mcaspConfig.params.outChannels = getNumOuts();
}

#define DO_AND_RETURN_ON_ERR(DO) \
	int ret; \
	if((ret = DO)) \
		return ret; \
// end DO_AND_RETURN_ON_ERR

#define FOR_EACH_CODEC_DO_PRIMARY(DO,PRIMARY_POS) \
	if(1 == PRIMARY_POS) { \
		DO_AND_RETURN_ON_ERR(primaryCodec->DO); \
	} \
	for(auto& c : codecs) { \
		if(PRIMARY_POS && primaryCodec == c) \
			continue; \
		DO_AND_RETURN_ON_ERR(c->DO); \
	} \
	if(2 == PRIMARY_POS) { \
		DO_AND_RETURN_ON_ERR(primaryCodec->DO); \
	} \
// end FOR_EACH_CODEC_DO_PRIMARY

#define FOR_EACH_CODEC_DO_PRIMARY_FIRST(DO) FOR_EACH_CODEC_DO_PRIMARY(DO,1)
#define FOR_EACH_CODEC_DO_PRIMARY_LAST(DO) FOR_EACH_CODEC_DO_PRIMARY(DO,2)
#define FOR_EACH_CODEC_DO(DO) FOR_EACH_CODEC_DO_PRIMARY(DO,0)

// This method initialises the audio codec to its default state
int I2c_MultiTLVCodec::initCodec()
{
	FOR_EACH_CODEC_DO(initCodec());
	return 0;
}

// Tell the codec to start generating audio
int I2c_MultiTLVCodec::startAudio(int dual_rate)
{
	FOR_EACH_CODEC_DO(startAudio(dual_rate));
	running = true;
	return 0;
}


int I2c_MultiTLVCodec::stopAudio()
{
	if(running) {
		FOR_EACH_CODEC_DO_PRIMARY_LAST(stopAudio());
	}

	running = false;
	return 0;
}

int I2c_MultiTLVCodec::setPga(float newGain, unsigned short int channel)
{
	FOR_EACH_CODEC_DO(setPga(newGain, channel));
	return 0;
}

int I2c_MultiTLVCodec::setDACVolume(int halfDbSteps)
{
	FOR_EACH_CODEC_DO(setDACVolume(halfDbSteps));
	return 0;
}

int I2c_MultiTLVCodec::setADCVolume(int halfDbSteps)
{
	FOR_EACH_CODEC_DO(setADCVolume(halfDbSteps));
	return 0;
}

int I2c_MultiTLVCodec::setHPVolume(int halfDbSteps)
{
	FOR_EACH_CODEC_DO(setHPVolume(halfDbSteps));
	return 0;
}

int I2c_MultiTLVCodec::disable()
{
	// Disable extra codecs first
	FOR_EACH_CODEC_DO_PRIMARY_LAST(stopAudio());
	return 0;
}

int I2c_MultiTLVCodec::reset()
{
	FOR_EACH_CODEC_DO(reset());
	return 0;
}

// How many I2C codecs were found?
int I2c_MultiTLVCodec::numDetectedCodecs()
{
	return codecs.size();
}

// For debugging purposes only!
void I2c_MultiTLVCodec::debugWriteRegister(int codecNum, int regNum, int value) {
	codecs.at(codecNum)->writeRegister(regNum, value);
}

int I2c_MultiTLVCodec::debugReadRegister(int codecNum, int regNum) {
	return codecs.at(codecNum)->readRegister(regNum);
}

I2c_MultiTLVCodec::~I2c_MultiTLVCodec()
{
	stopAudio();
}

McaspConfig& I2c_MultiTLVCodec::getMcaspConfig()
{
	return mcaspConfig;
/*
// Values below are for 16x 16-bit TDM slots
#define BELA_MULTI_TLV_MCASP_DATA_FORMAT_TX_VALUE 0x8074 // MSB first, 0 bit delay, 16 bits, DAT bus, ROR 16bits
#define BELA_MULTI_TLV_MCASP_ACLKXCTL_VALUE 0x00 // External clk, polarity (falling edge)
#define BELA_MULTI_TLV_MCASP_DATA_FORMAT_RX_VALUE 0x8074 // MSB first, 0 bit delay, 16 bits, DAT bus, ROR 16bits
#define BELA_MULTI_TLV_MCASP_ACLKRCTL_VALUE 0x00 // External clk, polarity (falling edge)
#ifdef CODEC_GENERATES_WCLK
#define BELA_MULTI_TLV_MCASP_AFSRCTL_VALUE 0x800 // 16-slot TDM external fsclk, rising edge means beginning of frame
#define BELA_MULTI_TLV_MCASP_AFSXCTL_VALUE 0x800 // 16-slot TDM external fsclk, rising edge means beginning of frame
#define MCASP_OUTPUT_PINS MCASP_PIN_AHCLKX | (1 << 2) // AHCLKX and AXR2 outputs
#else // CODEC_GENERATES_WCLK
#define BELA_MULTI_TLV_MCASP_AFSRCTL_VALUE 0x802 // 16-slot TDM internal fsclk, rising edge means beginning of frame
#define BELA_MULTI_TLV_MCASP_AFSXCTL_VALUE 0x802 // 16-slot TDM internal fsclk, rising edge means beginning of frame
#define MCASP_OUTPUT_PINS MCASP_PIN_AHCLKX | MCASP_PIN_AFSX | (1 << 2) // AHCLKX, FSX, AXR2 outputs
#endif // CODEC_GENERATES_WCLK
*/
}

unsigned int I2c_MultiTLVCodec::getNumIns(){
	unsigned int sum = 0;
	for(auto& c : codecs)
		sum += c->getNumIns();
	return sum;
}

unsigned int I2c_MultiTLVCodec::getNumOuts(){
	unsigned int sum = 0;
	for(auto& c : codecs)
		sum += c->getNumOuts();
	return sum;
}

float I2c_MultiTLVCodec::getSampleRate() {
	if(primaryCodec)
		return primaryCodec->getSampleRate();
	return 0;
}
