#ifndef HTTP_H
#define HTTP_H
#include <string>
#include <vector>
#include <map>
#include <cctype>
#include "socket.h"

namespace Net
{
std::string urlencode(const std::string &str)
{
	std::string s;
	for (std::string::const_iterator it = str.begin();it!=str.end();++it) {
		char c = *it;
		char h[8];
		if (c>='A' && c<='Z' || c>='a' && c<='z' || c>='0' && c<='9') {
			s+=c;
		} else {
			sprintf_s(h,"%%%02x",c);
			s+=h;
		}
	}
	return s;
}
std::string urldecode(const std::string &str)
{
	std::string s;
	for (std::string::const_iterator it = str.begin();it!=str.end();++it) {
		char c = *it;
		char h[8];
		if (c!='%') {
			s+=c;
		} else {
			// メモリ内で連続じゃ無いかも知れないのでコピー
			h[0]=*++it;
			h[1]=*++it;
			h[2]=0;
			int d;
			sscanf_s(h,"%02x",&d);
			s+=(char)d;
		}
	}
	return s;
}

class HttpResponse
{
public:
	int status;
	std::map<std::string, std::vector<std::string> > headers;
	std::string content;
	std::string getHeader(const std::string &name) {
		if (headers.count(name)) {
			return headers.find(name)->second[0];
		}
		return "";
	}
	void clear() {
		status = 0;
		headers.clear();
		content = "";
	}
};

// レスポンスボディ読み出し用のバッファ付きリーダ。
// ヘッダはSocket::readLine()で1バイトずつ読まれるためボディの先頭で構築でき、
// ここから先はまとめて受信してよい
class HttpBodyReader
{
	Socket &m_soc;
	std::string m_buf;
	size_t m_pos;

	// ソケットから追加でデータを読む。切断/タイムアウトならfalse
	bool fill() {
		char tmp[4096];
		int n = m_soc.recv(tmp, sizeof(tmp));
		if (n < 1) return false;
		// 消費済みの領域を捨ててから追記する
		if (m_pos > 0) {
			m_buf.erase(0, m_pos);
			m_pos = 0;
		}
		m_buf.append(tmp, n);
		return true;
	}

	static int hexval(char c) {
		if (c>='0' && c<='9') return c-'0';
		if (c>='a' && c<='f') return c-'a'+10;
		if (c>='A' && c<='F') return c-'A'+10;
		return -1;
	}

public:
	explicit HttpBodyReader(Socket &soc) : m_soc(soc), m_pos(0) {}

	// CRLF(またはLF)終端の1行を取り出す。行終端はoutに含めない
	bool readLine(std::string &out) {
		for (;;) {
			size_t nl = m_buf.find('\n', m_pos);
			if (nl != std::string::npos) {
				size_t end = nl;
				if (end > m_pos && m_buf[end-1]=='\r') end--;
				out.assign(m_buf, m_pos, end-m_pos);
				m_pos = nl+1;
				return true;
			}
			if (!fill()) return false;
		}
	}

	// 指定バイト数ちょうど読み出す。足りないまま切断されたらfalse
	bool readExact(size_t len, std::string &out) {
		while (m_buf.size()-m_pos < len) {
			if (!fill()) {
				out.append(m_buf, m_pos, std::string::npos);
				m_pos = m_buf.size();
				return false;
			}
		}
		out.append(m_buf, m_pos, len);
		m_pos += len;
		return true;
	}

	// 切断されるまで読み切る
	void readAll(std::string &out) {
		for (;;) {
			out.append(m_buf, m_pos, std::string::npos);
			m_pos = m_buf.size();
			if (!fill()) break;
		}
	}

	// chunked転送のボディをデコードして連結する。
	// 各チャンクは "<16進長>[;拡張]CRLF <データ> CRLF" で、長さ0が終端
	bool readChunked(size_t maxSize, std::string &out) {
		for (;;) {
			std::string line;
			if (!readLine(line)) return false;

			// チャンク拡張は読み捨て、前後の空白を落としてサイズだけ取り出す
			size_t semi = line.find(';');
			if (semi != std::string::npos) line.resize(semi);
			size_t b = line.find_first_not_of(" \t");
			if (b == std::string::npos) continue;	// 空行は読み飛ばす
			size_t e = line.find_last_not_of(" \t");
			line = line.substr(b, e-b+1);

			size_t size = 0;
			for (size_t i = 0; i < line.size(); i++) {
				int d = hexval(line[i]);
				if (d < 0) return false;			// 壊れたチャンク長
				if (size > maxSize/16) return false;	// 桁あふれ前に打ち切る
				size = size*16 + (size_t)d;
				if (size > maxSize) return false;	// 上限超過
			}
			if (size == 0) break;					// 終端チャンク
			if (out.size()+size > maxSize) return false;
			if (!readExact(size, out)) return false;

			std::string crlf;
			if (!readLine(crlf)) return false;		// データ直後のCRLF
		}
		return true;
	}
};

class HttpClient
{
public:
	enum METHOD {
		AUTO,
		GET,
		POST,
		HEAD,
	};
	METHOD method;

