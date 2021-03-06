#include "Transport.hpp"
#include "FInfoManager.hpp"
#include <boost/thread/thread.hpp>
#include "tools.hpp"

using namespace std;
namespace pl = std::placeholders;

Transport::Transport(FInfoManager& info_manager)
	:native_(5),
	interval_(INTERVAL_INIT),
	is_recv_ack_(false),
	info_manager_(info_manager),
	ndriver_()
{
  //注册相关消息
  ndriver_.register_cmd_plugin(NetMSG::FINFO,
								[=](const char*data, size_t s){
								try {handle_info(info_from_net(data, s)); }
								catch (IllegalData&) {assert(!"IllegalData");}
								});
  ndriver_.register_cmd_plugin(NetMSG::BILL,
								[=](const char*data, size_t s){
								try {handle_bill(bill_from_net(data, s));}
								catch(IllegalData&) {assert(!"IllegalData");}
								});
  ndriver_.register_cmd_plugin(NetMSG::ACK,
								[=](const char*data, size_t s){
								try {handle_ckack(ckack_from_net(data, s));}
								catch(IllegalData&) {assert(!"IllegalData");}
								});
  ndriver_.register_cmd_plugin(NetMSG::SENDBEGIN,
								[=](const char*data, size_t s){
								try {handle_sb(sb_from_net(data, s));}
								catch(IllegalData&) {assert(!"IllegalData");}
								});
  ndriver_.register_cmd_plugin(NetMSG::SENDEND,
								[=](const char*data, size_t s){
								try {handle_se(se_from_net(data, s));}
								catch(IllegalData&) {assert(!"IllegalData");}
								});


  ndriver_.register_data_plugin([=](RecvBufPtr buf, size_t s){handle_chunk(buf, s);});

  //注册定时器以便统计速度
  ndriver_.start_timer(1, std::bind(&Transport::record_speed, this));
  ndriver_.start_timer(1, std::bind(&Transport::check_timeout, this));

  info_manager_.on_new_info.connect(std::bind(&Transport::cb_new_info, this, pl::_1));
  info_manager_.on_del_info.connect(bind(&Transport::cb_del_info, this, pl::_1));
}

void Transport::check_timeout()
{
  if (!is_recv_ack_) {
	interval_ = 5;
	FLog("timeout********set interval_=5");
  }

  for (auto& i : sendqueue_)
	i.second.timeout();

  vector<Hash> tmp;
  for (auto& i: recvqueue_) {
	  if (i.second.count() == 0)
		tmp.push_back(i.first);
	  else
		i.second.timeout();
  }
  for (auto& h: tmp)
	recvqueue_.erase(h);
}

void Transport::handle_info(const FInfo& info)
{
  try {
	  info_manager_.add_info(info);
	  FLog("%1% %2%") % "New Info" % info.path;
	  on_new_file(info);
  } catch (InfoExists&) {
	  FLog("FileInfo %1% exits!") % info.path;
  }
}

void Transport::sendqueue_new(const Hash& h)
{
  try {
	  FInfo info = info_manager_.find(h);
	  if (info.status == FInfo::Local) {
		  sendqueue_.insert(make_pair(h, SendHelper(*this, h, info.chunknum)));
		  sendqueue_.find(h)->second.receive_sb();
	  }
  } catch (InfoNotFound&) {
  }
}

void Transport::handle_ckack(const CKACK& ack)
{
  is_recv_ack_ = true;
  payload_tmp_.global += ack.payload;
  int loss = ack.loss;
  if (loss < 2) interval_ /= 2;
  else if (loss < 8) interval_ += 1;
  else if (loss < 16) interval_ += 2;
  else if (loss < 24) interval_ += 3;
  else  interval_ = 5;

  if (interval_ < 0) interval_ = 0;
  else if (interval_ > 5) interval_ = 5;

  FLog("interval:%1% loss:%2%") % interval_ % loss;
}

void Transport::handle_bill(const Bill& b)
{
  auto it = sendqueue_.find(b.hash);
  if (it != sendqueue_.end()) {
	  it->second.deal_bill(b);
  }
}

void Transport::handle_se(const Hash& b)
{
  auto it = recvqueue_.find(b);
  if (it != recvqueue_.end())
	it->second.receive_se();
}
void Transport::handle_sb(const Hash& b)
{
  auto it = sendqueue_.find(b);
  if (it != sendqueue_.end())
	it->second.receive_sb();
  else
	sendqueue_new(b);
}

