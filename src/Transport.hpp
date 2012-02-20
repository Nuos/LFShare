#ifndef _TRANSPOR_HPP__
#define _TRANSPOR_HPP__
#include <string>
#include <map>
#include <set>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/dynamic_bitset.hpp>
#include <boost/signals2/signal.hpp>
#include "NetDriver.hpp"
#include "FInfo.hpp"

typedef std::string Hash;
class FInfoManager;
class Bill;
class Chunk;

struct Payload {
	int global;
	std::map<Hash, int> files;
};

class NativeFileManager {
public:
	NativeFileManager(int n): max_num_(n) {}
	void new_file(const FInfo&);
	void write(const Hash& h, long begin, const char* data, size_t s);
	void read(const Hash& h, long begin, char* data, size_t s);
private:
	void set_current_file(const Hash& h);
	void push_hot(const Hash& h);
	std::list<std::pair<Hash, boost::iostreams::mapped_file> > hot_;
	std::map<Hash, FInfo> files_;
	char* current_;
	int max_num_;
};

class Transport {
public:
  	Transport(FInfoManager& info_manager);

	void start_receive(const Hash& file_hash);
	void stop_receive(const Hash& file_hash);


	void run();
	Payload payload();


	boost::signals2::signal<void(const FInfo&)> on_new_file;
	boost::signals2::signal<void(const Hash&,double)> on_new_chunk;

	void cb_new_info(const FInfo& h);
	void cb_del_info(const Hash& h);

private:
	void send_chunks();
	void send_bill();

	void handle_chunk(const Chunk& c);
	void handle_bill(const Bill& b);
	void handle_info(const FInfo& info);
	void record_speed();

	void check_complete(const Hash& h);

	//用来保存上一秒的结果
	Payload payload_;
	//用来统计当前这一秒的信息
	Payload payload_tmp_;

	//记录整个网络中缺少的且本地拥有的文件块
	std::map<Hash, boost::dynamic_bitset<> > global_bill_;

	//记录正在下载的文件所缺文件块
	std::map<Hash, boost::dynamic_bitset<> > local_bill_;

	std::set<Hash> complete_;
	std::set<Hash> incomplete_;

	NativeFileManager native_;
	FInfoManager& info_manager_;
	NetDriver ndriver_;
};

#endif