	// 接続待ち/送受信待ちのタイムアウト(ms)。0以下でOS既定のまま待ち続ける
	int connect_timeout_ms;
	int io_timeout_ms;
	// デコード後のボディの上限(壊れたchunkedでメモリを食い潰さないための保険)
	size_t max_content_size;

	// old
	int status;
	std::string body;
	std::map<std::string, std::vector<std::string> > headers;
	std::map<std::string,std::string> header;
	std::map<std::string,std::string> req_header;
	std::string cookie;

	HttpClient()
	{
		method = AUTO;
		connect_timeout_ms = 10000;
		io_timeout_ms = 30000;
		max_content_size = 64*1024*1024;
	}

	void clear()
	{
		body.clear();
		header.clear();
		headers.clear();
		method = AUTO;
		req_header.clear();
		cookie.clear();
	}

	Socket request(const std::string &host, int port,const std::string &path ,const std::string &data="")
	{
		status=0;
		header.clear();
		headers.clear();

		if (!req_header.count("Host")) req_header["Host"] = host;
		std::string methodstr;
		if (method==AUTO) {
			methodstr=data.size()?"POST":"GET";
		} else {
			methodstr=(method==POST)?"POST":"GET";
		}
		req_header["Connection"] = "close";

		Socket soc(host,(short)port,connect_timeout_ms,io_timeout_ms);
		if (method==GET && data.size()) {
			soc.write(methodstr+" "+path+"?"+data+" HTTP/1.1\r\n");
		} else {
			soc.write(methodstr+" "+path+" HTTP/1.1\r\n");
		}

		if (method==POST || method==AUTO&&data.size()) {
			char s[30];
			sprintf_s(s,"Content-Length: %zd\r\n", data.size());
			soc.write(s);
			soc.write("Content-Type: application/x-www-form-urlencoded\r\n");
		}
		if (cookie != "") {
			soc.write(std::string("Cookie: ")+cookie+"\r\n");
		}

		for (std::map<std::string,std::string>::iterator it=req_header.begin();it!=req_header.end();++it) {
			soc.write(it->first+": "+it->second+"\r\n");
		}
		soc.write("\r\n" );
		if (soc.error()) return soc;

		if (method!=GET && data.size()) {
			soc.write(data);
		}

		std::string line;
		line = soc.readLine(); // HTTP status
		size_t p=line.find(" ");
		if (p!=std::string::npos) {
			status = atoi(line.c_str()+p+1);
		}
		while(!soc.error()) {
			line = soc.readLine();
#ifdef CPPFL_DEBUG
			std::cout << line << std::endl;
#endif
			if (line.size()==0) break;
			if (line.size() && line[line.size()-1]=='\r') line.resize(line.size()-1);
			if (line.empty()) break;
			size_t p=line.find(":");
			std::string name=line.substr(0, p);
			if (line[p+1]==' ') p++;
			std::string value=line.substr(p+1);
			header[name]=value;
			headers[name].push_back(value);
			//cout << name << " : " << value << endl;
		}

		if (headers.count("Set-Cookie")) {
			std::string c = headers["Set-Cookie"][0];
			size_t p = c.find(";");
			if (p != std::string::npos) {
				c.resize(p);
			}
			cookie.swap(c);
		}

		return soc;
	}
	
	Socket request(const std::string &url, const std::string &data="")
	{
		using namespace std;
		int s=0;
		if (url.substr(0,7)=="http://") s=7;
		size_t p=url.find("/",s);
		string host = url.substr(s, p-s);
		string path = url.substr(p);
		int port=80;

		p=host.find(":");
		if (p!=string::npos) {
			port=atoi(host.substr(p+1).c_str());
			host = host.substr(0, p);
		}
		return request(host, port, path, data);
	}

	// old method
	size_t load(const std::string &url, const std::string &data="")
	{
		Socket soc = request(url, data);
		while(!soc.error()) {
			body += soc.read();
		}
		soc.close();
		return body.size();
	}

	// old method
	size_t load(const std::string &url, const std::map<std::string,std::string> &params)
	{
		std::string data;
		for (std::map<std::string,std::string>::const_iterator it = params.begin();it!=params.end();++it) {
			if (data!="") data+="&";
			data += (*it).first;
			data += '=';
			data+= urlencode((*it).second);
		}
		return load(url,data);
	}

