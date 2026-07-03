#ifdef ENABLE_MMT4K
#include "mmt4kConverter.h"

#include <iostream>

#include "acasHandler.h"
#include "casProxy.h"
#include "config.h"
#include "demuxerTeeHandler.h"
#include "mmtTlvDemuxer.h"
#include "mmtsRecorder.h"
#include "remuxerHandler.h"
#include "smartCard.h"
#include "stream.h"

namespace {

// Feeds MFU/MPT events to MmtsRecorder so its .mmtsmap sidecar (track/seek-point
// index) stays in sync with whatever gets saved via StartMmtsRecording(), without
// disturbing the existing RemuxerHandler -> MPEG2-TS output path.
class MmtsRecordingTapHandler : public MmtTlv::DemuxerHandler
{
public:
	void onVideoData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override
	{
		MmtsRecorder::OnVideoData(stream, mfu);
	}
	void onAudioData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override
	{
		MmtsRecorder::OnAudioData(stream, mfu);
	}
	void onSubtitleData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override
	{
		MmtsRecorder::OnSubtitleData(stream, mfu);
	}
	void onMpt(const MmtTlv::Mpt& mpt) override
	{
		MmtsRecorder::OnMpt(mpt);
	}
};

} // namespace

struct Mmt4kConverter::Impl
{
	MmtTlv::MmtTlvDemuxer demuxer;
	RemuxerHandler handler{ demuxer };
	MmtsRecordingTapHandler recordingTapHandler;
	MmtTlv::DemuxerTeeHandler teeHandler{ handler, recordingTapHandler };
	std::vector<uint8_t> inputBuffer;
	std::vector<uint8_t> remuxOutput;

	Impl()
	{
		handler.setOutputCallback([this](const uint8_t* data, size_t size) {
			if (size == 188) {
				remuxOutput.insert(remuxOutput.end(), data, data + size);
			}
		});
		demuxer.setDemuxerHandler(teeHandler);
		// While StartMmtsRecording() is active, feed each decoded (ACAS-decrypted)
		// TLV packet straight to MmtsRecorder, same as dantto4k + Write_MMTS do.
		demuxer.setDecodedDumpCallback([](const uint8_t* data, size_t size) {
			MmtsRecorder::WriteDecoded(data, size);
		});
		demuxer.setDecodedDumpErrorCallback([]() {
			MmtsRecorder::MarkDecodeFailure();
		});
	}
};

Mmt4kConverter::Mmt4kConverter() : impl(std::make_unique<Impl>()) {}
Mmt4kConverter::~Mmt4kConverter() = default;

bool Mmt4kConverter::Init(const std::string& smartCardReaderName, const std::string& casProxyServer, const std::string& customWinscardDLL, bool convertResolutionGaiji)
{
	config.smartCardReaderName = smartCardReaderName;
	config.casProxyServer = casProxyServer;
	config.customWinscardDLL = customWinscardDLL;
	config.convertResolutionGaiji = convertResolutionGaiji;

	try {
		auto acasHandler = std::make_unique<AcasHandler>();
		std::unique_ptr<ISmartCard> smartCard;

		if (casProxyServer.empty()) {
			smartCard = std::make_unique<LocalSmartCard>();
		}
		else {
			auto parsed = casproxy::parseAddress(casProxyServer);
			if (!parsed) {
				std::cerr << "Mmt4kConverter: invalid casProxyServer address" << std::endl;
				return false;
			}
			smartCard = std::make_unique<RemoteSmartCard>(parsed->first, parsed->second);
		}

		smartCard->setSmartCardReaderName(smartCardReaderName);
		acasHandler->setSmartCard(std::move(smartCard));
		impl->demuxer.setCasHandler(std::move(acasHandler));
	}
	catch (const std::exception& e) {
		std::cerr << "Mmt4kConverter::Init: " << e.what() << std::endl;
		return false;
	}

	return true;
}

void Mmt4kConverter::Push(const uint8_t* data, size_t size)
{
	impl->inputBuffer.insert(impl->inputBuffer.end(), data, data + size);

	MmtTlv::Common::ReadStream input(impl->inputBuffer);
	while (!input.isEof()) {
		if (impl->demuxer.demux(input) == MmtTlv::DemuxStatus::NotEnoughBuffer) {
			break;
		}
	}

	impl->inputBuffer.erase(impl->inputBuffer.begin(), impl->inputBuffer.begin() + (impl->inputBuffer.size() - input.leftBytes()));
}

std::vector<uint8_t> Mmt4kConverter::TakeOutput()
{
	std::vector<uint8_t> out = std::move(impl->remuxOutput);
	impl->remuxOutput.clear();
	return out;
}

void Mmt4kConverter::Reset()
{
	impl->inputBuffer.clear();
	impl->remuxOutput.clear();
	impl->demuxer.clear();
}

#endif // ENABLE_MMT4K
