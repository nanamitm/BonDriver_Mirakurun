#ifdef ENABLE_MMT4K
#include "mmt4kConverter.h"

#include <iostream>

#include <windows.h>

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

// ACAS復号に使うwinscard.dllの既定パス(System32の本物)を絶対パスで返す。
// dantto4kはcustomWinscardDLLが空だとLoadLibraryA("winscard.dll")を呼ぶが、
// その解決には通常のDLL検索順が使われるため、ホストアプリ(TVTest等)の実行ファイルが
// 置かれたフォルダにwinscard.dllの差し替え版があるとそちらが先に読み込まれてしまう。
// 差し替え版DLLをホストアプリと共有すると、双方が独立にSCardConnect/SCardDisconnectを
// 行った結果DLL内部のグローバル状態を壊し合い、BonDriverの解放時(=別BonDriverへの
// 切り替え時)にホストアプリ側がwinscard.dll内でアクセス違反を起こす。
// ACASカードは実カードリーダーで読む必要があり差し替え版を使う理由もないため、
// 既定ではSystem32のものを明示的に指す。
std::string GetDefaultWinscardPath()
{
	char szSystemDir[MAX_PATH];
	const UINT uLen = ::GetSystemDirectoryA(szSystemDir, MAX_PATH);

	if (uLen == 0 || uLen >= MAX_PATH) {
		// 取得できない場合は従来通りDLL検索順に委ねる
		return std::string("winscard.dll");
	}

	std::string path(szSystemDir, uLen);
	if (path.back() != '\\') {
		path += '\\';
	}
	path += "winscard.dll";

	return path;
}

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

bool Mmt4kConverter::Init(const std::string& smartCardReaderName, const std::string& casProxyServer, const std::string& customWinscardDLL, bool convertResolutionGaiji, bool useSmartCard)
{
	config.smartCardReaderName = smartCardReaderName;
	config.casProxyServer = casProxyServer;
	config.customWinscardDLL = customWinscardDLL.empty() ? GetDefaultWinscardPath() : customWinscardDLL;
	config.convertResolutionGaiji = convertResolutionGaiji;

	if (!useSmartCard) {
		// カードリーダーを使わない設定。CASハンドラを付けないだけだと、スクランブル
		// フラグの立ったパケットは来ない鍵を待ち続けて何も出力されなくなるので、
		// ペイロードを平文として扱うよう指示する(復号済みのstreamでフラグだけが
		// 残っている場合も同じ扱いで通る)。winscard.dllはLoadLibraryもしない。
		impl->demuxer.setAssumeDescrambled(true);
		return true;
	}

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