	// ヘッダ名の大文字小文字を無視して値を取り出す
	// (HTTPのヘッダ名は大文字小文字を区別しないが、headersは受信したままの表記で持つ)
	static bool findHeader(const std::map<std::string, std::vector<std::string> > &headers,
	                       const std::string &name, std::string &value)
	{
		for (std::map<std::string, std::vector<std::string> >::const_iterator it = headers.begin();
		     it != headers.end(); ++it) {
			if (it->first.size() != name.size()) continue;
			bool same = true;
			for (size_t i = 0; i < name.size(); i++) {
				if (tolower((unsigned char)it->first[i]) != tolower((unsigned char)name[i])) {
					same = false;
					break;
				}
			}
			if (same && !it->second.empty()) {
				value = it->second[0];
				return true;
			}
		}
		return false;
	}

	// Content-Lengthの値を10進数として解釈する。数字以外が混ざっていたら不正とみなす
	static bool contentLength(const std::string &value, size_t &len)
	{
		size_t b = value.find_first_not_of(" \t");
		if (b == std::string::npos) return false;
		size_t e = value.find_last_not_of(" \t");

		// 32bitビルドのsize_tにも収まる範囲(4GB未満)だけを受け付ける。
		// これを超えるボディはどのみち呼び出し側の上限に引っかかる
		const unsigned long limit = 0xFFFFFFFFUL;
		unsigned long n = 0;
		for (size_t i = b; i <= e; i++) {
			if (!isdigit((unsigned char)value[i])) return false;
			if (n > (limit-9)/10) return false;	// 桁あふれ
			n = n*10 + (unsigned long)(value[i]-'0');
		}
		len = (size_t)n;
		return true;
	}

	// トークンが含まれるか(大文字小文字無視)
	static bool containsToken(const std::string &value, const std::string &token)
	{
		std::string lower;
		lower.reserve(value.size());
		for (size_t i = 0; i < value.size(); i++) {
			lower += (char)tolower((unsigned char)value[i]);
		}
		return lower.find(token) != std::string::npos;
	}

	int load(HttpResponse &res, const std::string &url, const std::string &postdata="")
	{
		Socket soc = request(url, postdata);
		res.status = status;
		res.content.clear();
		res.headers.swap(headers);

		if (!soc.error()) {
			HttpBodyReader reader(soc);
			std::string value;
			size_t len = 0;

			if (findHeader(res.headers, "Transfer-Encoding", value) && containsToken(value, "chunked")) {
				// chunkedのままでは制御行がボディに混ざるので必ずデコードする。
				// 途中で壊れていた場合は不完全なボディを返さない
				if (!reader.readChunked(max_content_size, res.content)) {
					res.content.clear();
					res.status = 0;
				}
			}
			else if (findHeader(res.headers, "Content-Length", value)
			         && contentLength(value, len)) {
				// 長さが分かるならそこで読み終える(サーバーがConnection: closeを
				// 無視してkeep-aliveのままでも切断待ちにならない)
				if (len > max_content_size) {
					res.status = 0;
				} else if (!reader.readExact(len, res.content)) {
					// 宣言された長さに満たないまま切断された
					res.content.clear();
					res.status = 0;
				}
			}
			else {
				// 長さが分からない場合はConnection: closeを期待して読み切る
				reader.readAll(res.content);
			}
		}

		soc.close();
		return res.status == 200;
	}

	int get(HttpResponse &res, const std::string &url, const std::map<std::string,std::string> &params = std::map<std::string,std::string>())
	{
		std::string data = url;
		bool is_first = true;
		for (std::map<std::string,std::string>::const_iterator it = params.begin();it!=params.end();++it) {
			data+= is_first?"?":"&";
			is_first = false;
			data += (*it).first;
			data += '=';
			data += urlencode((*it).second);
		}
		return load(res, data);
	}

	int post(HttpResponse &res, const std::string &url, const std::map<std::string,std::string> &params)
	{
		std::string data;
		for (std::map<std::string,std::string>::const_iterator it = params.begin();it!=params.end();++it) {
			if (data!="") data+="&";
			data += (*it).first;
			data += '=';
			data+= urlencode((*it).second);
		}
		return load(res, url, data);
	}

	HttpResponse get(const std::string &url, const std::map<std::string,std::string> &params = std::map<std::string,std::string>())
	{
		HttpResponse res;
		get(res, url, params);
		return res;
	}

	HttpResponse post(const std::string &url, const std::map<std::string,std::string> &params = std::map<std::string,std::string>())
	{
		HttpResponse res;
		post(res, url, params);
		return res;
	}

	std::string get_content(const std::string &url) {
		HttpResponse res;
		get(res, url);
		return res.content;
	}


};

}
#endif