void Transport::handle_chunk(RecvBufPtr buf, size_t s)
{
  //尝试转换数据为Chunk
  char* data = buf->data(); 
  Chunk c = chunk_from_net(data, s);

  auto it = recvqueue_.find(c.file_hash);
  if (it != recvqueue_.end()) {
	  if (it->second.ack(c.index)) {
		  assert(!it->second.ack(c.index));
		  //同时记录此文件有效获得一次文件块以便统计此文件的下载速度
		  payload_tmp_.files[c.file_hash]++;
		  payload_tmp_.global++;

		  function<void()> cb = [=]() {
			  // 用来使保存数据的shared_ptr不被销毁。
			  buf;
			  //检查是否已经完成
			  check_complete(c.file_hash);
		  };
		  write_chunk(c, cb);
	  } else {
		  FLog("******Alerady has Chunk:%1%*********") % c.index;
	  }
  }
}

void Transport::start_receive(const Hash& h)
{
  FInfo info = info_manager_.find(h);
  native_.new_file(info);

  recvqueue_.insert(make_pair(h, RecvHelper(*this, h, info.chunknum)));
  auto it = recvqueue_.find(h);
  it->second.begin_send();
}


/***************************发送函数**********************/
void Transport::send_bill(const Bill& b)
{
  FLog("Send Bill Region:%1% Bits:%2%") % b.region % bitset<BLOCK_LEN>(b.bits);
  ndriver_.cmd_send(bill_to_net(b));
}

void Transport::write_chunk(const Chunk& c, function<void()> cb)
{
  native_.async_write(c.file_hash, c.index*CHUNK_SIZE, c.data, c.size, cb);
}

void Transport::send_chunk(const Hash& fh, uint32_t index)
{
  FInfo info = info_manager_.find(fh);
  uint16_t size = (info.chunknum-1) == index ? info.lastchunksize : CHUNK_SIZE;
  char* data = native_.read(fh, index*CHUNK_SIZE);
  ndriver_.data_send(chunk_to_net(Chunk(fh, index, size, data)));
  // 默认延迟0ms
  boost::this_thread::sleep(boost::posix_time::milliseconds(interval_));
}

void Transport::send_sb(const Hash& fh)
{
  FLog("************send begin*****************");
  ndriver_.cmd_send(sb_to_net(fh));
}

void Transport::send_se(const Hash& fh)
{
  FLog("************send end*****************");
  ndriver_.cmd_send(se_to_net(fh));
}

void Transport::send_ckack(const CKACK& ack)
{
  FLog("************send ckack*****************");
  ndriver_.cmd_send(ckack_to_net(ack));
}


/**********************其他函数*****************/

//每隔一秒调用一次record_speed函数
void Transport::record_speed()
{
  int global = (payload_.global + payload_tmp_.global) / 2;
  std::map<Hash, int> files;
  //更新上一秒记录下的数据

  for (auto& f: payload_.files) {
	  files[f.first] = f.second;
  }
  for (auto& f: payload_tmp_.files) {
	  files[f.first] += f.second;
	  files[f.first] /=  2;
  }

  //清空payload_tmp_以便下次重新计算
  payload_ = Payload{global, files};
  payload_tmp_ = Payload();
}


void Transport::stop_receive(const Hash& h)
{
  //incomplete_.erase(h);
}


Payload Transport::payload()
{
  return payload_;
}

void Transport::check_complete(const Hash& h)
{
  FInfo info = info_manager_.find(h);
  auto& helper = recvqueue_.find(h)->second;

  uint32_t count = helper.count();

  if (count == 0) {
	  info_manager_.modify_status(h, FInfo::Local);
	  native_.close(h);
	  recvqueue_.erase(h);
  }
  //发送信号告知上层模块有新文件块已写入文件
  double progress = (info.chunknum-count) / double(info.chunknum);
  on_new_chunk(info.hash, progress);
}


string Transport::get_chunk_info(const Hash& h)
{
  return "1110001";
  /*
	 auto it = local_bill_.find(h);
	 if (it != local_bill_.end()) {
	 string r;
	 for (auto& i : it->second) {
	 r.append(i.to_string());
	 }
	 return r;
	 }
	 throw InfoNotFound();
	 */
}

void Transport::cb_new_info(const FInfo& f)
{
  if (f.status == FInfo::Local) {
	  //complete_.insert(f.hash);
	  native_.new_file(f);

	  //向网络发送此文件信息
	  ndriver_.cmd_send(info_to_net(f));
  }
}

void Transport::cb_del_info(const Hash& h)
{
  native_.close(h);
  sendqueue_.erase(h);
  recvqueue_.erase(h);
}

void Transport::run() 
{
  ndriver_.run(); 
}

void Transport::native_run() 
{
  native_.run();
}
