#pragma once
// Wraps dantto4k's MMT/TLV demuxer + ACAS decrypt + MPEG2-TS remuxer (reused in-process
// via the thirdparty/dantto4k git submodule) so BS4K-type channels can be converted to
// plain TS before being handed back through IBonDriver2::GetTsStream. Only available on
// x64 builds (TSDuck static libs are only built for x64); see BonDriver_Mirakurun.vcxproj.
#ifdef ENABLE_MMT4K

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class Mmt4kConverter
{
public:
	Mmt4kConverter();
	~Mmt4kConverter();

	// smartCardReaderName/casProxyServer/customWinscardDLL mirror dantto4k.ini's [acas]
	// section. casProxyServer empty => use a local PC/SC reader; otherwise "host:port"
	// of a CasProxyServer (see https://github.com/nekohkr/casproxyserver).
	bool Init(const std::string& smartCardReaderName, const std::string& casProxyServer, const std::string& customWinscardDLL);

	// Feed raw MMT/TLV bytes received over HTTP. Converted MPEG2-TS bytes accumulate
	// internally; drain them with TakeOutput().
	void Push(const uint8_t* data, size_t size);

	// Returns (and clears) any MPEG2-TS bytes converted so far. May be empty if not
	// enough MMT/TLV data has arrived yet to emit a full unit.
	std::vector<uint8_t> TakeOutput();

	// Resets demuxer/remuxer/input state. Call on channel change.
	void Reset();

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

#endif // ENABLE_MMT4K
